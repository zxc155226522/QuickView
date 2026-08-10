// ============================================================================
// ThumbnailWorker.cpp
// Headless thumbnail generator: reuses CImageLoader::LoadThumbnail (the same
// pipeline as the in-app gallery) and writes a 32bpp BGRA BMP for the shell
// thumbnail provider DLL (QuickViewThumbnailProvider.dll).
//
//   QuickView.exe --thumbnail --input <file> --out <bmp> --size <px>
//
// Vector formats (SVG/CDR/CMX/PLT/DXF/DWG) render on a TRANSPARENT background
// (Explorer draws its own plate). PDF/AI stay opaque white (paper semantics).
// ============================================================================
#include "ThumbnailWorker.h"
#include "ImageLoader.h"
#include "SupportedExtensions.h"

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <strsafe.h>

namespace {

bool TryReadArgValue(int argc, LPWSTR* argv, const wchar_t* name,
                     std::wstring* out) {
  if (!out) return false;
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] && _wcsicmp(argv[i], name) == 0 && argv[i + 1]) {
      *out = argv[i + 1];
      return true;
    }
  }
  return false;
}

bool TryReadPositiveIntArg(int argc, LPWSTR* argv, const wchar_t* name,
                           int* out) {
  std::wstring val;
  if (!TryReadArgValue(argc, argv, name, &val)) return false;
  wchar_t* end = nullptr;
  long v = wcstol(val.c_str(), &end, 10);
  if (!end || *end != L'\0' || v <= 0 || v > 4096) return false;
  *out = static_cast<int>(v);
  return true;
}

// Vector formats: transparent background. Documents (PDF/AI): opaque white.
bool WantsTransparentBackground(const std::wstring& path) {
  std::wstring_view ext = QuickView::ExtensionOf(path);
  return QuickView::ExtEqualsIgnoreCase(ext, L".svg") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cdr") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cmx") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".plt") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".dxf") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".dwg");
}

// Write 32bpp BGRA top-down BMP. Alpha bytes are preserved verbatim (some
// viewers ignore the alpha channel, but our provider DLL reads it back raw).
bool WriteBmp32(const std::wstring& outPath, const uint8_t* pixels, int width,
                int height, int stride) {
  if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return false;

  BITMAPFILEHEADER bfh = {};
  BITMAPINFOHEADER bih = {};
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = width;
  bih.biHeight = -height; // top-down
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;

  const DWORD rowBytes = static_cast<DWORD>(width) * 4;
  const DWORD imageBytes = rowBytes * static_cast<DWORD>(height);
  bfh.bfType = 0x4D42; // 'BM'
  bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
  bfh.bfSize = bfh.bfOffBits + imageBytes;

  HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;

  bool ok = false;
  DWORD written = 0;
  if (WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr) &&
      WriteFile(hFile, &bih, sizeof(bih), &written, nullptr)) {
    ok = true;
    for (int y = 0; y < height && ok; ++y) {
      ok = WriteFile(hFile, pixels + static_cast<size_t>(y) * stride, rowBytes,
                     &written, nullptr) != FALSE;
    }
  }
  CloseHandle(hFile);
  if (!ok) DeleteFileW(outPath.c_str());
  return ok;
}

// ---------------------------------------------------------------------------
// Persistent server support (named-pipe IPC).
// ---------------------------------------------------------------------------

// Build a 32bpp BGRA top-down BMP into an in-memory buffer (same layout as
// WriteBmp32 writes to disk, so the provider can parse it back identically).
bool BuildBmp32Memory(const uint8_t* pixels, int width, int height, int stride,
                      std::vector<BYTE>& out) {
  if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return false;
  BITMAPFILEHEADER bfh = {};
  BITMAPINFOHEADER bih = {};
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = width;
  bih.biHeight = -height; // top-down
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;
  const DWORD rowBytes = static_cast<DWORD>(width) * 4;
  const DWORD imageBytes = rowBytes * static_cast<DWORD>(height);
  bfh.bfType = 0x4D42; // 'BM'
  bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
  bfh.bfSize = bfh.bfOffBits + imageBytes;

  out.resize(bfh.bfSize);
  BYTE* p = out.data();
  memcpy(p, &bfh, sizeof(bfh)); p += sizeof(bfh);
  memcpy(p, &bih, sizeof(bih)); p += sizeof(bih);
  for (int y = 0; y < height; ++y) {
    memcpy(p, pixels + static_cast<size_t>(y) * stride, rowBytes);
    p += rowBytes;
  }
  return true;
}

