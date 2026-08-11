/*
 * QuickView Mini TIFF Decoder - Core implementation
 * Copyright (C) 2026-Present QuickView Contributors
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "MiniTiff.h"
#include "ImageLoaderSimd.h"
#include <cstring>
#include <vector>
#include <span>
#include <omp.h>
#include <zlib.h>
#include <algorithm>

namespace QuickView::MiniTiff {

struct TiffImageDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t compression = 1;
    uint16_t photometric = 2;
    uint16_t samples = 1;
    uint16_t bitsPerSample = 8;
    uint16_t planarConfig = 1;
    uint16_t predictor = 1;
    uint16_t orientation = 1;
    uint16_t extraSamples = 0;
    uint16_t newSubfileType = 0;
    uint32_t rowsPerStrip = 0;
    bool isTiled = false;
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    bool isLE = true;

    std::vector<uint64_t> offsets;
    std::vector<uint64_t> byteCounts;
    std::span<const uint8_t> iccProfile;
    std::vector<uint16_t> colorMap;
};

// PackBits decoder implementation
static bool DecompressPackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t srcIdx = 0;
    size_t dstIdx = 0;
    while (srcIdx < srcLen && dstIdx < dstLen) {
        int8_t n = static_cast<int8_t>(src[srcIdx++]);
        if (n >= 0) {
            size_t count = static_cast<size_t>(n) + 1;
            if (srcIdx + count > srcLen || dstIdx + count > dstLen) {
                return false;
            }
            std::memcpy(dst + dstIdx, src + srcIdx, count);
            srcIdx += count;
            dstIdx += count;
        } else if (n != -128) {
            size_t count = static_cast<size_t>(-n) + 1;
            if (srcIdx >= srcLen || dstIdx + count > dstLen) {
                return false;
            }
            uint8_t val = src[srcIdx++];
            std::memset(dst + dstIdx, val, count);
            dstIdx += count;
        }
    }
    return dstIdx == dstLen;
}

static bool DecompressDeflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcLen);
    strm.next_out = dst;
    strm.avail_out = static_cast<uInt>(dstLen);

    // Try standard zlib wrapper (15)
    int ret = inflateInit2(&strm, 15);
    if (ret == Z_OK) {
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret == Z_STREAM_END && strm.total_out == dstLen) {
            return true;
        }
    }

    // Try raw deflate retry (-15)
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcLen);
    strm.next_out = dst;
    strm.avail_out = static_cast<uInt>(dstLen);

    ret = inflateInit2(&strm, -15);
    if (ret == Z_OK) {
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret == Z_STREAM_END && strm.total_out == dstLen) {
            return true;
        }
    }

    return false;
}

static Status ParseTiffIFD(const uint8_t* data, size_t size, TiffImageDesc& desc,
                           uint32_t ifdOffsetArg = 0) {
    if (size < 8) return Status::NotTiff;

    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00) {
        isLE = true;
    } else if (data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A) {
        isLE = false;
    } else {
        return Status::NotTiff;
    }
    desc.isLE = isLE;

    auto read16 = [&](size_t off) -> uint16_t {
        if (off + 2 > size) return 0;
        if (isLE) {
            return data[off] | (static_cast<uint16_t>(data[off + 1]) << 8);
        } else {
            return (static_cast<uint16_t>(data[off]) << 8) | data[off + 1];
        }
    };

    auto read32 = [&](size_t off) -> uint32_t {
        if (off + 4 > size) return 0;
        if (isLE) {
            return data[off] | (static_cast<uint32_t>(data[off + 1]) << 8) |
                   (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
        } else {
            return (static_cast<uint32_t>(data[off]) << 24) | (static_cast<uint32_t>(data[off + 1]) << 16) |
                   (static_cast<uint32_t>(data[off + 2]) << 8) | data[off + 3];
        }
    };

    auto getTagValue = [&](size_t entryOff, uint16_t type, uint32_t count) -> uint32_t {
        if (count != 1) return 0;
        if (type == 3) {
            return read16(entryOff + 8);
        } else if (type == 4) {
            return read32(entryOff + 8);
        } else if (type == 1) {
            return data[entryOff + 8];
        }
        return 0;
    };

    uint32_t ifdOffset = (ifdOffsetArg == 0) ? read32(4) : ifdOffsetArg;
    if (ifdOffset == 0 || ifdOffset + 2 > size) {
        return Status::Corrupt;
    }

    uint16_t numEntries = read16(ifdOffset);
    size_t current = ifdOffset + 2;
    if (current + numEntries * 12 > size) {
        return Status::Corrupt;
    }

    uint32_t stripOffsetsOffset = 0;
    uint32_t stripOffsetsCount = 0;
    uint16_t stripOffsetsType = 0;

    uint32_t stripByteCountsOffset = 0;
    uint32_t stripByteCountsCount = 0;
    uint16_t stripByteCountsType = 0;

    uint32_t tileOffsetsOffset = 0;
    uint32_t tileOffsetsCount = 0;
    uint16_t tileOffsetsType = 0;

    uint32_t tileByteCountsOffset = 0;
    uint32_t tileByteCountsCount = 0;
    uint16_t tileByteCountsType = 0;

    for (uint16_t i = 0; i < numEntries; ++i) {
        size_t entryOff = current + i * 12;
        uint16_t tag = read16(entryOff);
        uint16_t type = read16(entryOff + 2);
        uint32_t count = read32(entryOff + 4);

        switch (tag) {
            case 254: // NewSubfileType (bit0=reduced-res, bit1=thumbnail)
                desc.newSubfileType = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 256: // ImageWidth
                desc.width = getTagValue(entryOff, type, count);
                break;
            case 257: // ImageHeight
                desc.height = getTagValue(entryOff, type, count);
                break;
            case 258: // BitsPerSample
                if (count == 1) {
                    desc.bitsPerSample = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                } else if (count > 1) {
                    uint32_t valOrOff = read32(entryOff + 8);
                    if (type == 3) {
                        if (valOrOff + count * 2 > size) return Status::Corrupt;
                        desc.bitsPerSample = read16(valOrOff);
                        for (uint32_t j = 1; j < count; ++j) {
                            if (read16(valOrOff + j * 2) != desc.bitsPerSample) {
                                return Status::Unsupported; // Inconsistent bit depths
                            }
                        }
                    } else {
                        return Status::Unsupported;
                    }
                }
                break;
            case 259: // Compression
                desc.compression = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 262: // PhotometricInterpretation
                desc.photometric = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 273: // StripOffsets
                stripOffsetsOffset = read32(entryOff + 8);
                stripOffsetsCount = count;
                stripOffsetsType = type;
                break;
            case 274: // Orientation
                desc.orientation = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 277: // SamplesPerPixel
                desc.samples = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 278: // RowsPerStrip
                desc.rowsPerStrip = getTagValue(entryOff, type, count);
                break;
            case 279: // StripByteCounts
                stripByteCountsOffset = read32(entryOff + 8);
                stripByteCountsCount = count;
                stripByteCountsType = type;
                break;
            case 284: // PlanarConfiguration
                desc.planarConfig = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 317: // Predictor
                desc.predictor = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 320: // ColorMap
                if (type == 3) {
                    uint32_t valOrOff = read32(entryOff + 8);
                    if (valOrOff + count * 2 > size) return Status::Corrupt;
                    desc.colorMap.resize(count);
                    for (uint32_t j = 0; j < count; ++j) {
                        desc.colorMap[j] = read16(valOrOff + j * 2);
                    }
                }
                break;
            case 322: // TileWidth
                desc.isTiled = true;
                desc.tileWidth = getTagValue(entryOff, type, count);
                break;
            case 323: // TileLength
                desc.tileHeight = getTagValue(entryOff, type, count);
                break;
            case 324: // TileOffsets
                tileOffsetsOffset = read32(entryOff + 8);
                tileOffsetsCount = count;
                tileOffsetsType = type;
                break;
            case 325: // TileByteCounts
                tileByteCountsOffset = read32(entryOff + 8);
                tileByteCountsCount = count;
                tileByteCountsType = type;
                break;
            case 338: // ExtraSamples
                desc.extraSamples = static_cast<uint16_t>(getTagValue(entryOff, type, count));
                break;
            case 34675: // ICCProfile
                if (type == 7 || type == 1) {
                    uint32_t valOrOff = read32(entryOff + 8);
                    if (valOrOff + count <= size) {
                        desc.iccProfile = std::span<const uint8_t>(data + valOrOff, count);
                    }
                }
                break;
        }
    }

    // Populate offsets and byteCounts
    if (desc.isTiled) {
        if (tileOffsetsCount == 0 || tileOffsetsCount != tileByteCountsCount) {
            return Status::Corrupt;
        }
        desc.offsets.resize(tileOffsetsCount);
        desc.byteCounts.resize(tileByteCountsCount);

        if (tileOffsetsCount == 1) {
            desc.offsets[0] = tileOffsetsOffset;
            desc.byteCounts[0] = tileByteCountsOffset;
        } else {
            if (tileOffsetsType == 3) {
                if (tileOffsetsOffset + tileOffsetsCount * 2 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < tileOffsetsCount; ++j) {
                    desc.offsets[j] = read16(tileOffsetsOffset + j * 2);
                }
            } else if (tileOffsetsType == 4) {
                if (tileOffsetsOffset + tileOffsetsCount * 4 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < tileOffsetsCount; ++j) {
                    desc.offsets[j] = read32(tileOffsetsOffset + j * 4);
                }
            } else {
                return Status::Unsupported;
            }

            if (tileByteCountsType == 3) {
                if (tileByteCountsOffset + tileByteCountsCount * 2 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < tileByteCountsCount; ++j) {
                    desc.byteCounts[j] = read16(tileByteCountsOffset + j * 2);
                }
            } else if (tileByteCountsType == 4) {
                if (tileByteCountsOffset + tileByteCountsCount * 4 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < tileByteCountsCount; ++j) {
                    desc.byteCounts[j] = read32(tileByteCountsOffset + j * 4);
                }
            } else {
                return Status::Unsupported;
            }
        }
    } else {
        if (stripOffsetsCount == 0 || stripOffsetsCount != stripByteCountsCount) {
            return Status::Corrupt;
        }
        desc.offsets.resize(stripOffsetsCount);
        desc.byteCounts.resize(stripByteCountsCount);

        if (stripOffsetsCount == 1) {
            desc.offsets[0] = stripOffsetsOffset;
            desc.byteCounts[0] = stripByteCountsOffset;
        } else {
            if (stripOffsetsType == 3) {
                if (stripOffsetsOffset + stripOffsetsCount * 2 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < stripOffsetsCount; ++j) {
                    desc.offsets[j] = read16(stripOffsetsOffset + j * 2);
                }
            } else if (stripOffsetsType == 4) {
                if (stripOffsetsOffset + stripOffsetsCount * 4 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < stripOffsetsCount; ++j) {
                    desc.offsets[j] = read32(stripOffsetsOffset + j * 4);
                }
            } else {
                return Status::Unsupported;
            }

            if (stripByteCountsType == 3) {
                if (stripByteCountsOffset + stripByteCountsCount * 2 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < stripByteCountsCount; ++j) {
                    desc.byteCounts[j] = read16(stripByteCountsOffset + j * 2);
                }
            } else if (stripByteCountsType == 4) {
                if (stripByteCountsOffset + stripByteCountsCount * 4 > size) return Status::Corrupt;
                for (uint32_t j = 0; j < stripByteCountsCount; ++j) {
                    desc.byteCounts[j] = read32(stripByteCountsOffset + j * 4);
                }
            } else {
                return Status::Unsupported;
            }
        }
    }

    return Status::Ok;
}

static Status CapabilityGate(const TiffImageDesc& desc) {
    if (desc.width == 0 || desc.height == 0) return Status::Corrupt;

    // Phase 1 supported compressions: 1 (Uncompressed), 32773 (PackBits), 5 (LZW), 8 (Deflate), 32946 (Deflate)
    if (desc.compression != 1 && desc.compression != 32773 &&
        desc.compression != 5 && desc.compression != 8 && desc.compression != 32946) {
        return Status::Unsupported;
    }

    // 8-bit and 16-bit supported (16-bit will be downsampled to 8-bit)
    if (desc.bitsPerSample != 8 && desc.bitsPerSample != 16) {
        return Status::Unsupported;
    }

    // Photometrics supported: 0 (WhiteIsZero), 1 (BlackIsZero), 2 (RGB), 3 (Palette), 5 (CMYK)
    if (desc.photometric > 5 || desc.photometric == 4) {
        return Status::Unsupported;
    }

    // PlanarConfiguration: only contiguous (1) supported
    if (desc.planarConfig != 1) {
        return Status::Unsupported;
    }

    // SamplesPerPixel checks
    if (desc.photometric == 2 && desc.samples < 3) return Status::Corrupt;
    if (desc.photometric == 5 && desc.samples < 4) return Status::Corrupt;
    if (desc.photometric != 2 && desc.photometric != 5 && desc.samples < 1) return Status::Corrupt;

    // Predictor: supports None (1) or Horizontal (2)
    if (desc.predictor != 1 && desc.predictor != 2) {
        return Status::Unsupported;
    }

    return Status::Ok;
}

HRESULT LoadRegion(const uint8_t* data, size_t size,
                   const QuickView::Codec::DecodeContext& ctx,
                   QuickView::Codec::DecodeResult& result,
                   int cropX, int cropY, int cropW, int cropH) {
    TiffImageDesc desc;
    Status status = ParseTiffIFD(data, size, desc);
    if (status != Status::Ok) {
        return (status == Status::NotTiff) ? E_NOTIMPL : E_FAIL;
    }

    status = CapabilityGate(desc);
    if (status != Status::Ok) {
        return (status == Status::Unsupported) ? E_NOTIMPL : E_FAIL;
    }

    // Clamp crop parameters to valid boundaries
    cropX = (std::max)(0, cropX);
    cropY = (std::max)(0, cropY);
    cropW = (std::min)(cropW, static_cast<int>(desc.width) - cropX);
    cropH = (std::min)(cropH, static_cast<int>(desc.height) - cropY);
    if (cropW <= 0 || cropH <= 0) return E_INVALIDARG;

    uint32_t samples = desc.samples;
    uint32_t bytesPerSample = desc.bitsPerSample / 8;
    if (bytesPerSample == 0) bytesPerSample = 1;
    uint32_t highByteOffset = (bytesPerSample == 2) ? (desc.isLE ? 1 : 0) : 0;
    
    // Intermediate buffer for 16-to-8 bit SIMD conversion to avoid allocations in hot-path
    std::vector<uint8_t> rgb8Buf;
    if (bytesPerSample == 2 && samples == 3 && highByteOffset == 1) {
        rgb8Buf.resize(static_cast<size_t>(cropW) * 3);
    }

    // Prepare Output
    int outStride = ((cropW * 4) + 63) & ~63; // 64-byte aligned
    size_t totalBytes = static_cast<size_t>(outStride) * cropH;
    uint8_t* pixels = ctx.allocator(totalBytes);
    if (!pixels) {
        return E_OUTOFMEMORY;
    }
    std::memset(pixels, 0, totalBytes);


    uint32_t pixelStride = samples * bytesPerSample;
    uint32_t rowBytes = desc.width * pixelStride;
    uint16_t predictor = desc.predictor;
    uint16_t photometric = desc.photometric;

    bool decodeFailed = false;

    if (desc.isTiled) {
        uint32_t tilesPerRow = (desc.width + desc.tileWidth - 1) / desc.tileWidth;
        uint32_t tilesPerCol = (desc.height + desc.tileHeight - 1) / desc.tileHeight;
        uint32_t tilesCount = tilesPerRow * tilesPerCol;
        if (desc.offsets.size() < tilesCount || desc.byteCounts.size() < tilesCount) {
            return E_FAIL;
        }

        // Tiled parallel decoder
        [[maybe_unused]] bool useParallel = (tilesCount >= 4 && cropH >= 512);

        #pragma omp parallel for if(useParallel) shared(decodeFailed)
        for (int i = 0; i < static_cast<int>(tilesCount); ++i) {
            if (decodeFailed) continue;

                if (ctx.checkCancel && ctx.checkCancel()) {
                    decodeFailed = true;
                    continue;
                }

                // Locate Tile boundary
                int tileX = (i % tilesPerRow) * desc.tileWidth;
                int tileY = (i / tilesPerRow) * desc.tileHeight;
                int unitW = (std::min)(static_cast<int>(desc.tileWidth), static_cast<int>(desc.width) - tileX);
                int unitH = (std::min)(static_cast<int>(desc.tileHeight), static_cast<int>(desc.height) - tileY);

                // Compute Intersection with requested ROI
                int intersectX = (std::max)(cropX, tileX);
                int intersectEndX = (std::min)(cropX + cropW, tileX + unitW);
                int intersectY = (std::max)(cropY, tileY);
                int intersectEndY = (std::min)(cropY + cropH, tileY + unitH);

                if (intersectX >= intersectEndX || intersectY >= intersectEndY) {
                    continue; // No overlap with this tile, skip decoding!
                }

                uint64_t offset = desc.offsets[i];
                uint64_t byteCount = desc.byteCounts[i];
                if (offset + byteCount > size) {
                    decodeFailed = true;
                    continue;
                }

                size_t tileBytes = static_cast<size_t>(desc.tileWidth) * desc.tileHeight * samples;
                std::vector<uint8_t> localBuf;
                const uint8_t* tileData = nullptr;

                if (desc.compression == 1) {
                    if (offset + tileBytes > size) {
                        decodeFailed = true;
                        continue;
                    }
                    tileData = data + offset;
                } else if (desc.compression == 32773) {
                    localBuf.resize(tileBytes);
                    if (!DecompressPackBits(data + offset, byteCount, localBuf.data(), tileBytes)) {
                        decodeFailed = true;
                        continue;
                    }
                    tileData = localBuf.data();
                } else if (desc.compression == 5) {
                    localBuf.resize(tileBytes);
                    if (!DecompressLzw(data + offset, byteCount, localBuf.data(), tileBytes)) {
                        decodeFailed = true;
                        continue;
                    }
                    tileData = localBuf.data();
                } else if (desc.compression == 8 || desc.compression == 32946) {
                    localBuf.resize(tileBytes);
                    if (!DecompressDeflate(data + offset, byteCount, localBuf.data(), tileBytes)) {
                        decodeFailed = true;
                        continue;
                    }
                    tileData = localBuf.data();
                }

                if (!tileData) {
                    decodeFailed = true;
                    continue;
                }

                // Undo predictor
                std::vector<uint8_t> predictorBuf;
                if (predictor == 2) {
                    if (desc.compression == 1) {
                        predictorBuf.assign(tileData, tileData + tileBytes);
                        tileData = predictorBuf.data();
                    }
                    uint8_t* mutableData = const_cast<uint8_t*>(tileData);
                    for (int y = 0; y < (int)desc.tileHeight; ++y) {
                        uint8_t* rowData = mutableData + y * (desc.tileWidth * pixelStride);
                        for (int x = 1; x < (int)desc.tileWidth; ++x) {
                            for (uint32_t c = 0; c < pixelStride; ++c) {
                                rowData[x * pixelStride + c] += rowData[(x - 1) * pixelStride + c];
                            }
                        }
                    }
                }

                // Pack pixels falling into ROI
                for (int y = intersectY; y < intersectEndY; ++y) {
                    int localY = y - tileY;
                    int outY = y - cropY;
                    const uint8_t* srcRow = tileData + localY * (desc.tileWidth * pixelStride);
                    uint8_t* dstRow = pixels + outY * outStride;

                    if (photometric == 0 || photometric == 1) {
                        for (int x = intersectX; x < intersectEndX; ++x) {
                            int localX = x - tileX;
                            int outX = x - cropX;
                            uint8_t val = srcRow[localX * pixelStride + highByteOffset];
                            if (photometric == 0) val = 255 - val;
                            uint8_t a = (samples > 1) ? srcRow[localX * pixelStride + bytesPerSample + highByteOffset] : 255;
                            if (samples > 1 && desc.extraSamples != 1) {
                                val = static_cast<uint8_t>((val * a + 127) / 255);
                            }
                            dstRow[outX * 4 + 0] = val;
                            dstRow[outX * 4 + 1] = val;
                            dstRow[outX * 4 + 2] = val;
                            dstRow[outX * 4 + 3] = a;
                        }
                    } else if (photometric == 2) {
                        int runWidth = intersectEndX - intersectX;
                        // [Titan Perf] SIMD Accelerated 16-to-8 bit downsampling + BGRA Swizzle
                        // Only applicable when image is RGB (samples == 3) and Little-Endian (highByteOffset == 1)
                        if (bytesPerSample == 2 && samples == 3 && highByteOffset == 1) {
                            int localXStart = intersectX - tileX;
                            int outXStart = intersectX - cropX;
                            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow + localXStart * pixelStride);
                            uint8_t* dst32 = dstRow + outXStart * 4;
                            
                            ImageLoaderSimd::Pack16to8(src16, rgb8Buf.data(), runWidth * 3);
                            ImageLoaderSimd::ConvertRGBToBGRA(rgb8Buf.data(), dst32, runWidth, 1, runWidth * 4);
                        } else {
                            for (int x = intersectX; x < intersectEndX; ++x) {
                                int localX = x - tileX;
                                int outX = x - cropX;
                                uint8_t r = srcRow[localX * pixelStride + 0 * bytesPerSample + highByteOffset];
                                uint8_t g = srcRow[localX * pixelStride + 1 * bytesPerSample + highByteOffset];
                                uint8_t b = srcRow[localX * pixelStride + 2 * bytesPerSample + highByteOffset];
                                uint8_t a = (samples >= 4) ? srcRow[localX * pixelStride + 3 * bytesPerSample + highByteOffset] : 255;
                                if (samples >= 4 && desc.extraSamples != 1) {
                                    r = static_cast<uint8_t>((r * a + 127) / 255);
                                    g = static_cast<uint8_t>((g * a + 127) / 255);
                                    b = static_cast<uint8_t>((b * a + 127) / 255);
                                }
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = a;
                            }
                        }
                    } else if (photometric == 3) {
                        if (desc.colorMap.size() >= 768) {
                            for (int x = intersectX; x < intersectEndX; ++x) {
                                int localX = x - tileX;
                                int outX = x - cropX;
                                uint8_t idx = srcRow[localX * pixelStride + highByteOffset];
                                uint8_t r = static_cast<uint8_t>(desc.colorMap[idx] / 256);
                                uint8_t g = static_cast<uint8_t>(desc.colorMap[idx + 256] / 256);
                                uint8_t b = static_cast<uint8_t>(desc.colorMap[idx + 512] / 256);
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = 255;
                            }
                        } else {
                            decodeFailed = true;
                        }
} else if (photometric == 5) {
int localXStart = intersectX - tileX;
int outXStart = intersectX - cropX;
int runWidth = intersectEndX - intersectX;
bool cmykPremultiply = (desc.extraSamples != 1);
if (bytesPerSample == 1) {
ConvertCmykToBgra(srcRow + localXStart * pixelStride, dstRow + outXStart * 4, runWidth, samples, cmykPremultiply);
} else {
for (int dx = 0; dx < runWidth; ++dx) {
uint8_t c = srcRow[(localXStart + dx) * pixelStride + 0 * bytesPerSample + highByteOffset];
uint8_t m = srcRow[(localXStart + dx) * pixelStride + 1 * bytesPerSample + highByteOffset];
uint8_t ye = srcRow[(localXStart + dx) * pixelStride + 2 * bytesPerSample + highByteOffset];
uint8_t k = (samples >= 4) ? srcRow[(localXStart + dx) * pixelStride + 3 * bytesPerSample + highByteOffset] : 0;
uint8_t a = (samples >= 5) ? srcRow[(localXStart + dx) * pixelStride + 4 * bytesPerSample + highByteOffset] : 255;
uint8_t r = (255 - c) * (255 - k) / 255;
uint8_t g = (255 - m) * (255 - k) / 255;
uint8_t b = (255 - ye) * (255 - k) / 255;
if (samples >= 5 && cmykPremultiply) {
r = static_cast<uint8_t>((r * a + 127) / 255);
g = static_cast<uint8_t>((g * a + 127) / 255);
b = static_cast<uint8_t>((b * a + 127) / 255);
}
dstRow[(outXStart + dx) * 4 + 0] = b;
dstRow[(outXStart + dx) * 4 + 1] = g;
dstRow[(outXStart + dx) * 4 + 2] = r;
dstRow[(outXStart + dx) * 4 + 3] = a;
}
}
}
                }
            }
    } else {
        // Strip layout
        uint32_t rowsPerStrip = desc.rowsPerStrip;
        if (rowsPerStrip == 0 || rowsPerStrip > desc.height) {
            rowsPerStrip = desc.height;
        }

        uint32_t stripsCount = (desc.height + rowsPerStrip - 1) / rowsPerStrip;
        if (desc.offsets.size() < stripsCount || desc.byteCounts.size() < stripsCount) {
            return E_FAIL;
        }

        [[maybe_unused]] bool useParallel = (stripsCount >= 4 && cropH >= 512);
        int startStrip = cropY / rowsPerStrip;
        int endStrip = (cropY + cropH - 1) / rowsPerStrip;

        #pragma omp parallel for if(useParallel) shared(decodeFailed)
        for (int i = startStrip; i <= endStrip && i < static_cast<int>(stripsCount); ++i) {
            if (decodeFailed) continue;

                if (ctx.checkCancel && ctx.checkCancel()) {
                    decodeFailed = true;
                    continue;
                }

                int startY = i * rowsPerStrip;
                int unitH = (std::min)(static_cast<int>(rowsPerStrip), static_cast<int>(desc.height) - startY);

                // Y overlap check
                int intersectY = (std::max)(cropY, startY);
                int intersectEndY = (std::min)(cropY + cropH, startY + unitH);

                if (intersectY >= intersectEndY) {
                    continue; // Skip strip with no vertical intersection
                }

                uint64_t offset = desc.offsets[i];
                uint64_t byteCount = desc.byteCounts[i];
                if (offset + byteCount > size) {
                    decodeFailed = true;
                    continue;
                }

                size_t stripBytes = static_cast<size_t>(unitH) * rowBytes;
                std::vector<uint8_t> localBuf;
                const uint8_t* stripData = nullptr;

                if (desc.compression == 1) {
                    if (offset + stripBytes > size) {
                        decodeFailed = true;
                        continue;
                    }
                    stripData = data + offset;
                } else if (desc.compression == 32773) {
                    localBuf.resize(stripBytes);
                    if (!DecompressPackBits(data + offset, byteCount, localBuf.data(), stripBytes)) {
                        decodeFailed = true;
                        continue;
                    }
                    stripData = localBuf.data();
                } else if (desc.compression == 5) {
                    localBuf.resize(stripBytes);
                    if (!DecompressLzw(data + offset, byteCount, localBuf.data(), stripBytes)) {
                        decodeFailed = true;
                        continue;
                    }
                    stripData = localBuf.data();
                } else if (desc.compression == 8 || desc.compression == 32946) {
                    localBuf.resize(stripBytes);
                    if (!DecompressDeflate(data + offset, byteCount, localBuf.data(), stripBytes)) {
                        decodeFailed = true;
                        continue;
                    }
                    stripData = localBuf.data();
                }

                if (!stripData) {
                    decodeFailed = true;
                    continue;
                }

                // Undo predictor
                std::vector<uint8_t> predictorBuf;
                if (predictor == 2) {
                    if (desc.compression == 1) {
                        predictorBuf.assign(stripData, stripData + stripBytes);
                        stripData = predictorBuf.data();
                    }
                    uint8_t* mutableData = const_cast<uint8_t*>(stripData);
                    for (int y = 0; y < (int)unitH; ++y) {
                        uint8_t* rowData = mutableData + y * rowBytes;
                        for (int x = 1; x < (int)desc.width; ++x) {
                            for (uint32_t c = 0; c < pixelStride; ++c) {
                                rowData[x * pixelStride + c] += rowData[(x - 1) * pixelStride + c];
                            }
                        }
                    }
                }

                // Pack pixels falling into ROI horizontally
                for (int y = intersectY; y < intersectEndY; ++y) {
                    int localY = y - startY;
                    int outY = y - cropY;
                    const uint8_t* srcRow = stripData + localY * rowBytes;
                    uint8_t* dstRow = pixels + outY * outStride;

                    if (photometric == 0 || photometric == 1) {
                        for (int x = cropX; x < cropX + cropW; ++x) {
                            int outX = x - cropX;
                            uint8_t val = srcRow[x * pixelStride + highByteOffset];
                            if (photometric == 0) val = 255 - val;
                            uint8_t a = (samples > 1) ? srcRow[x * pixelStride + bytesPerSample + highByteOffset] : 255;
                            if (samples > 1 && desc.extraSamples != 1) {
                                val = static_cast<uint8_t>((val * a + 127) / 255);
                            }
                            dstRow[outX * 4 + 0] = val;
                            dstRow[outX * 4 + 1] = val;
                            dstRow[outX * 4 + 2] = val;
                            dstRow[outX * 4 + 3] = a;
                        }
                    } else if (photometric == 2) {
                        // [Titan Perf] SIMD Accelerated 16-to-8 bit downsampling + BGRA Swizzle for Stripped Layout
                        if (bytesPerSample == 2 && samples == 3 && highByteOffset == 1) {
                            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow + cropX * pixelStride);
                            ImageLoaderSimd::Pack16to8(src16, rgb8Buf.data(), cropW * 3);
                            ImageLoaderSimd::ConvertRGBToBGRA(rgb8Buf.data(), dstRow, cropW, 1, cropW * 4);
                        } else {
                            for (int x = cropX; x < cropX + cropW; ++x) {
                                int outX = x - cropX;
                                uint8_t r = srcRow[x * pixelStride + 0 * bytesPerSample + highByteOffset];
                                uint8_t g = srcRow[x * pixelStride + 1 * bytesPerSample + highByteOffset];
                                uint8_t b = srcRow[x * pixelStride + 2 * bytesPerSample + highByteOffset];
                                uint8_t a = (samples >= 4) ? srcRow[x * pixelStride + 3 * bytesPerSample + highByteOffset] : 255;
                                if (samples >= 4 && desc.extraSamples != 1) {
                                    r = static_cast<uint8_t>((r * a + 127) / 255);
                                    g = static_cast<uint8_t>((g * a + 127) / 255);
                                    b = static_cast<uint8_t>((b * a + 127) / 255);
                                }
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = a;
                            }
                        }
                    } else if (photometric == 3) {
                        if (desc.colorMap.size() >= 768) {
                            for (int x = cropX; x < cropX + cropW; ++x) {
                                int outX = x - cropX;
                                uint8_t idx = srcRow[x * pixelStride + highByteOffset];
                                uint8_t r = static_cast<uint8_t>(desc.colorMap[idx] / 256);
                                uint8_t g = static_cast<uint8_t>(desc.colorMap[idx + 256] / 256);
                                uint8_t b = static_cast<uint8_t>(desc.colorMap[idx + 512] / 256);
                                dstRow[outX * 4 + 0] = b;
                                dstRow[outX * 4 + 1] = g;
                                dstRow[outX * 4 + 2] = r;
                                dstRow[outX * 4 + 3] = 255;
                            }
                        } else {
                            decodeFailed = true;
                        }
} else if (photometric == 5) {
bool cmykPremultiply = (desc.extraSamples != 1);
if (bytesPerSample == 1) {
ConvertCmykToBgra(srcRow + cropX * pixelStride, dstRow, cropW, samples, cmykPremultiply);
} else {
for (int dx = 0; dx < cropW; ++dx) {
uint8_t c = srcRow[(cropX + dx) * pixelStride + 0 * bytesPerSample + highByteOffset];
uint8_t m = srcRow[(cropX + dx) * pixelStride + 1 * bytesPerSample + highByteOffset];
uint8_t ye = srcRow[(cropX + dx) * pixelStride + 2 * bytesPerSample + highByteOffset];
uint8_t k = (samples >= 4) ? srcRow[(cropX + dx) * pixelStride + 3 * bytesPerSample + highByteOffset] : 0;
uint8_t a = (samples >= 5) ? srcRow[(cropX + dx) * pixelStride + 4 * bytesPerSample + highByteOffset] : 255;
uint8_t r = (255 - c) * (255 - k) / 255;
uint8_t g = (255 - m) * (255 - k) / 255;
uint8_t b = (255 - ye) * (255 - k) / 255;
if (samples >= 5 && cmykPremultiply) {
r = static_cast<uint8_t>((r * a + 127) / 255);
g = static_cast<uint8_t>((g * a + 127) / 255);
b = static_cast<uint8_t>((b * a + 127) / 255);
}
dstRow[dx * 4 + 0] = b;
dstRow[dx * 4 + 1] = g;
dstRow[dx * 4 + 2] = r;
dstRow[dx * 4 + 3] = a;
}
}
                    }
                }
            }
    }

    if (decodeFailed) {
        return E_FAIL;
    }

    // Setup DecodeResult
    result.pixels = pixels;
    result.width = cropW;
    result.height = cropH;
    result.stride = outStride;
    result.format = PixelFormat::BGRA8888;
    result.success = true;

    // Setup Metadata
    result.metadata.Width = desc.width;
    result.metadata.Height = desc.height;
    result.metadata.Format = L"TIFF";

    // Alpha detection: extra samples beyond what photometric requires
    if (photometric == 2) // RGB needs 3; 4+ means alpha
      result.metadata.hasAlpha = (desc.samples >= 4) || (desc.extraSamples > 0);
    else if (photometric == 0 || photometric == 1) // Grayscale needs 1; 2+ means alpha
      result.metadata.hasAlpha = (desc.samples >= 2) || (desc.extraSamples > 0);
    else if (photometric == 5) // CMYK needs 4; 5+ means alpha
      result.metadata.hasAlpha = (desc.samples >= 5) || (desc.extraSamples > 0);
    
    wchar_t details[128];
    const wchar_t* compStr = L"";
    if (desc.compression == 1) compStr = L"Uncompressed";
    else if (desc.compression == 32773) compStr = L"PackBits";
    else if (desc.compression == 5) compStr = L"LZW";
    else if (desc.compression == 8 || desc.compression == 32946) compStr = L"Deflate";

    const wchar_t* photoStr = L"";
    if (photometric == 0 || photometric == 1) photoStr = L"Grayscale";
    else if (photometric == 2) photoStr = L"RGB";
    else if (photometric == 3) photoStr = L"Palette";
    else if (photometric == 5) photoStr = L"CMYK";

    swprintf_s(details, L"8-bit %s %s Lossless", photoStr, compStr);
    result.metadata.FormatDetails = details;
    result.metadata.ExifOrientation = desc.orientation;
    result.metadata.LoaderName = L"MiniTIFF";

    if (!desc.iccProfile.empty() && desc.photometric != 5) {
        result.metadata.iccProfileData.assign(desc.iccProfile.begin(), desc.iccProfile.end());
        result.metadata.HasEmbeddedColorProfile = true;
    } else {
        result.metadata.HasEmbeddedColorProfile = false;
        result.metadata.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;
    }

    return S_OK;
}

// ---------------------------------------------------------------------------
// Reduced-resolution IFD selection (thumbnail/preview acceleration).
// Large TIFFs often embed a pyramid of reduced-resolution sub-IFDs or an
// explicit thumbnail subfile. For a small thumbnail we only need to decode the
// smallest IFD that is still >= the target size, instead of the
// full-resolution primary IFD. This turns a multi-hundred-megapixel decode
// into a tiny one.
// ---------------------------------------------------------------------------
struct IfdInfo {
    uint32_t off = 0;
    uint16_t entryCount = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint16_t newSubfileType = 0;
};

static bool TiffIsLE(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    if (data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00) return true;
    if (data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A) return false;
    return false;
}

// Parse width/height/newSubfileType and collect SubIFD offsets from one IFD.
static bool ParseIfdDims(const uint8_t* data, size_t size, bool le, uint32_t off,
                         IfdInfo& info, std::vector<uint32_t>& subOffsets) {
    auto rd16 = [&](size_t o) -> uint16_t {
        if (o + 2 > size) return 0;
        if (le) return static_cast<uint16_t>(data[o] | (data[o + 1] << 8));
        return static_cast<uint16_t>((data[o] << 8) | data[o + 1]);
    };
    auto rd32 = [&](size_t o) -> uint32_t {
        if (o + 4 > size) return 0;
        if (le) return data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (data[o + 3] << 24);
        return (data[o] << 24) | (data[o + 1] << 16) | (data[o + 2] << 8) | data[o + 3];
    };
    if (off + 2 > size) return false;
    uint16_t n = rd16(off);
    size_t cur = off + 2;
    if (cur + static_cast<size_t>(n) * 12u > size) return false;
    uint32_t subIfdsOff = 0, subIfdsCount = 0, subIfdsType = 0;
    for (uint16_t i = 0; i < n; ++i) {
        size_t e = cur + static_cast<size_t>(i) * 12u;
        uint16_t tag = rd16(e);
        uint16_t type = rd16(e + 2);
        uint32_t cnt = rd32(e + 4);
        switch (tag) {
            case 256: info.w = (type == 3) ? rd16(e + 8) : rd32(e + 8); break;
            case 257: info.h = (type == 3) ? rd16(e + 8) : rd32(e + 8); break;
            case 254: info.newSubfileType = (type == 3) ? rd16(e + 8) : rd32(e + 8); break;
            case 0x014A: // SubIFDs
                subIfdsOff = e + 8; subIfdsCount = cnt; subIfdsType = type; break;
            default: break;
        }
    }
    info.off = off;
    info.entryCount = n;
    if (subIfdsCount > 0 && subIfdsCount < 1024 && subIfdsType == 4) {
        uint32_t valOff = rd32(subIfdsOff);
        if (valOff + subIfdsCount * 4u <= size) {
            for (uint32_t i = 0; i < subIfdsCount; ++i)
                subOffsets.push_back(rd32(valOff + i * 4u));
        }
    }
    return true;
}

static std::vector<IfdInfo> EnumerateTiffIfds(const uint8_t* data, size_t size) {
    std::vector<IfdInfo> out;
    if (size < 8) return out;
    bool le = TiffIsLE(data, size);
    if (data[0] != 0x49 && data[0] != 0x4D) return out;
    auto rd32 = [&](size_t o) -> uint32_t {
        if (o + 4 > size) return 0;
        if (le) return data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (data[o + 3] << 24);
        return (data[o] << 24) | (data[o + 1] << 16) | (data[o + 2] << 8) | data[o + 3];
    };
    uint32_t off = rd32(4);
    std::vector<uint32_t> visited;
    int guard = 0;
    while (off != 0 && off + 2 <= size && guard++ < 256) {
        bool dup = false;
        for (uint32_t v : visited) if (v == off) { dup = true; break; }
        if (dup) break;
        visited.push_back(off);
        IfdInfo info;
        std::vector<uint32_t> subs;
        if (!ParseIfdDims(data, size, le, off, info, subs)) break;
        out.push_back(info);
        for (uint32_t s : subs) {
            IfdInfo sinfo;
            std::vector<uint32_t> dummy;
            if (ParseIfdDims(data, size, le, s, sinfo, dummy)) out.push_back(sinfo);
        }
        size_t nextPos = static_cast<size_t>(off) + 2 + static_cast<size_t>(info.entryCount) * 12u;
        if (nextPos + 4 > size) break;
        off = rd32(nextPos);
    }
    return out;
}

static const IfdInfo* SelectThumbIfd(const std::vector<IfdInfo>& ifds, uint32_t targetPx) {
    if (ifds.empty()) return nullptr;
    // 1) explicit thumbnail subfile (NewSubfileType bit1) if adequate
    for (auto& i : ifds)
        if ((i.newSubfileType & 0x1) && i.w >= targetPx && i.h >= targetPx) return &i;
    // 2) smallest IFD whose max dimension >= targetPx
    const IfdInfo* best = nullptr;
    uint32_t bestMax = UINT32_MAX;
    for (auto& i : ifds) {
        uint32_t m = std::max(i.w, i.h);
        if (m >= targetPx && m < bestMax) { best = &i; bestMax = m; }
    }
    if (best) return best;
    // 3) fallback: largest IFD (primary full-res) — caller decodes full res
    best = &ifds[0];
    uint32_t bm = 0;
    for (auto& i : ifds) {
        uint32_t m = std::max(i.w, i.h);
        if (m > bm) { best = &i; bm = m; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Uncompressed-preview fast path (thumbnail acceleration).
// For an UNCOMPRESSED, stripped, 8-bit, chunky, no-predictor TIFF (the common
// case for flat scanned/prepress artwork), the pixel bytes are already laid out
// raw in the file. A thumbnail only needs a strided subset of them, so instead
// of reading + decoding the full image and then downscaling, we seek straight
// to each sampled row and copy only the sampled pixels. This cuts I/O and
// memory by ~step^2 (often >1000x) and needs no decode loop / threading / GPU.
// Returns S_OK if it produced the thumbnail; otherwise the caller falls through
// to the normal LoadRegion decode (so we never regress to "no thumbnail").
// ---------------------------------------------------------------------------
static HRESULT LoadUncompressedPreview(const uint8_t* data, size_t size,
                                       const QuickView::Codec::DecodeContext& ctx,
                                       QuickView::Codec::DecodeResult& result,
                                       const TiffImageDesc& desc,
                                       uint32_t targetPx) {
    const uint32_t w = desc.width, h = desc.height;
    if (w == 0 || h == 0 || targetPx == 0) return E_FAIL;

    uint32_t step = (std::max)(w, h) / targetPx;
    if (step < 1) step = 1;
    // Already small enough: no win, let the normal decoder handle it.
    if (step == 1) return E_FAIL;

    uint32_t outW = (w + step - 1) / step;
    uint32_t outH = (h + step - 1) / step;

    const uint32_t samples = desc.samples;
    const uint32_t pixelStride = samples;            // bytesPerSample == 1
    const uint64_t rowBytes = static_cast<uint64_t>(w) * pixelStride;
    const uint32_t rowsPerStrip = (desc.rowsPerStrip == 0 || desc.rowsPerStrip > h)
                                      ? h : desc.rowsPerStrip;
    const uint64_t stripsCount = (h + rowsPerStrip - 1) / rowsPerStrip;
    if (desc.offsets.size() < stripsCount) return E_FAIL;

    // Validate every sampled row's source offset BEFORE allocating.
    for (uint32_t oy = 0; oy < outH; ++oy) {
        uint32_t sy = oy * step;
        if (sy >= h) break;
        uint32_t strip = sy / rowsPerStrip;
        uint64_t rowOff = desc.offsets[strip] + (sy - strip * rowsPerStrip) * rowBytes;
        if (rowOff + rowBytes > size) return E_FAIL;
    }

    int outStride = ((outW * 4) + 63) & ~63;          // 64-byte aligned, like LoadRegion
    size_t total = static_cast<size_t>(outStride) * outH;
    uint8_t* pixels = ctx.allocator(total);
    if (!pixels) return E_OUTOFMEMORY;
    std::memset(pixels, 0, total);

    const uint16_t photometric = desc.photometric;
    for (uint32_t oy = 0; oy < outH; ++oy) {
        uint32_t sy = oy * step;
        if (sy >= h) break;
        uint32_t strip = sy / rowsPerStrip;
        uint64_t rowOff = desc.offsets[strip] + (sy - strip * rowsPerStrip) * rowBytes;
        const uint8_t* srcRow = data + rowOff;
        uint8_t* dstRow = pixels + static_cast<size_t>(oy) * outStride;
        for (uint32_t ox = 0; ox < outW; ++ox) {
            uint32_t sx = ox * step;
            if (sx >= w) break;
            const uint8_t* sp = srcRow + static_cast<size_t>(sx) * pixelStride;
            uint8_t r, g, b, a = 255;
            if (photometric == 2) {                   // RGB / RGBA
                r = sp[0]; g = sp[1]; b = sp[2];
                a = (samples >= 4) ? sp[3] : 255;
            } else {                                   // Grayscale (0=white-is-0, 1=black-is-0)
                uint8_t v = sp[0];
                if (photometric == 0) v = static_cast<uint8_t>(255 - v);
                r = g = b = v;
            }
            int o = static_cast<int>(ox) * 4;
            dstRow[o + 0] = b;
            dstRow[o + 1] = g;
            dstRow[o + 2] = r;
            dstRow[o + 3] = a;
        }
    }

    result.pixels = pixels;
    result.width = outW;
    result.height = outH;
    result.stride = outStride;
    result.format = PixelFormat::BGRA8888;
    result.success = true;

    result.metadata.Width = w;
    result.metadata.Height = h;
    result.metadata.Format = L"TIFF";
    if (photometric == 2)
        result.metadata.hasAlpha = (samples >= 4) || (desc.extraSamples > 0);
    else if (photometric == 0 || photometric == 1)
        result.metadata.hasAlpha = (samples >= 2) || (desc.extraSamples > 0);
    result.metadata.ExifOrientation = desc.orientation;
    result.metadata.LoaderName = L"MiniTIFF";
    result.metadata.HasEmbeddedColorProfile = false;
    result.metadata.colorInfo.dataSpace = QuickView::PixelDataSpace::EncodedSdr;

    return S_OK;
}

// Choose the uncompressed fast path (preview only) or the full decode.
static HRESULT DecodePreviewOrFull(const uint8_t* data, size_t size,
                                   const QuickView::Codec::DecodeContext& ctx,
                                   QuickView::Codec::DecodeResult& result,
                                   const TiffImageDesc& desc,
                                   uint32_t targetPx) {
    if (ctx.forcePreview && desc.compression == 1 && !desc.isTiled &&
        desc.bitsPerSample == 8 && desc.planarConfig == 1 && desc.predictor == 1 &&
        (desc.photometric == 0 || desc.photometric == 1 || desc.photometric == 2) &&
        (desc.samples == 1 || desc.samples == 3 || desc.samples == 4)) {
        HRESULT hr = LoadUncompressedPreview(data, size, ctx, result, desc, targetPx);
        if (hr == S_OK) return hr;
    }
    return LoadRegion(data, size, ctx, result, 0, 0, desc.width, desc.height);
}

HRESULT Load(const uint8_t* data, size_t size,
             const QuickView::Codec::DecodeContext& ctx,
             QuickView::Codec::DecodeResult& result) {
    // Preview/thumbnail request: decode only a reduced-resolution IFD when the
    // file has one, instead of the full-resolution primary IFD.
    if (ctx.forcePreview) {
        std::vector<IfdInfo> ifds = EnumerateTiffIfds(data, size);
        uint32_t targetPx = (ctx.targetWidth > 0)
                                ? static_cast<uint32_t>(ctx.targetWidth)
                                : 256;
        const IfdInfo* pick = SelectThumbIfd(ifds, targetPx);
        if (pick) {
            TiffImageDesc desc;
            Status st = ParseTiffIFD(data, size, desc, pick->off);
            if (st == Status::Ok && CapabilityGate(desc) == Status::Ok)
                return DecodePreviewOrFull(data, size, ctx, result, desc, targetPx);
            // Chosen IFD unsupported (exotic bit depth): fall through to the
            // primary IFD below so we never regress to "no thumbnail".
        }
    }

    TiffImageDesc desc;
    Status status = ParseTiffIFD(data, size, desc);
    if (status != Status::Ok) {
        return (status == Status::NotTiff) ? E_NOTIMPL : E_FAIL;
    }

    status = CapabilityGate(desc);
    if (status != Status::Ok) {
        return (status == Status::Unsupported) ? E_NOTIMPL : E_FAIL;
    }

    return DecodePreviewOrFull(data, size, ctx, result, desc, 256);
}

} // namespace QuickView::MiniTiff
