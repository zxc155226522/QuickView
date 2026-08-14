#include "pch.h"
#include "PreviewExtractor.h"
#include <algorithm>
#include <cstring>
#include <zlib.h>

// Helper Macros
#define U16LE(p) (*(const uint16_t*)(p))
#define U32LE(p) (*(const uint32_t*)(p))
#define U16BE(p) (uint16_t)((p)[0]<<8 | (p)[1])
#define U32BE(p) (uint32_t)((p)[0]<<24 | (p)[1]<<16 | (p)[2]<<8 | (p)[3])

// --- RAW (TIFF-based) ---

bool PreviewExtractor::ExtractFromRAW(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 1024) return false;

    // TIFF Header check
    // II (49 49) = Little Endian
    // MM (4D 4D) = Big Endian
    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49) isLE = true;
    else if (data[0] == 0x4D && data[1] == 0x4D) isLE = false;
    else return false; // Not TIFF

    uint16_t fortyTwo = isLE ? U16LE(data + 2) : U16BE(data + 2);
    if (fortyTwo != 42 && fortyTwo != 0x55 && fortyTwo != 0x4F52) { // 42, Panasonic(85), Olympus(0x4F52)
        // Check for CR3? CR3 is ISOBMFF, not TIFF. 
        // If CR3, this check fails, handled by HEIC parser (similar structure) or specific CR3 logic.
        // But Canon CR2 is TIFF.
        // Let's assume TIFF standard for now.
    }
    
    uint32_t firstIFD = isLE ? U32LE(data + 4) : U32BE(data + 4);
    if (firstIFD >= size) return false;

    uint64_t jpgOff = 0, jpgSz = 0;
    
    // Parse IFD0
    // IFD0 usually contains the tiny thumbnail (160x120).
    // SubIFDs usually contain the full preview.
    // Sony ARW: Preview in SubIFD.
    // Canon CR2: Preview image tag in IFD0 or StripOffsets?
    
    // Strategy: Look for "JpgFromRawStart" (Tag 0x0201) or "PreviewImageStart"
    // Also check SubIFDs (Tag 0x014A)
    
    if (ParseTiffIFD(data, size, firstIFD, isLE, jpgOff, jpgSz)) {
        if (jpgOff > 0 && jpgSz > 1024 && jpgOff + jpgSz <= size) {
             out.pData = data + jpgOff;
             out.size = (size_t)jpgSz;
             return true;
        }
    }
    
    return false;
}

bool PreviewExtractor::ExtractFromTIFF(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 16) return false;

    bool isLE = false;
    if (data[0] == 0x49 && data[1] == 0x49) isLE = true;
    else if (data[0] == 0x4D && data[1] == 0x4D) isLE = false;
    else return false;

    uint16_t magic = isLE ? U16LE(data + 2) : U16BE(data + 2);
    if (magic != 42 && magic != 43) return false;

    uint32_t ifd0 = isLE ? U32LE(data + 4) : U32BE(data + 4);
    if (ifd0 >= size) return false;

    uint64_t jpgOff = 0;
    uint64_t jpgSz = 0;
    if (!ParseTiffIFD(data, size, ifd0, isLE, jpgOff, jpgSz)) {
        return false;
    }

    if (jpgOff > 0 && jpgSz > 512 && jpgOff + jpgSz <= size) {
        out.pData = data + jpgOff;
        out.size = static_cast<size_t>(jpgSz);
        return true;
    }

    return false;
}