// [Server log] Append to C:\Windows\Temp\qvthumb_server.log for diagnosis.
static void ServerLog(const std::wstring& msg) {
  HANDLE h = CreateFileW(L"C:\\Windows\\Temp\\qvthumb_server.log",
                         FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  wchar_t line[640];
  DWORD ts = GetTickCount();
  StringCchPrintfW(line, 640, L"[srv %lu] %s\r\n", ts, msg.c_str());
  DWORD written = 0;
  WriteFile(h, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
  CloseHandle(h);
}

// Overlapped, timeout-bounded pipe read/write that always transfers exactly
// `len` bytes (looping on partial transfers). Used by both server and client.
// On timeout the pending I/O is cancelled and reaped so the local OVERLAPPED
// never outlives an in-flight operation.
static bool PipeIoExact(HANDLE hPipe, HANDLE hEvent, void* buf, DWORD len,
                        DWORD timeoutMs, bool isRead) {
  BYTE* p = static_cast<BYTE*>(buf);
  DWORD total = 0;
  while (total < len) {
    ResetEvent(hEvent);
    OVERLAPPED ol = {};
    ol.hEvent = hEvent;
    DWORD done = 0;
    BOOL ok = isRead ? ReadFile(hPipe, p + total, len - total, &done, &ol)
                     : WriteFile(hPipe, p + total, len - total, &done, &ol);
    if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        if (WaitForSingleObject(hEvent, timeoutMs) != WAIT_OBJECT_0) {
          CancelIo(hPipe);
          GetOverlappedResult(hPipe, &ol, &done, TRUE); // reap aborted op
          return false;
        }
        if (!GetOverlappedResult(hPipe, &ol, &done, FALSE)) return false;
      } else {
        return false;
      }
    }
    if (done == 0) return false; // peer closed early
    total += done;
  }
  return true;
}
static bool PipeReadExact(HANDLE hPipe, HANDLE hEvent, void* buf, DWORD len, DWORD ms) {
  return PipeIoExact(hPipe, hEvent, buf, len, ms, true);
}
static bool PipeWriteAll(HANDLE hPipe, HANDLE hEvent, const void* buf, DWORD len, DWORD ms) {
  return PipeIoExact(hPipe, hEvent, const_cast<void*>(buf), len, ms, false);
}

// Serve one connected client: read request frame, render, write response.
static void ServeOneRequest(HANDLE hPipe, HANDLE hEvent, CImageLoader& loader) {
  uint32_t pathByteLen = 0;
  if (!PipeReadExact(hPipe, hEvent, &pathByteLen, sizeof(pathByteLen), 15000)) return;
  if (pathByteLen == 0 || pathByteLen > 65536) return;
  std::wstring path;
  path.resize(pathByteLen / 2);
  if (!PipeReadExact(hPipe, hEvent, path.data(), pathByteLen, 15000)) return;
  uint32_t size = 0;
  if (!PipeReadExact(hPipe, hEvent, &size, sizeof(size), 15000)) return;
  if (size == 0 || size > 4096) size = 256;

  auto t0 = std::chrono::steady_clock::now();
  CImageLoader::ThumbData thumb;
  const bool transparentBg = WantsTransparentBackground(path);
  HRESULT hr = loader.LoadThumbnail(path.c_str(), static_cast<int>(size), &thumb, true, transparentBg);
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  std::vector<BYTE> bmp;
  bool ok = SUCCEEDED(hr) && thumb.isValid && !thumb.pixels.empty() &&
            BuildBmp32Memory(thumb.pixels.data(), thumb.width, thumb.height, thumb.stride, bmp);

  uint32_t status = ok ? 0u : 1u;
  if (!PipeWriteAll(hPipe, hEvent, &status, sizeof(status), 15000)) return;
  if (ok) {
    uint32_t bmpLen = static_cast<uint32_t>(bmp.size());
    if (!PipeWriteAll(hPipe, hEvent, &bmpLen, sizeof(bmpLen), 15000) ||
        !PipeWriteAll(hPipe, hEvent, bmp.data(), bmpLen, 15000)) {
      ServerLog(L"write bmp failed");
      return;
    }
  }
  ServerLog(L"req path=" + path + L" size=" + std::to_wstring(size) +
            (ok ? L" OK " : L" FAIL ") + L"(" + std::to_wstring(static_cast<int>(ms)) + L"ms)");
}

} // namespace

