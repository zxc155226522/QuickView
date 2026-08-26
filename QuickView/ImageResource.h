/*
 * QuickView - Image Resource Management and GPU Asset Handles
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

#pragma once

#include "pch.h"
#include "ImageTypes.h"
#include "AnimationDecoder.h"

#include <d2d1_2.h>
#include <d2d1_3.h>
#include <dxgi1_2.h>
#include <memory>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct ImageResource {
    ComPtr<ID2D1Bitmap> bitmap;
    ComPtr<ID2D1SvgDocument> svgDoc;
    bool isSvg = false;
    float svgW = 0.0f;
    float svgH = 0.0f;

    // [resvg] CDR/AI/SVG that render through the resvg rasterizer (embedded
    // bitmaps or D2D SVG subset fallback) keep their SVG source here so the
    // preview can re-rasterize at display resolution on open and on zoom,
    // instead of stretching one fixed low-res bitmap (which caused blur).
    bool isResvg = false;
    std::shared_ptr<QuickView::RawImageFrame::SvgData> resvgSrc;
    UINT resvgRasterW = 0;
    UINT resvgRasterH = 0;

    // [MuPDF] CDR/CMX rendered via MuPDF display list (vector pipeline).
    // display list is built once from SVG XML, then fz_run_display_list
    // rasterizes at any resolution with just a new transform matrix.
    // Replaces the resvg path for CDR/CMX — no SVG re-parsing on zoom.
    bool isMupdf = false;
    std::shared_ptr<QuickView::RawImageFrame::SvgData> mupdfSrc;  // SVG XML buffer
    UINT mupdfRasterW = 0;  // current bitmap resolution
    UINT mupdfRasterH = 0;

    // [PDFium] PDF/AI rendered via PDFium (vector rasterizer).
    // PDF is inherently vector — PDFium can re-rasterize at any resolution.
    // On zoom, the surface is re-rasterized via DocumentRenderController to
    // maintain sharp text/lines (same principle as resvg/mupdf re-raster).
    bool isPdfium = false;
    std::wstring pdfiumPath;           // source PDF file path
    uint32_t pdfiumPageIndex = 0;      // current page index
    float pdfiumPageWidthPts = 0.0f;  // page width in points (72dpi)
    float pdfiumPageHeightPts = 0.0f;  // page height in points
    UINT pdfiumRasterW = 0;           // current rasterized bitmap width
    UINT pdfiumRasterH = 0;            // current rasterized bitmap height

    QuickView::GpuBlendOp blendOp = QuickView::GpuBlendOp::None;
    QuickView::GpuShaderPayload shaderPayload = {};
    std::unique_ptr<QuickView::AuxLayer> auxLayer;

    std::shared_ptr<QuickView::IAnimationDecoder> animator;
    QuickView::AnimationFrameMeta frameMeta;

    void Reset() {
        bitmap.Reset();
        svgDoc.Reset();
        isSvg = false;
        svgW = 0.0f;
        svgH = 0.0f;
        isResvg = false;
        resvgSrc.reset();
        resvgRasterW = 0;
        resvgRasterH = 0;
        isMupdf = false;
        mupdfSrc.reset();
        mupdfRasterW = 0;
        mupdfRasterH = 0;
        isPdfium = false;
        pdfiumPath.clear();
        pdfiumPageIndex = 0;
        pdfiumPageWidthPts = 0.0f;
        pdfiumPageHeightPts = 0.0f;
        pdfiumRasterW = 0;
        pdfiumRasterH = 0;
        blendOp = QuickView::GpuBlendOp::None;
        shaderPayload = {};
        auxLayer.reset();
        animator.reset();
        frameMeta = {};
    }

    ImageResource() = default;
    ImageResource(const ImageResource&) = delete;
    ImageResource& operator=(const ImageResource&) = delete;
    ImageResource(ImageResource&&) = default;
    ImageResource& operator=(ImageResource&&) = default;

    ImageResource Clone() const {
        ImageResource cloned;
        cloned.bitmap = bitmap;
        cloned.svgDoc = svgDoc;
        cloned.isSvg = isSvg;
        cloned.svgW = svgW;
        cloned.svgH = svgH;
        cloned.isResvg = isResvg;
        cloned.resvgSrc = resvgSrc;
        cloned.resvgRasterW = resvgRasterW;
        cloned.resvgRasterH = resvgRasterH;
        cloned.isMupdf = isMupdf;
        cloned.mupdfSrc = mupdfSrc;
        cloned.mupdfRasterW = mupdfRasterW;
        cloned.mupdfRasterH = mupdfRasterH;
        cloned.isPdfium = isPdfium;
        cloned.pdfiumPath = pdfiumPath;
        cloned.pdfiumPageIndex = pdfiumPageIndex;
        cloned.pdfiumPageWidthPts = pdfiumPageWidthPts;
        cloned.pdfiumPageHeightPts = pdfiumPageHeightPts;
        cloned.pdfiumRasterW = pdfiumRasterW;
        cloned.pdfiumRasterH = pdfiumRasterH;
        cloned.blendOp = blendOp;
        cloned.shaderPayload = shaderPayload;
        if (auxLayer) {
            cloned.auxLayer = auxLayer->Clone();
        }
        cloned.animator = animator;
        cloned.frameMeta = frameMeta;
        return cloned;
    }

    D2D1_SIZE_F GetSize() const {
        // [resvg/MuPDF] rasterized SVGs store intrinsic dimensions in svgW/svgH.
        // [PDFium] PDF pages store intrinsic dimensions in pdfiumPageWidthPts/HeightPts
        // (converted to pixels at 96dpi for consistent layout with other images).
        if (isSvg || isResvg || isMupdf) return D2D1::SizeF(svgW, svgH);
        if (isPdfium && pdfiumPageWidthPts > 0 && pdfiumPageHeightPts > 0) {
            const float kPtsToPx = 96.0f / 72.0f;
            return D2D1::SizeF(pdfiumPageWidthPts * kPtsToPx, pdfiumPageHeightPts * kPtsToPx);
        }
        if (bitmap) return bitmap->GetSize();
        return D2D1::SizeF(0.0f, 0.0f);
    }

    operator bool() const {
        return (isSvg && svgDoc) || bitmap;
    }
};

inline DXGI_FORMAT GetImageResourceSurfaceFormat(const ImageResource& resource) {
    if (!resource.bitmap) {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    const DXGI_FORMAT bitmapFormat = resource.bitmap->GetPixelFormat().format;
    if (bitmapFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        bitmapFormat == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

inline D2D1_SIZE_F GetOrientedSize(const ImageResource& res, int exifOrientation) {
    D2D1_SIZE_F size = res.GetSize();
    const bool swapped = (exifOrientation >= 5 && exifOrientation <= 8);
    if (swapped) {
        return D2D1::SizeF(size.height, size.width);
    }
    return size;
}