bool PreviewExtractor::ParseTiffIFD(const uint8_t* data, size_t size, size_t offset, bool isLE, uint64_t& jpegOffset, uint64_t& jpegSize) {
    if (offset + 2 > size) return false;
    
    uint16_t count = isLE ? U16LE(data + offset) : U16BE(data + offset);
    size_t entrySize = 12;
    size_t current = offset + 2;
    
    if (current + count * entrySize > size) return false;

    uint64_t subIfdOffset = 0;
    
    for (int i = 0; i < count; i++) {
        const uint8_t* ev = data + current + i * entrySize;
        uint16_t tag = isLE ? U16LE(ev) : U16BE(ev);
        uint16_t type = isLE ? U16LE(ev + 2) : U16BE(ev + 2);
        uint32_t cnt = isLE ? U32LE(ev + 4) : U32BE(ev + 4);
        uint32_t valOrOff = isLE ? U32LE(ev + 8) : U32BE(ev + 8); // Value if fits 4 bytes, else offset
        
        // JPEGInterchangeFormat (0x0201) - Common in EXIF/TIFF, points to SOI
        if (tag == 0x0201) { 
            jpegOffset = valOrOff; 
        }
        // JPEGInterchangeFormatLength (0x0202)
        else if (tag == 0x0202) { 
            jpegSize = valOrOff; 
        }
        // StripOffsets (0x0111) - Sometimes used for thumb
        // StripByteCounts (0x0117)
        // SubIFDs (0x014A) - VERY IMPORTANT for RAW
        else if (tag == 0x014A) {
             // SubIFDs usually points to a list of offsets.
             // If type=4 (LONG), cnt=1, then valOrOff is offset.
             // If cnt > 1, valOrOff is offset to array of offsets.
             if (type == 4 || type == 3) { // LONG or SHORT
                 if (cnt == 1) subIfdOffset = valOrOff;
                 else subIfdOffset = valOrOff; // Offset to array
             }
        }
    }
    
    // If found valid JPEG in this IFD, check if it's "large enough" (> 50KB?)
    // Thumbnails are small. Previews are large.
    if (jpegOffset > 0 && jpegSize > 50000) return true;
    
    // Recurse into SubIFDs if current wasn't good
    if (jpegOffset == 0 && subIfdOffset > 0 && subIfdOffset < size) {
        // Assume subIfdOffset points to first SubIFD or array
        // For simplicity, just try to parse the offset itself as an IFD
        // Correct logic is complex (array reading).
        // Hack: Try to parse whatever is at subIfdOffset.
        // Usually it's an array of LONGs. Read first one.
        // If SubIFDS tag pointed to array, we need to read memory.
        // Let's assume it points to valid IFD.
        
        // Actually for ARW/DNG, SubIFD is main image or preview.
        // Let's implement full parsing later. 
        // For now, let's enable DNG/ARW basic detection.
    }
    
    return (jpegOffset > 0 && jpegSize > 0);
}


// --- HEIC (ISOBMFF) ---
uint32_t PreviewExtractor::ReadU32BE(const uint8_t* p) { return U32BE(p); }

bool PreviewExtractor::ExtractFromHEIC(const uint8_t* data, size_t size, ExtractedData& out) {
    // Simplified ISOBMFF Parser
    // Look for 'meta' box -> 'iloc' box -> item info
    // HEIC usually stores thumb as a separate item, or main image (grid)
    // Finding extracted JPEG (Exif thumb) inside 'meta' -> 'Exif' item?
    // Often HEIC contains a small AVC/HEVC thumbnail track.
    // BUT, we want JPEG.
    // Many camera HEIC embed Exif. Exif has JPEG thumb.
    
    // Quick Scan for Exif Marker?
    // Exif in ISOBMFF is wrapped in 'Exif' item.
    // Scanning for "Exif\0\0" (Standard Exif header) might work.
    
    if (size < 1024) return false;
    
    const uint8_t exifSig[] = { 0x45, 0x78, 0x69, 0x66, 0x00, 0x00 };
    // Brute force scan for Exif header (fast enough for memory mapped file)
    // Limit scan to first 1MB?
    size_t scanLimit = std::min(size, (size_t)512 * 1024);
    
    for (size_t i = 0; i < scanLimit - 6; i++) {
        if (std::memcmp(data + i, exifSig, 6) == 0) {
            // Found Exif Block
            // Parse Exif (TIFF format)
            // Exif header usually follows 49 49 or 4D 4D within 6 bytes? 
            // Standard: Exif\0\0 + TIFF Header
            size_t tiffStart = i + 6;
            if (tiffStart + 8 > size) continue;
            
            uint64_t jpgOff = 0, jpgSz = 0;
            bool isLE = (data[tiffStart] == 0x49);
            
            uint32_t ifd0 = isLE ? U32LE(data + tiffStart + 4) : U32BE(data + tiffStart + 4);
            
            if (ParseTiffIFD(data + tiffStart, size - tiffStart, ifd0, isLE, jpgOff, jpgSz)) {
                if (jpgOff > 0 && jpgSz > 0) {
                     out.pData = data + tiffStart + jpgOff;
                     out.size = (size_t)jpgSz;
                     return true;
                }
            }
        }
    }
    return false;
}

// --- PSD ---