int QuickView::RunThumbnailWorker(int argc, LPWSTR* argv) {
  std::wstring inputPath;
  std::wstring outPath;
  int size = 0;
  if (!TryReadArgValue(argc, argv, L"--input", &inputPath) ||
      !TryReadArgValue(argc, argv, L"--out", &outPath) ||
      !TryReadPositiveIntArg(argc, argv, L"--size", &size)) {
    return 2;
  }

  // WIC is used by some thumbnail fallbacks inside LoadThumbnail.
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  CImageLoader loader;
  CImageLoader::ThumbData thumb;
  const bool transparentBg = WantsTransparentBackground(inputPath);
  HRESULT hr =
      loader.LoadThumbnail(inputPath.c_str(), size, &thumb, true, transparentBg);

  bool ok = SUCCEEDED(hr) && thumb.isValid && !thumb.pixels.empty() &&
            WriteBmp32(outPath, thumb.pixels.data(), thumb.width, thumb.height,
                       thumb.stride);

  if (SUCCEEDED(coHr)) CoUninitialize();
  return ok ? 0 : 2;
}

int QuickView::RunThumbnailServer(int argc, LPWSTR* argv) {
  int idleSec = 60;
  std::wstring idleVal;
  if (TryReadArgValue(argc, argv, L"--idle", &idleVal)) {
    long v = wcstol(idleVal.c_str(), nullptr, 10);
    if (v > 0 && v <= 3600) idleSec = static_cast<int>(v);
  }

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  // One loader instance reused across requests. The server is single-threaded
  // (renders one request at a time), so this is safe despite g_cdrPageCache
  // and other shared mutable state inside CImageLoader.
  CImageLoader loader;
  const DWORD idleTimeoutMs = static_cast<DWORD>(idleSec) * 1000;

  HANDLE hPipe = CreateNamedPipeW(
      L"\\\\.\\pipe\\QuickViewThumb",
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1, 65536, 65536, 0, nullptr);
  if (hPipe == INVALID_HANDLE_VALUE) {
    // Another instance already owns this pipe name -> a server is running.
    // Exit quietly so only one server exists.
    DWORD err = GetLastError();
    ServerLog(L"CreateNamedPipe failed err=" + std::to_wstring(err) +
              L" (another server already running?)");
    if (SUCCEEDED(coHr)) CoUninitialize();
    return (err == ERROR_ACCESS_DENIED) ? 0 : 2;
  }

  HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!hEvent) {
    CloseHandle(hPipe);
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 2;
  }

  ServerLog(L"Server started (idle=" + std::to_wstring(idleSec) + L"s)");

  OVERLAPPED ol = {};
  ol.hEvent = hEvent;
  while (true) {
    ResetEvent(hEvent);
    BOOL connected = ConnectNamedPipe(hPipe, &ol);
    bool isConnected = false;
    if (connected) {
      isConnected = true;
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        isConnected = true; // client connected between CreateNamedPipe and here
      } else if (err == ERROR_IO_PENDING) {
        DWORD w = WaitForSingleObject(hEvent, idleTimeoutMs);
        if (w == WAIT_TIMEOUT) {
          CancelIo(hPipe);
          DWORD dummy = 0;
          GetOverlappedResult(hPipe, &ol, &dummy, TRUE); // reap aborted connect
          ServerLog(L"Idle timeout, exiting");
          break;
        } else if (w == WAIT_OBJECT_0) {
          isConnected = true;
        } else {
          break; // wait failed
        }
      } else {
        ServerLog(L"ConnectNamedPipe err=" + std::to_wstring(err));
        break;
      }
    }

    if (isConnected) {
      ServeOneRequest(hPipe, hEvent, loader);
      DisconnectNamedPipe(hPipe);
    }
  }

  CloseHandle(hEvent);
  CloseHandle(hPipe);
  if (SUCCEEDED(coHr)) CoUninitialize();
  ServerLog(L"Server stopped");
  return 0;
}
