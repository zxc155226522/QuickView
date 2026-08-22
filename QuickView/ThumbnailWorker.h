#pragma once
// ============================================================================
// ThumbnailWorker.h
// Headless thumbnail generator for the Windows Shell (Explorer) integration.
// Invoked as:
//   QuickView.exe --thumbnail --input <file> --out <bmp> --size <px>   (one-shot, legacy/fallback)
//   QuickView.exe --thumbnail-server [--idle <sec>]                    (persistent IPC server)
// Both run BEFORE config/window/single-instance initialization (tool process).
// ============================================================================
#include <windows.h>

namespace QuickView {

// One-shot: render a single thumbnail and write BMP data to stdout (pipe).
// No temp file is created. Returns 0 = success, 2 = failure.
// Invocation: QuickView.exe --thumbnail --input <file> --size <px>
int RunThumbnailWorker(int argc, LPWSTR* argv);

// Persistent: a named-pipe server that renders thumbnails for the shell
// provider DLL on demand, reusing ONE process/DLL-load across many requests.
// This eliminates the per-thumbnail process-spawn storm under folder load.
// Returns process exit code (0 = clean idle exit, 2 = fatal).
int RunThumbnailServer(int argc, LPWSTR* argv);

// Gracefully stop a running thumbnail server (if any) so it respawns with
// updated settings (e.g. parallel-thread count). Called from the settings UI.
void KillThumbnailServer();

} // namespace QuickView