bool PreviewExtractor::ExtractFromPSD(const uint8_t* data, size_t size, ExtractedData& out) {
    if (size < 26) return false;
    if (std::memcmp(data, "8BPS", 4) != 0) return false;
    
    // Header (26 bytes)
    // Channels (2 bytes), Height (4), Width (4), Depth (2), Mode (2)
    
    // Color Mode Data Section
    size_t offset = 26;
    uint32_t colorModeLen = U32BE(data + offset);
    offset += 4 + colorModeLen;
    if (offset >= size) return false;
    
    // Image Resources Section
    if (offset + 4 > size) return false;
    uint32_t imgResLen = U32BE(data + offset);
    size_t resEnd = offset + 4 + imgResLen;
    offset += 4;
    
    if (resEnd > size) return false;
    
    // Parse Resources
    while (offset < resEnd) {
        if (offset + 6 > resEnd) break;
        // Signature 8BIM
        if (std::memcmp(data + offset, "8BIM", 4) != 0) break;
        offset += 4;
        
        uint16_t id = U16BE(data + offset);
        offset += 2;
        
        // Name (Pascal String, padded to even)
        uint8_t nameLen = data[offset];
        offset += 1 + nameLen;
        if (offset % 2 != 0) offset++; // Pad
        
        if (offset + 4 > resEnd) break;
        uint32_t sizeData = U32BE(data + offset);
        offset += 4;
        
        if (offset + sizeData > resEnd) break;
        
        // ID 1033 (Thumbnail resource 1) or 1036 (Thumbnail resource 2 - preferred)
        if (id == 1036 || id == 1033) {
            // Found thumbnail resource!
            // Format: 
            // 4 bytes: Format (1 = kJpegRGB)
            // 4 bytes: Width
            // 4 bytes: Height
            // 4 bytes: WidthBytes
            // 4 bytes: TotalSize
            // 4 bytes: SizeCompressed
            // 2 bytes: BitsPerPixel
            // 2 bytes: Planes
            // Data...
            
            if (sizeData > 28) {
                uint32_t format = U32BE(data + offset);
                if (format == 1) { // JPEG
                    // JPEG data starts at offset + 28
                    out.pData = data + offset + 28;
                    out.size = sizeData - 28;
                    return true;
                }
            }
        }
        
        offset += sizeData;
        if (offset % 2 != 0) offset++; // Pad
    }
    
    return false;
}

// --- CDR / CMX (CorelDRAW embedded preview) ---

// Raw DEFLATE (no zlib header), as used by ZIP entries.
bool PreviewExtractor::InflateRaw(const uint8_t* src, size_t srcSize,
                                  std::vector<uint8_t>& outBuf, size_t hint) {
    z_stream strm{};
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
        return false;
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcSize);
    outBuf.clear();
    outBuf.reserve(hint ? hint : srcSize * 4);
    const size_t kChunk = 65536;
    int ret = Z_OK;
    do {
        outBuf.resize(outBuf.size() + kChunk);
        strm.next_out = outBuf.data() + outBuf.size() - kChunk;
        strm.avail_out = static_cast<uInt>(kChunk);
        ret = inflate(&strm, Z_FINISH);
    } while (ret == Z_OK || ret == Z_BUF_ERROR);
    outBuf.resize(strm.total_out);
    inflateEnd(&strm);
    // Only a fully decoded stream is usable; a truncated tail (Z_BUF_ERROR)
    // means the output buffer was exhausted and the image is incomplete.
    return ret == Z_STREAM_END && !outBuf.empty();
}

// Search an aligned BITMAPINFOHEADER (DIB) inside [start, end).
bool PreviewExtractor::FindDibInRange(const uint8_t* data, size_t start, size_t end,
                                      size_t& dibOff, size_t& pixOffInDib, size_t& dibSize) {
    for (size_t p = start; p + 40 <= end; p += 4) {
        uint32_t biSize = U32LE(data + p);
        if (biSize != 40 && biSize != 52 && biSize != 56 &&
            biSize != 60 && biSize != 108 && biSize != 124)
            continue;
        int32_t w = (int32_t)U32LE(data + p + 4);
        int32_t h = (int32_t)U32LE(data + p + 8);
        uint16_t planes = U16LE(data + p + 12);
        uint16_t bpp = U16LE(data + p + 14);
        if (w <= 0 || h == 0 || planes != 1)
            continue;
        if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 &&
            bpp != 24 && bpp != 32)
            continue;
        int absH = h < 0 ? -h : h;
        if (absH > 4096 || w > 4096)
            continue;
        uint32_t comp = U32LE(data + p + 16);
        uint32_t palBytes = 0;
        if (bpp <= 8)
            palBytes = (1u << bpp) * 4;
        else if ((bpp == 16 || bpp == 32) && comp == 3)
            palBytes = 12; // BI_BITFIELDS: 3 DWORD masks
        uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4;
        uint32_t imgSize = U32LE(data + p + 20);
        uint32_t pixBytes = imgSize != 0 ? imgSize : stride * (uint32_t)absH;
        uint32_t total = biSize + palBytes + pixBytes;
        if (p + total > end + 16) // +pad tolerance
            continue;
        dibOff = p;
        pixOffInDib = biSize + palBytes;
        dibSize = total;
        return true;
    }
    return false;
}

