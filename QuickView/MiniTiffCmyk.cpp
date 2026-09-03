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
#include <windows.h>
#include <icm.h>

namespace QuickView::MiniTiff {

// Helper to locate and open standard Adobe CMYK ICC profile from Windows color directory
static cmsHPROFILE OpenDefaultAdobeCmykProfile() {
    static const wchar_t* const kColorDir = L"C:\\Windows\\System32\\spool\\drivers\\color";

    // Common standard Adobe CMYK profile candidates in order of preference:
    // 1. Japan Color 2001 Coated (standard across Asia / domestic prepress)
    // 2. US Web Coated (SWOP) v2 (standard Adobe default in US/global)
    // 3. RSWOP.icm (Windows default SWOP)
    const wchar_t* candidates[] = {
        L"JapanColor2001Coated.icc",
        L"USWebCoatedSWOP.icc",
        L"RSWOP.icm"
    };

    for (const wchar_t* name : candidates) {
        wchar_t fullPath[MAX_PATH] = {};
        swprintf_s(fullPath, L"%s\\%s", kColorDir, name);
        HANDLE hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD sizeHigh = 0;
            DWORD sizeLow = GetFileSize(hFile, &sizeHigh);
            if (sizeHigh == 0 && sizeLow >= 128 && sizeLow <= 64 * 1024 * 1024) {
                std::vector<uint8_t> buf(sizeLow);
                DWORD readBytes = 0;
                if (ReadFile(hFile, buf.data(), sizeLow, &readBytes, nullptr) && readBytes == sizeLow) {
                    CloseHandle(hFile);
                    cmsHPROFILE hProf = cmsOpenProfileFromMem(buf.data(), static_cast<cmsUInt32Number>(buf.size()));
                    if (hProf) {
                        if (cmsGetColorSpace(hProf) == cmsSigCmykData) {
                            return hProf;
                        }
                        cmsCloseProfile(hProf);
                    }
                    continue;
                }
            }
            CloseHandle(hFile);
        }
    }
    return nullptr;
}

bool CmykIccXform::Build(const uint8_t* icc, size_t iccLen) {
    if (icc && iccLen >= 128) {
        hIn = cmsOpenProfileFromMem(icc, static_cast<cmsUInt32Number>(iccLen));
        if (hIn && cmsGetColorSpace(hIn) != cmsSigCmykData) {
            cmsCloseProfile(hIn);
            hIn = nullptr;
        }
    }

    // When the TIFF has no embedded CMYK ICC profile (or it was invalid),
    // fall back to the standard Adobe CMYK profile (Japan Color 2001 Coated / US Web Coated SWOP)
    // instead of degrading to inaccurate naive formulas.
    if (!hIn) {
        hIn = OpenDefaultAdobeCmykProfile();
    }

    if (!hIn) return false;

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

void ConvertCmykToBgra(const uint8_t* src, uint8_t* dst, int width, int samples, bool hasAlpha, bool premultiply, cmsHTRANSFORM xform) {
    (void)premultiply;
    if (xform != nullptr) {
        // Accurate CMYK -> sRGB via ICC profile (either embedded or Adobe standard fallback).
        // lcms expects tightly packed 4-channel CMYK input (TYPE_CMYK_8).
        if (samples == 4) {
            cmsDoTransform(xform, const_cast<uint8_t*>(src), dst, width);
            for (int x = 0; x < width; ++x) dst[x * 4 + 3] = 255;   // opaque
            return;
        }

        // samples > 4 (e.g. CMYK + Alpha or CMYK + Spot channels):
        // 1. Convert the first 4 CMYK channels to RGB via ICC
        std::vector<uint8_t> cmyk(static_cast<size_t>(width) * 4);
        for (int x = 0; x < width; ++x) {
            cmyk[x * 4 + 0] = src[x * samples + 0];
            cmyk[x * 4 + 1] = src[x * samples + 1];
            cmyk[x * 4 + 2] = src[x * samples + 2];
            cmyk[x * 4 + 3] = src[x * samples + 3];
        }
        cmsDoTransform(xform, cmyk.data(), dst, width);

        if (hasAlpha) {
            // Channel 4 is an authentic transparent background mask (ExtraSamples == 1 or 2).
            // Follow strict Direct2D D2D1_ALPHA_MODE_PREMULTIPLIED standard:
            // When a == 0 (fully transparent), RGB MUST be 0.
            // If RGB were non-zero (e.g. 255), D2D additive blend would display a solid white background!
            for (int x = 0; x < width; ++x) {
                uint8_t a = src[x * samples + 4];
                if (a == 0) {
                    dst[x * 4 + 0] = 0;
                    dst[x * 4 + 1] = 0;
                    dst[x * 4 + 2] = 0;
                    dst[x * 4 + 3] = 0;
                } else if (a < 255) {
                    dst[x * 4 + 0] = static_cast<uint8_t>((dst[x * 4 + 0] * a + 127) / 255);
                    dst[x * 4 + 1] = static_cast<uint8_t>((dst[x * 4 + 1] * a + 127) / 255);
                    dst[x * 4 + 2] = static_cast<uint8_t>((dst[x * 4 + 2] * a + 127) / 255);
                    dst[x * 4 + 3] = a;
                } else {
                    dst[x * 4 + 3] = 255;
                }
            }
        } else {
            // Unspecified spot plate (e.g. white ink, glue): fully opaque artwork
            for (int x = 0; x < width; ++x) {
                dst[x * 4 + 3] = 255;
            }
        }
        return;
    }

    // ---- Fallback: naive formula (only if no ICC and default profile unavailable) ----
    for (int x = 0; x < width; ++x) {
        uint8_t c = src[x * samples + 0];
        uint8_t m = src[x * samples + 1];
        uint8_t y = src[x * samples + 2];
        uint8_t k = src[x * samples + 3];

        uint32_t invK = 255 - k;
        uint32_t rTemp = (255 - c) * invK;
        uint32_t gTemp = (255 - m) * invK;
        uint32_t bTemp = (255 - y) * invK;

        // Mathematical fixed-point division by 255 with 100% precision: (val + 128 + (val >> 8)) >> 8
        uint8_t r = static_cast<uint8_t>((rTemp + 128 + (rTemp >> 8)) >> 8);
        uint8_t g = static_cast<uint8_t>((gTemp + 128 + (gTemp >> 8)) >> 8);
        uint8_t b = static_cast<uint8_t>((bTemp + 128 + (bTemp >> 8)) >> 8);

        if (hasAlpha && samples >= 5) {
            uint8_t a = src[x * samples + 4];
            if (a == 0) {
                dst[x * 4 + 0] = 0;
                dst[x * 4 + 1] = 0;
                dst[x * 4 + 2] = 0;
                dst[x * 4 + 3] = 0;
            } else if (a < 255) {
                r = static_cast<uint8_t>((r * a + 127) / 255);
                g = static_cast<uint8_t>((g * a + 127) / 255);
                b = static_cast<uint8_t>((b * a + 127) / 255);
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = a;
            } else {
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = 255;
            }
        } else {
            dst[x * 4 + 0] = b;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = r;
            dst[x * 4 + 3] = 255;
        }
    }
}

} // namespace QuickView::MiniTiff
