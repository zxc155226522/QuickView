#pragma once
// ============================================================================
// ThumbnailWorker.h
// Headless thumbnail generator for the Windows Shell (Explorer) integration.
// Invoked as: QuickView.exe --thumbnail --input <file> --out <bmp> --size <px>
// Runs BEFORE config/window/single-instance initialization (tool process).
// ============================================================================
#include <windows.h>

namespace QuickView {

// Returns process exit code: 0 = success, 2 = failure.
int RunThumbnailWorker(int argc, LPWSTR* argv);

} // namespace QuickView