// Recursively collect preview-chunk data ranges from a RIFF container.
void PreviewExtractor::WalkRiffCollect(const uint8_t* data, size_t size, size_t off,
                                       size_t end, bool dispOnly,
                                       std::vector<std::pair<size_t, size_t>>& ranges) {
    while (off + 8 <= end && off + 8 <= size) {
        uint32_t csize = U32LE(data + off + 4);
        size_t dstart = off + 8;
        size_t dend = dstart + csize;
        if (dend > size) dend = size;
        bool isContainer = (std::memcmp(data + off, "RIFF", 4) == 0 ||
                            std::memcmp(data + off, "LIST", 4) == 0 ||
                            std::memcmp(data + off, "FORM", 4) == 0 ||
                            std::memcmp(data + off, "CAT ", 4) == 0);
        if (!isContainer) {
            bool hit = false;
            if (dispOnly)
                hit = std::memcmp(data + off, "DISP", 4) == 0;
            else
                hit = (std::memcmp(data + off, "DISP", 4) == 0 ||
                       std::memcmp(data + off, "PRE8", 4) == 0 ||
                       std::memcmp(data + off, "PRE9", 4) == 0 ||
                       std::memcmp(data + off, "PRE ", 4) == 0 ||
                       std::memcmp(data + off, "PRV8", 4) == 0 ||
                       std::memcmp(data + off, "PRV9", 4) == 0 ||
                       std::memcmp(data + off, "PRV ", 4) == 0 ||
                       std::memcmp(data + off, "THMB", 4) == 0 ||
                       std::memcmp(data + off, "PTI_", 4) == 0);
            if (hit)
                ranges.push_back({dstart, dend});
        }
        if (isContainer && csize >= 4)
            WalkRiffCollect(data, size, dstart + 4, dstart + csize, dispOnly, ranges);
        size_t step = csize + (csize & 1);
        off = dstart + step;
        if (csize == 0) break;
    }
}

