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
#include <vector>

namespace QuickView::MiniTiff {

bool CmykIccXform::Build(const uint8_t* icc, size_t iccLen) {
    if (!icc || iccLen < 128) return false;
    hIn = cmsOpenProfileFromMem(icc, static_cast<cmsUInt32Number>(iccLen));
    if (!hIn) return false;
    // Only apply when the embedded profile is actually a CMYK color space.
    if (cmsGetColorSpace(hIn) != cmsSigCmykData) {
        cmsCloseProfile(hIn); hIn = nullptr; return false;
    }
    hOut = cmsCreate_sRGBProfile();
    if (!hOut) { cmsCloseProfile(hIn); hIn = nullptr; return false; }
    xform = cmsCreateTransform(hIn, TYPE_CMYK_8, hOut, TYPE_BGRA_8,
                               INTENT_RELATIVE_COLORIMETRIC,
                               cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
    // cmsFLAGS_NOCACHE: lcms2's internal 1-pixel cache is not thread-safe, so
    // it must be off for cmsDoTransform to be callable from multiple worker
    // threads on the same transform (thumbnail paths decode rows in parallel).
    if (!xform) { cmsCloseProfile(hOut); cmsCloseProfile(hIn); hOut = nullptr; hIn = nullptr; return false; }
    return true;
}

CmykIccXform::~CmykIccXform() {
    if (xform) cmsDeleteTransform(xform);   // must delete before closing profiles
    if (hOut)  cmsCloseProfile(hOut);
    if (hIn)   cmsCloseProfile(hIn);
}

void ConvertCmykToBgra(const uint8_t* src, uint8_t* dst, int width, int samples, bool premultiply, cmsHTRANSFORM xform) {
    bool hasAlpha = (samples >= 5);

    if (xform != nullptr) {
        // Accurate CMYK -> sRGB via the embedded ICC profile.
        // lcms expects tightly packed 4-channel CMYK input (TYPE_CMYK_8).
        if (samples == 4) {
            cmsDoTransform(xform, const_cast<uint8_t*>(src), dst, width);
            for (int x = 0; x < width; ++x) dst[x * 4 + 3] = 255;   // opaque
            return;
        }
        // samples > 4 (e.g. CMYKA): compact the CMYK channels into a temp row,
        // transform, then re-apply the alpha (lcms output alpha is meaningless).
        std::vector<uint8_t> cmyk(static_cast<size_t>(width) * 4);
        for (int x = 0; x < width; ++x) {
            cmyk[x * 4 + 0] = src[x * samples + 0];
            cmyk[x * 4 + 1] = src[x * samples + 1];
            cmyk[x * 4 + 2] = src[x * samples + 2];
            cmyk[x * 4 + 3] = src[x * samples + 3];
        }
        cmsDoTransform(xform, cmyk.data(), dst, width);
        for (int x = 0; x < width; ++x) {
            uint8_t a = hasAlpha ? src[x * samples + 4] : 255;
            if (premultiply && hasAlpha) {
                dst[x * 4 + 2] = static_cast<uint8_t>((dst[x * 4 + 2] * a + 127) / 255);
                dst[x * 4 + 1] = static_cast<uint8_t>((dst[x * 4 + 1] * a + 127) / 255);
                dst[x * 4 + 0] = static_cast<uint8_t>((dst[x * 4 + 0] * a + 127) / 255);
            }
            dst[x * 4 + 3] = a;
        }
        return;
    }

    // ---- Fallback: naive formula (no/unsuitable ICC, 16-bit, etc.) ----
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
