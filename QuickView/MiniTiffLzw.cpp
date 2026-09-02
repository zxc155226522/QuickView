/*
 * QuickView Mini TIFF Decoder - LZW decompression implementation
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
#include <cstdio>
#include <cstring>

namespace QuickView::MiniTiff {

// Fast table-driven decoder. Output is append-only, so when a code is decoded
// its full string lives contiguously at a recorded offset of the output buffer.
// Each new dictionary entry's string = string(oldCode) + firstChar(current
// code) — which is exactly the previous code's output span extended by one
// byte (outputs are back-to-back). Recording (pos, len) per entry turns every
// decode into a single memcpy instead of a per-byte prefix-chain walk.
bool DecompressLzw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (srcLen == 0 || dstLen == 0 || !src || !dst) {
        return false;
    }

    uint16_t nextCode = 258;
    uint32_t codeSize = 9;

    // Stack-allocated dictionary: offset/length of each entry's string inside
    // the already-written output. Valid until the next Clear code.
    uint32_t entPos[4096];
    uint32_t entLen[4096];

    size_t srcIdx = 0;
    size_t dstIdx = 0;

    uint64_t bitBuffer = 0;
    uint32_t bitCount = 0;

    auto readCode = [&]() -> int32_t {
        while (bitCount < codeSize) {
            if (srcIdx >= srcLen) {
                return -1; // Out of bitstream bytes
            }
            bitBuffer = (bitBuffer << 8) | src[srcIdx++];
            bitCount += 8;
        }
        uint32_t shift = bitCount - codeSize;
        uint32_t mask = (1u << codeSize) - 1;
        uint32_t code = static_cast<uint32_t>(bitBuffer >> shift) & mask;
        bitCount = shift;
        return static_cast<int32_t>(code);
    };

    // Offset/length of oldCode's emitted string (the previous decode).
    uint32_t prevPos = 0, prevLen = 0;
    bool havePrev = false;

    while (dstIdx < dstLen) {
        int32_t code = readCode();
        if (code < 0 || code == 257) { // EOF or error
            break;
        }

        if (code == 256) { // Clear Code
            nextCode = 258;
            codeSize = 9;
            havePrev = false;
            continue;
        }

        uint32_t curPos, curLen;
        if (!havePrev) {
            // First code after a clear is always a literal.
            if (code >= 256) {
                return false; // Invalid first code
            }
            dst[dstIdx++] = static_cast<uint8_t>(code);
            prevPos = static_cast<uint32_t>(dstIdx - 1);
            prevLen = 1;
            havePrev = true;
            continue; // no dictionary entry is added for the first code
        } else if (code < 256) {
            dst[dstIdx] = static_cast<uint8_t>(code);
            curPos = static_cast<uint32_t>(dstIdx);
            curLen = 1;
        } else if (code < nextCode) {
            curLen = entLen[code];
            if (dstIdx + curLen > dstLen) {
                return false; // Out of output bounds
            }
            memcpy(dst + dstIdx, dst + entPos[code], curLen);
            curPos = static_cast<uint32_t>(dstIdx);
        } else if (code == nextCode) {
            // KwKwK: string(oldCode) + firstChar(oldCode)
            curLen = prevLen + 1;
            if (dstIdx + curLen > dstLen) {
                return false;
            }
            memcpy(dst + dstIdx, dst + prevPos, prevLen);
            dst[dstIdx + prevLen] = dst[prevPos];
            curPos = static_cast<uint32_t>(dstIdx);
        } else {
            return false; // Code > nextCode (Corrupt)
        }
        dstIdx += curLen;

        // Add entry: string(oldCode) + firstChar(current code). The previous
        // string ends exactly where the string just emitted begins, so the
        // entry is the span [prevPos, prevPos + prevLen + 1).
        if (nextCode < 4096) {
            entPos[nextCode] = prevPos;
            entLen[nextCode] = prevLen + 1;
            nextCode++;

            // TIFF LZW bit-width adjustment
            if (nextCode == 511 && codeSize < 12) {
                codeSize = 10;
            } else if (nextCode == 1023 && codeSize < 12) {
                codeSize = 11;
            } else if (nextCode == 2047 && codeSize < 12) {
                codeSize = 12;
            }
        }

        prevPos = curPos;
        prevLen = curLen;
    }

    if (dstIdx != dstLen) {
        std::memset(dst + dstIdx, 0, dstLen - dstIdx);
    }

    return true;
}

} // namespace QuickView::MiniTiff