bool PreviewExtractor::ExtractFromCDR(const uint8_t* data, size_t size, ExtractedData& out) {
    if (!data || size < 16) return false;

    // ---- RIFF (older CorelDRAW) ----
    if (std::memcmp(data, "RIFF", 4) == 0) {
        uint32_t formSize = U32LE(data + 4);
        size_t formEnd = 12 + formSize;
        if (formEnd > size) formEnd = size;
        std::vector<std::pair<size_t, size_t>> ranges;
        WalkRiffCollect(data, size, 12, formEnd, true, ranges); // prefer DISP
        if (ranges.empty())
            WalkRiffCollect(data, size, 12, formEnd, false, ranges);
        for (const auto& r : ranges) {
            size_t dibOff = 0, pixOff = 0, dibSize = 0;
            if (FindDibInRange(data, r.first, r.second, dibOff, pixOff, dibSize)) {
                out.buffer.resize(14 + dibSize);
                out.buffer[0] = 'B'; out.buffer[1] = 'M';
                uint32_t bfSize = (uint32_t)(14 + dibSize);
                uint32_t bfOffBits = (uint32_t)(14 + pixOff);
                uint32_t zero = 0;
                std::memcpy(&out.buffer[2], &bfSize, 4);
                std::memcpy(&out.buffer[6], &zero, 4);
                std::memcpy(&out.buffer[10], &bfOffBits, 4);
                std::memcpy(out.buffer.data() + 14, data + dibOff, dibSize);
                out.pData = out.buffer.data();
                out.size = out.buffer.size();
                out.isCopy = true;
                return true;
            }
        }
        return false;
    }

    // ---- ZIP (CorelDRAW X4+) ----
    if (std::memcmp(data, "PK\x03\x04", 4) != 0)
        return false;

    // Find EOCD by scanning backward from the tail.
    int64_t scanStart = (int64_t)size > 66000 ? (int64_t)size - 66000 : 0;
    int64_t eocd = -1;
    for (int64_t p = (int64_t)size - 4; p >= scanStart; --p) {
        if (U32LE(data + p) == 0x06054b50) {
            uint32_t cdOff = U32LE(data + p + 16);
            if (cdOff + 4 <= size && U32LE(data + cdOff) == 0x02014b50) {
                eocd = p;
                break;
            }
        }
    }
    if (eocd < 0) return false;

    uint32_t cdOff = U32LE(data + eocd + 16);
    uint32_t cdCount = U16LE(data + eocd + 10);
    if (cdOff + 46 > size) return false;

    // Score-based pick: prefer the smallest dedicated thumbnail over the
    // full-page preview (page1), and PNG over BMP/JPG. This keeps large CDs
    // cheap — we never decode the big page1.png just to make a thumbnail.
    struct Cand { uint32_t localOff; uint32_t compSize; uint32_t uncompSize; uint16_t method; int score; };
    Cand best{}; best.score = -1;
    Cand thumbCand{}; thumbCand.score = -1;   // [Fix] 显式记录首个 thumbnail 候选，确保优先

    size_t j = cdOff;
    for (uint32_t n = 0; n < cdCount && j + 46 <= size; ++n) {
        if (U32LE(data + j) != 0x02014b50) break;
        uint16_t method = U16LE(data + j + 10);
        uint32_t compSize = U32LE(data + j + 20);
        uint32_t uncompSize = U32LE(data + j + 24);
        uint16_t nameLen = U16LE(data + j + 28);
        uint16_t extraLen = U16LE(data + j + 30);
        uint16_t commLen = U16LE(data + j + 32);
        uint32_t localOff = U32LE(data + j + 42);

        if (j + 46 + nameLen > size) break;
        std::string name((const char*)data + j + 46, nameLen);
        std::string nl;
        nl.reserve(name.size());
        for (char c : name) nl += (char)::tolower((unsigned char)c);

        bool isMatch = false, isPng = false;
        if (nl.find("thumb") != std::string::npos ||
            nl.find("preview") != std::string::npos ||
            nl.find("page") != std::string::npos) {
            if (nl.size() >= 4 && nl.compare(nl.size() - 4, 4, ".png") == 0) {
                isMatch = true; isPng = true;
            } else if ((nl.size() >= 4 &&
                        (nl.compare(nl.size() - 4, 4, ".bmp") == 0 ||
                         nl.compare(nl.size() - 4, 4, ".jpg") == 0)) ||
                       (nl.size() >= 5 && nl.compare(nl.size() - 5, 5, ".jpeg") == 0)) {
                isMatch = true;
            }
        }
        if (isMatch) {
            int score = 0;
            if (isPng) score += 2;                                  // PNG 优先
            if (nl.find("thumb") != std::string::npos) score += 4;  // 缩略图 > 整页
            else if (nl.find("page") != std::string::npos) score += 1;
            // [Fix] thumbnail 显式候选：名称含 "thumbnail" 即记录（取体积最小者）
            if (nl.find("thumb") != std::string::npos &&
                (thumbCand.score < 0 || uncompSize < thumbCand.uncompSize)) {
                thumbCand = {localOff, compSize, uncompSize, method, score};
            }
            if (score > best.score ||
                (score == best.score && uncompSize < best.uncompSize)) {
                best = {localOff, compSize, uncompSize, method, score};
            }
        }
        j += 46 + nameLen + extraLen + commLen;
    }

    // [Fix] thumbnail 优先：命中则直接抽整图预览，跳过 page1 单页切片
    const Cand* pick = (thumbCand.score >= 0) ? &thumbCand
                      : ((best.score >= 0) ? &best : nullptr);
    if (!pick) return false;

    if (pick->localOff + 30 > size) return false;
    uint16_t lNameLen = U16LE(data + pick->localOff + 26);
    uint16_t lExtraLen = U16LE(data + pick->localOff + 28);
    size_t dataStart = pick->localOff + 30 + lNameLen + lExtraLen;
    if (dataStart > size || dataStart + pick->compSize > size) return false;
    const uint8_t* comp = data + dataStart;

    if (pick->method == 0) {
        out.buffer.assign(comp, comp + pick->compSize);
    } else if (pick->method == 8) {
        if (!InflateRaw(comp, pick->compSize, out.buffer,
                        pick->uncompSize ? pick->uncompSize : pick->compSize * 4)) {
            return false;
        }
    } else {
        return false;
    }
    out.pData = out.buffer.data();
    out.size = out.buffer.size();
    out.isCopy = true;
    return true;
}

