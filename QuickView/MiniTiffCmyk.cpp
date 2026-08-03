/*
 * QuickView Mini TIFF Decoder - CMYK conversion implementation
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

namespace QuickView::MiniTiff {

void ConvertCmykToBgra(const uint8_t* src, uint8_t* dst, int width, int samples, bool premultiply) {
    bool hasAlpha = (samples >= 5);
    for (int x = 0; x < width; ++x) {
        uint8_t c = src[x * samples + 0];
        uint8_t m = src[x * samples + 1];
        uint8_t y = src[x * samples + 2];
        uint8_t k = src[x * samples + 3];
        uint8_t a = hasAlpha ? src[x * samples + 4] : 255;

        uint32_t invK = 255 - k;
        uint32_t rTemp = (255 - c) * invK;
        uint32_t gTemp = (255 - m) * invK;
        uint32_t bTemp = (255 - y) * invK;

        // Mathematical fixed-point division by 255 with 100% precision: (val + 128 + (val >> 8)) >> 8
        uint8_t r = static_cast<uint8_t>((rTemp + 128 + (rTemp >> 8)) >> 8);
        uint8_t g = static_cast<uint8_t>((gTemp + 128 + (gTemp >> 8)) >> 8);
        uint8_t b = static_cast<uint8_t>((bTemp + 128 + (bTemp >> 8)) >> 8);

        if (hasAlpha && premultiply) {
            r = static_cast<uint8_t>((r * a + 127) / 255);
            g = static_cast<uint8_t>((g * a + 127) / 255);
            b = static_cast<uint8_t>((b * a + 127) / 255);
        }

        dst[x * 4 + 0] = b;
        dst[x * 4 + 1] = g;
        dst[x * 4 + 2] = r;
        dst[x * 4 + 3] = a;
    }
}

} // namespace QuickView::MiniTiff
