#pragma once

#include <d2d1.h>

struct ImageViewportLayout {
    float Left = 0.0f;
    float Top = 0.0f;
    float Right = 1.0f;
    float Bottom = 1.0f;
    float Width = 1.0f;
    float Height = 1.0f;
    float CenterOffsetX = 0.0f;
    float CenterOffsetY = 0.0f;

    D2D1_RECT_F Rect() const {
        return D2D1::RectF(Left, Top, Right, Bottom);
    }
};

ImageViewportLayout ComputeImageViewportLayout(float windowWidth, float windowHeight);