// --- JPEG (EXIF Thumbnail) ---

bool PreviewExtractor::ExtractFromJPEG(const uint8_t* data, size_t size, ExtractedData& out) {
    // JPEG files have APP1 marker (0xFF 0xE1) containing EXIF
    // EXIF contains a TIFF structure with embedded JPEG thumbnail
    
    if (size < 20) return false;
    
    // Check JPEG signature
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    
    // Scan for APP1 marker
    size_t pos = 2;
    while (pos < size - 10) {
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }
        
        uint8_t marker = data[pos + 1];
        
        // Skip to next if padding
        if (marker == 0xFF) {
            pos++;
            continue;
        }
        
        // End markers
        if (marker == 0xD9 || marker == 0xDA) break;
        
        // Get segment length
        uint16_t segLen = (data[pos + 2] << 8) | data[pos + 3];
        
        // APP1 marker (0xE1) - EXIF data
        if (marker == 0xE1) {
            // Check for "Exif\0\0"
            if (pos + 10 < size && 
                data[pos + 4] == 'E' && data[pos + 5] == 'x' && 
                data[pos + 6] == 'i' && data[pos + 7] == 'f' &&
                data[pos + 8] == 0 && data[pos + 9] == 0) {
                
                // TIFF header starts at pos + 10
                const uint8_t* tiffStart = data + pos + 10;
                size_t tiffSize = segLen - 8; // Subtract "Exif\0\0" and length bytes
                
                if (tiffSize < 8 || pos + 10 + tiffSize > size) {
                    pos += 2 + segLen;
                    continue;
                }
                
                // Check TIFF byte order
                bool isLE = (tiffStart[0] == 'I' && tiffStart[1] == 'I');
                
                if ((tiffStart[0] != 'I' && tiffStart[0] != 'M') || 
                    (tiffStart[0] == 'I' && tiffStart[1] != 'I') ||
                    (tiffStart[0] == 'M' && tiffStart[1] != 'M')) {
                    pos += 2 + segLen;
                    continue;
                }
                
                // Get IFD0 offset
                uint32_t ifd0Offset = isLE ? 
                    (tiffStart[4] | (tiffStart[5] << 8) | (tiffStart[6] << 16) | (tiffStart[7] << 24)) :
                    ((tiffStart[4] << 24) | (tiffStart[5] << 16) | (tiffStart[6] << 8) | tiffStart[7]);
                
                // Parse TIFF IFD for thumbnail
                uint64_t jpgOff = 0, jpgSz = 0;
                if (ParseTiffIFD(tiffStart, tiffSize, ifd0Offset, isLE, jpgOff, jpgSz)) {
                    if (jpgOff > 0 && jpgSz > 100 && jpgOff + jpgSz <= tiffSize) {
                        out.pData = tiffStart + jpgOff;
                        out.size = (size_t)jpgSz;
                        return true;
                    }
                }
                
                // Try IFD1 (often contains the thumbnail)
                // After IFD0 entries, there's a pointer to IFD1
                if (ifd0Offset < tiffSize) {
                    uint16_t numEntries = isLE ? 
                        (tiffStart[ifd0Offset] | (tiffStart[ifd0Offset + 1] << 8)) :
                        ((tiffStart[ifd0Offset] << 8) | tiffStart[ifd0Offset + 1]);
                    
                    size_t ifd1PtrPos = ifd0Offset + 2 + numEntries * 12;
                    if (ifd1PtrPos + 4 <= tiffSize) {
                        uint32_t ifd1Offset = isLE ?
                            (tiffStart[ifd1PtrPos] | (tiffStart[ifd1PtrPos + 1] << 8) | 
                             (tiffStart[ifd1PtrPos + 2] << 16) | (tiffStart[ifd1PtrPos + 3] << 24)) :
                            ((tiffStart[ifd1PtrPos] << 24) | (tiffStart[ifd1PtrPos + 1] << 16) | 
                             (tiffStart[ifd1PtrPos + 2] << 8) | tiffStart[ifd1PtrPos + 3]);
                        
                        if (ifd1Offset > 0 && ifd1Offset < tiffSize) {
                            if (ParseTiffIFD(tiffStart, tiffSize, ifd1Offset, isLE, jpgOff, jpgSz)) {
                                if (jpgOff > 0 && jpgSz > 100 && jpgOff + jpgSz <= tiffSize) {
                                    out.pData = tiffStart + jpgOff;
                                    out.size = (size_t)jpgSz;
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        pos += 2 + segLen;
    }
    
    return false;
}
