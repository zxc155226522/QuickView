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
#include <wincodec.h>

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <strsafe.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <shlobj.h>

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

// Vector graphics (SVG/CDR/CMX/PLT): transparent background.
// Documents & CAD blueprints (PDF/AI/DXF/DWG): opaque white (paper/drawing semantics,
// ensures black lines remain visible on dark Windows Explorer themes).
bool WantsTransparentBackground(const std::wstring& path) {
  std::wstring_view ext = QuickView::ExtensionOf(path);
  return QuickView::ExtEqualsIgnoreCase(ext, L".svg") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cdr") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cmx") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".plt");
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
// `len` bytes (looping on partial transfers). Each call creates its OWN event
// so concurrent workers (and the accept loop's connect) never share one event
// and stomp on each other's ResetEvent/WaitForSingleObject. On timeout the
// pending I/O is cancelled and reaped so the local OVERLAPPED never outlives
// an in-flight operation.
static bool PipeIoExact(HANDLE hPipe, void* buf, DWORD len,
                        DWORD timeoutMs, bool isRead) {
  HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!hEvent) return false;
  BYTE* p = static_cast<BYTE*>(buf);
  DWORD total = 0;
  bool ok = true;
  while (total < len) {
    ResetEvent(hEvent);
    OVERLAPPED ol = {};
    ol.hEvent = hEvent;
    DWORD done = 0;
    BOOL r = isRead ? ReadFile(hPipe, p + total, len - total, &done, &ol)
                    : WriteFile(hPipe, p + total, len - total, &done, &ol);
    if (!r) {
      DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        if (WaitForSingleObject(hEvent, timeoutMs) != WAIT_OBJECT_0) {
          CancelIo(hPipe);
          GetOverlappedResult(hPipe, &ol, &done, TRUE); // reap aborted op
          ok = false; break;
        }
        if (!GetOverlappedResult(hPipe, &ol, &done, FALSE)) { ok = false; break; }
      } else { ok = false; break; }
    }
    if (done == 0) { ok = false; break; } // peer closed early
    total += done;
  }
  CloseHandle(hEvent);
  return ok;
}
static bool PipeReadExact(HANDLE hPipe, void* buf, DWORD len, DWORD ms) {
  return PipeIoExact(hPipe, buf, len, ms, true);
}
static bool PipeWriteAll(HANDLE hPipe, const void* buf, DWORD len, DWORD ms) {
  return PipeIoExact(hPipe, const_cast<void*>(buf), len, ms, false);
}

// ---------------------------------------------------------------------------
// Persistent server: unified worker pool with small-file-first priority queue.
// ---------------------------------------------------------------------------

struct ServerConfig {
  int threads = 8;
  uint64_t smallFileBytes = 50ULL * 1024 * 1024; // 50 MB threshold for large-file downsampling
};

// Pipe I/O timeout for the server side writing the response back to the
// provider (mirrors the provider's kPipeTimeoutMs). Raised from 15s so large /
// complex files don't get their response cut off mid-write.
static constexpr DWORD kPipeTimeoutMs = 60000;

struct PipeTask {
  HANDLE hPipe = INVALID_HANDLE_VALUE; // owned by this task once enqueued
  std::wstring path;
  uint32_t size = 0;
  uint64_t fileSizeBytes = 0;
  uint64_t seq = 0; // Insertion sequence to maintain stable FIFO for same-size files

  // Min-heap comparator: smallest file size first.
  // When file sizes match, older requests (smaller seq) are prioritized.
  bool operator>(const PipeTask& other) const {
    if (fileSizeBytes != other.fileSizeBytes) {
      return fileSizeBytes > other.fileSizeBytes;
    }
    return seq > other.seq;
  }
};

static ServerConfig g_cfg;
static std::atomic<bool> g_shutdown{false};

// Threshold for large-file thumbnail downsampling protection.
static std::atomic<uint64_t> g_smallFileBytes{50ULL * 1024 * 1024};
// Named event letting the settings UI gracefully stop a running server so it
// respawns with updated settings (e.g. thread count).
static const wchar_t* kStopEventName = L"Local\\QuickViewThumbStop";
static HANDLE g_hStopEvent = nullptr;

// Unified priority task queue (Min-Heap by file size)
static std::vector<PipeTask> g_taskHeap;
static std::mutex g_taskMtx;
static std::condition_variable g_taskCv;
static const size_t kTaskCap = 256;
static std::atomic<uint64_t> g_nextSeq{0};

// Decode-target cap for large-file thumbnails: render at a smaller size to cut
// decode cost / timeout risk (the on-screen thumbnail is tiny anyway).
static const uint32_t kLargeThumbMax = 256;

// Locate QuickView.ini the same way the main app does: portable copy next to
// the exe, otherwise %APPDATA%\QuickView\QuickView.ini.
static std::wstring ResolveIniPath() {
  wchar_t exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t ls = exeDir.find_last_of(L"\\/");
  if (ls != std::wstring::npos) exeDir = exeDir.substr(0, ls);
  std::wstring portable = exeDir + L"\\QuickView.ini";
  if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;
  wchar_t ad[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad))) {
    return std::wstring(ad) + L"\\QuickView\\QuickView.ini";
  }
  return portable;
}

static ServerConfig ReadServerConfig() {
  ServerConfig cfg;
  std::wstring ini = ResolveIniPath();
  wchar_t buf[32] = {};
  if (GetPrivateProfileStringW(L"Thumbnail", L"ThumbnailThreads", L"8", buf, 32, ini.c_str())) {
    int t = wcstol(buf, nullptr, 10);
    if (t >= 1 && t <= 64) cfg.threads = t;
  }
  if (GetPrivateProfileStringW(L"Thumbnail", L"ThumbnailSmallFileThresholdMB", L"50", buf, 32, ini.c_str())) {
    long mb = wcstol(buf, nullptr, 10);
    if (mb >= 1 && mb <= 1024) cfg.smallFileBytes = (uint64_t)mb * 1024 * 1024;
  }
  return cfg;
}

// Re-read the small-file threshold so settings changes apply on the next
// request without restarting the server (thread count still needs a restart).
static void ReloadThreshold() {
  std::wstring ini = ResolveIniPath();
  wchar_t buf[32] = {};
  if (GetPrivateProfileStringW(L"Thumbnail", L"ThumbnailSmallFileThresholdMB", L"50", buf, 32, ini.c_str())) {
    long mb = wcstol(buf, nullptr, 10);
    if (mb >= 1 && mb <= 1024) g_smallFileBytes.store((uint64_t)mb * 1024 * 1024);
  }
}

// Fast zero-handle file size check using NTFS/FAT directory metadata.
// Eliminates CreateFileW/CloseHandle handle storms that cause disk stalls during bulk folder load.
static uint64_t GetFileSizeFast(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | static_cast<uint64_t>(data.nFileSizeLow);
  }
  return 0;
}

// Reply STALE(3) to a dropped request so the provider returns immediately
// instead of spawning a one-shot worker (which would re-introduce the
// per-thumbnail process storm we removed).
static void StaleAndClose(PipeTask& t) {
  HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  uint32_t status = 3; // STALE
  if (hEvent) {
    PipeWriteAll(t.hPipe, &status, sizeof(status), 2000);
    CloseHandle(hEvent);
  }
  FlushFileBuffers(t.hPipe);
  DisconnectNamedPipe(t.hPipe);
  CloseHandle(t.hPipe);
  ServerLog(L"dropped stale req path=" + t.path);
}

// Reply FAIL(1) immediately for unsupported / AppleDouble shadow files without touching queues or workers.
static void FailAndClose(PipeTask& t) {
  uint32_t status = 1; // FAIL
  HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (hEvent) {
    PipeWriteAll(t.hPipe, &status, sizeof(status), 2000);
    CloseHandle(hEvent);
  }
  FlushFileBuffers(t.hPipe);
  DisconnectNamedPipe(t.hPipe);
  CloseHandle(t.hPipe);
}

static bool IsValidImageFileForThumb(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  std::wstring filename = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
  // Skip AppleDouble files (._filename or ._tf)
  if (filename.starts_with(L"._") || filename.ends_with(L"._tf")) return false;

  std::wstring_view ext = QuickView::ExtensionOf(path);
  if (ext.empty() || !QuickView::IsSupportedExtension(ext)) return false;

  return true;
}

static void Enqueue(PipeTask t) {
  if (!IsValidImageFileForThumb(t.path)) {
    FailAndClose(t);
    return;
  }

  t.fileSizeBytes = GetFileSizeFast(t.path);
  t.seq = g_nextSeq.fetch_add(1, std::memory_order_relaxed);

  std::unique_lock<std::mutex> lk(g_taskMtx);
  if (g_taskHeap.size() >= kTaskCap) {
    // Drop the largest file currently in the queue to free space for smaller/fresher tasks.
    auto maxIt = std::max_element(g_taskHeap.begin(), g_taskHeap.end(),
                                  [](const PipeTask& a, const PipeTask& b) {
                                    return a.fileSizeBytes < b.fileSizeBytes;
                                  });
    if (maxIt != g_taskHeap.end() && maxIt->fileSizeBytes > t.fileSizeBytes) {
      PipeTask dropped = std::move(*maxIt);
      g_taskHeap.erase(maxIt);
      std::make_heap(g_taskHeap.begin(), g_taskHeap.end(), std::greater<PipeTask>());
      lk.unlock();
      StaleAndClose(dropped);
      lk.lock();
    } else {
      // New task is larger than everything in the queue, drop new task directly
      lk.unlock();
      StaleAndClose(t);
      return;
    }
  }

  g_taskHeap.push_back(std::move(t));
  std::push_heap(g_taskHeap.begin(), g_taskHeap.end(), std::greater<PipeTask>());
  g_taskCv.notify_one();
}

// [Adaptive Contrast Background for Shell Thumbnail]
// When an image has transparent regions, Explorer draws its own background
// (typically solid white on light themes), causing white graphics/text to disappear completely.
// If transparency is detected, blend the pixels with the high-contrast adaptive background
// and set Alpha to 255 so Explorer thumbnails always show the graphic clearly.
static void ApplyAdaptiveBackgroundToThumb(CImageLoader::ThumbData& thumb) {
  if (!thumb.isValid || thumb.pixels.empty() || thumb.width <= 0 || thumb.height <= 0) {
    return;
  }
  if (!thumb.hasTransparency || thumb.adaptiveBgColor == 0) {
    return;
  }

  const uint32_t bg = thumb.adaptiveBgColor;
  const uint8_t bgR = static_cast<uint8_t>((bg >> 16) & 0xFF);
  const uint8_t bgG = static_cast<uint8_t>((bg >> 8) & 0xFF);
  const uint8_t bgB = static_cast<uint8_t>(bg & 0xFF);

  const int w = thumb.width;
  const int h = thumb.height;
  const int stride = thumb.stride;
  uint8_t* raw = thumb.pixels.data();

  for (int y = 0; y < h; ++y) {
    uint8_t* row = raw + static_cast<size_t>(y) * stride;
    for (int x = 0; x < w; ++x) {
      uint8_t* px = row + x * 4;
      const uint8_t b = px[0];
      const uint8_t g = px[1];
      const uint8_t r = px[2];
      const uint8_t a = px[3];

      if (a == 255) {
        continue;
      }
      if (a == 0) {
        px[0] = bgB;
        px[1] = bgG;
        px[2] = bgR;
        px[3] = 255;
      } else {
        // Semi-transparent pixel blending (Premultiplied BGRA):
        // out_c = src_c + bg_c * (255 - a) / 255
        const uint32_t invA = 255 - a;
        const uint32_t outB = static_cast<uint32_t>(b) + (static_cast<uint32_t>(bgB) * invA + 127) / 255;
        const uint32_t outG = static_cast<uint32_t>(g) + (static_cast<uint32_t>(bgG) * invA + 127) / 255;
        const uint32_t outR = static_cast<uint32_t>(r) + (static_cast<uint32_t>(bgR) * invA + 127) / 255;
        px[0] = static_cast<uint8_t>((std::min)(255u, outB));
        px[1] = static_cast<uint8_t>((std::min)(255u, outG));
        px[2] = static_cast<uint8_t>((std::min)(255u, outR));
        px[3] = 255;
      }
    }
  }
}

// Render one request and write the response on its pipe. Each worker owns its
// own CImageLoader instance, so there is no shared per-instance mutable state
// across threads. The only process-global mutable state (g_cdrPageCache) is
// reached by CDR/CMX, but every worker runs with
// m_bPopulateCdrCache=false, so it is never touched -- hence race-free.
static void RenderAndRespond(PipeTask& t, CImageLoader& loader, bool degrade) {
  auto t0 = std::chrono::steady_clock::now();
  CImageLoader::ThumbData thumb;
  const bool transparentBg = WantsTransparentBackground(t.path);
  // Guard the decode: a malformed file must never crash the shared server
  // process (a one-shot worker tolerated this because it was isolated).
  HRESULT hr = E_FAIL;
  int targetSize = degrade ? (int)std::min((uint32_t)t.size, kLargeThumbMax) : (int)t.size;
  __try {
    hr = loader.LoadThumbnail(t.path.c_str(), targetSize, &thumb, true, transparentBg);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    hr = E_FAIL;
    ServerLog(L"LoadThumbnail threw, path=" + t.path);
  }
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  // [Retry-on-partial-write] Files being exported by upstream software (or on
  // a congested NAS share) can fail to open/read within a few ms because the
  // byte count is still growing. A short delayed retry rescues those requests
  // cheaply: by the second attempt the writer has usually finished, turning an
  // "Explorer falls back to the system thumbnail (no badge/no backing)"
  // outcome into a proper QuickView render.
  if (FAILED(hr) || !thumb.isValid || thumb.pixels.empty()) {
    if (hr != E_ABORT) {
      Sleep(250);
      auto r0 = std::chrono::steady_clock::now();
      thumb = CImageLoader::ThumbData{};
      __try {
        hr = loader.LoadThumbnail(t.path.c_str(), targetSize, &thumb, true, transparentBg);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_FAIL;
        ServerLog(L"LoadThumbnail threw (retry), path=" + t.path);
      }
      auto r1 = std::chrono::steady_clock::now();
      ms += std::chrono::duration<double, std::milli>(r1 - r0).count();
      if (SUCCEEDED(hr)) ServerLog(L"retry rescued, path=" + t.path);
    }
  }

  bool composited = false;
  if (SUCCEEDED(hr) && thumb.isValid && !thumb.pixels.empty()) {
    if (thumb.hasTransparency && thumb.adaptiveBgColor != 0) {
      ApplyAdaptiveBackgroundToThumb(thumb);
      composited = true;
    }
  }

  std::vector<BYTE> bmp;
  bool ok = SUCCEEDED(hr) && thumb.isValid && !thumb.pixels.empty() &&
            BuildBmp32Memory(thumb.pixels.data(), thumb.width, thumb.height, thumb.stride, bmp);

  HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  uint32_t status = ok ? 0u : 1u;
  if (hEvent) {
    if (!PipeWriteAll(t.hPipe, &status, sizeof(status), kPipeTimeoutMs)) {
      // client gone (folder switched / scrolled away) -> nothing to write
    } else if (ok) {
      uint32_t bmpLen = static_cast<uint32_t>(bmp.size());
      if (!PipeWriteAll(t.hPipe, &bmpLen, sizeof(bmpLen), kPipeTimeoutMs) ||
          !PipeWriteAll(t.hPipe, bmp.data(), bmpLen, kPipeTimeoutMs)) {
        ServerLog(L"write bmp failed");
      }
    }
    CloseHandle(hEvent);
  }
  ServerLog(L"req path=" + t.path + L" size=" + std::to_wstring(t.size) +
            (ok ? L" OK " : L" FAIL ") + L"(" + std::to_wstring(static_cast<int>(ms)) + L"ms)" +
            L" trans=" + (thumb.hasTransparency ? L"1" : L"0") +
            L" bg=" + (composited ? L"1" : L"0"));
  // Byte-mode pipes discard any bytes the client has not yet read when
  // DisconnectNamedPipe runs. Flush first so the caller receives the full
  // response (status + bmpLen + bmp); otherwise it sees a truncated/EOF read
  // and the thumbnail silently fails (intermittent in production).
  FlushFileBuffers(t.hPipe);
  DisconnectNamedPipe(t.hPipe);
  CloseHandle(t.hPipe);
}

static void WorkerThread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  {
    // [Fix] loader (and its m_wicFactory ComPtr) is scoped so it is destroyed
    // BEFORE CoUninitialize: releasing a COM object on a torn-down apartment AVs.
    CImageLoader loader;
    // CDR/CMX share this pool; disable the shared page-cache so
    // they never race on g_cdrPageCache (ImageLoader.cpp:62). No-op for other
    // formats.
    loader.m_bPopulateCdrCache = false;
    {
      IWICImagingFactory* wf = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                     reinterpret_cast<void**>(&wf)))) {
        loader.Initialize(wf);
        wf->Release();
      }
    }
    while (true) {
      PipeTask t;
      {
        std::unique_lock<std::mutex> lk(g_taskMtx);
        g_taskCv.wait(lk, [] { return g_shutdown.load() || !g_taskHeap.empty(); });
        if (g_shutdown.load() && g_taskHeap.empty()) break;
        if (g_taskHeap.empty()) continue;
        std::pop_heap(g_taskHeap.begin(), g_taskHeap.end(), std::greater<PipeTask>());
        t = std::move(g_taskHeap.back());
        g_taskHeap.pop_back();
      }
      const bool degrade = (t.fileSizeBytes >= g_smallFileBytes.load());
      RenderAndRespond(t, loader, degrade);
    }
  }  // loader destroyed here, before CoUninitialize
  CoUninitialize();
}

} // namespace

int QuickView::RunThumbnailWorker(int argc, LPWSTR* argv) {
  std::wstring inputPath;
  int size = 0;
  if (!TryReadArgValue(argc, argv, L"--input", &inputPath) ||
      !TryReadPositiveIntArg(argc, argv, L"--size", &size)) {
    return 2;
  }

  // WIC is used by some thumbnail fallbacks inside LoadThumbnail.
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  // [Fix] Initialize a WIC factory so the WIC fallback inside LoadThumbnail
  // (LoadToMemory -> m_wicFactory->CreateStream) cannot deref a null factory.
  // Without this the one-shot thumbnail worker crashed (0xc0000005) on any
  // format that reaches the WIC fallback, breaking Explorer thumbnails for
  // CDR/AI/etc. (same root cause already fixed for the server's three
  // internal worker threads).
  // The loader (and its m_wicFactory ComPtr) is scoped so it is destroyed
  // BEFORE CoUninitialize: releasing a COM object on a torn-down apartment AVs.
  bool ok = false;
  {
    CImageLoader loader;
    {
      IWICImagingFactory* wf = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                     reinterpret_cast<void**>(&wf)))) {
        loader.Initialize(wf);
        wf->Release();
      }
    }
    CImageLoader::ThumbData thumb;
    const bool transparentBg = WantsTransparentBackground(inputPath);
    HRESULT hr =
        loader.LoadThumbnail(inputPath.c_str(), size, &thumb, true, transparentBg);

    // Write BMP data to stdout (pipe) instead of a temp file.
    // Format: 4-byte status (0=OK, 1=FAIL) + 4-byte bmpLen + bmp data.
    if (SUCCEEDED(hr) && thumb.isValid && !thumb.pixels.empty()) {
      if (thumb.hasTransparency && thumb.adaptiveBgColor != 0) {
        ApplyAdaptiveBackgroundToThumb(thumb);
      }
      std::vector<BYTE> bmp;
      if (BuildBmp32Memory(thumb.pixels.data(), thumb.width, thumb.height,
                           thumb.stride, bmp)) {
        uint32_t status = 0;
        uint32_t bmpLen = static_cast<uint32_t>(bmp.size());
        DWORD written = 0;
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        ok = WriteFile(hOut, &status, sizeof(status), &written, nullptr) &&
             WriteFile(hOut, &bmpLen, sizeof(bmpLen), &written, nullptr) &&
             WriteFile(hOut, bmp.data(), bmpLen, &written, nullptr);
      }
    } else {
      uint32_t status = 1; // FAIL
      DWORD written = 0;
      HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
      WriteFile(hOut, &status, sizeof(status), &written, nullptr);
    }
  }  // loader (m_wicFactory) released here while COM is still initialized

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

  g_cfg = ReadServerConfig();
  g_smallFileBytes.store(g_cfg.smallFileBytes);
  g_shutdown = false;

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  const DWORD idleTimeoutMs = static_cast<DWORD>(idleSec) * 1000;
  const wchar_t* name = L"\\\\.\\pipe\\QuickViewThumb";

  auto makeInstance = [&]() -> HANDLE {
    return CreateNamedPipeW(name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        64, 65536, 65536, 0, nullptr);
  };

  HANDLE hPipe = makeInstance();
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

  // Named stop event so the settings UI can gracefully terminate this server
  // (e.g. when the parallel-thread count changes) and force a fresh process.
  g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
  if (!g_hStopEvent) ServerLog(L"warn: cannot create stop event");

  ServerLog(L"Server started threads=" + std::to_wstring(g_cfg.threads) +
            L" smallMB=" + std::to_wstring(g_cfg.smallFileBytes / (1024 * 1024)) +
            L" idle=" + std::to_wstring(idleSec) + L"s");

  // Worker pool: unified N threads processing the min-heap priority queue.
  // Each owns its own CImageLoader instance so there is no shared
  // per-instance mutable state.
  std::vector<std::thread> workers;
  for (int i = 0; i < g_cfg.threads; ++i)
    workers.emplace_back(WorkerThread);

  OVERLAPPED ol = {};
  ol.hEvent = hEvent;
  while (true) {
    if (g_hStopEvent && WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) {
      ServerLog(L"Stop event signaled, exiting");
      break;
    }
    // Ensure we have a pipe instance to accept the next client on. A fresh
    // instance is created per accepted connection so acceptance never blocks
    // on rendering (the connected handle is handed to a worker).
    if (hPipe == INVALID_HANDLE_VALUE) {
      for (int tries = 0; tries < 1000; ++tries) {
        hPipe = makeInstance();
        if (hPipe != INVALID_HANDLE_VALUE) break;
        Sleep(10);
      }
      if (hPipe == INVALID_HANDLE_VALUE) {
        ServerLog(L"pipe instance exhaustion, stopping");
        break;
      }
    }

    ReloadThreshold(); // pick up threshold changes without restarting
    ResetEvent(hEvent);
    ol = {};
    ol.hEvent = hEvent;
    BOOL connected = ConnectNamedPipe(hPipe, &ol);
    bool isConnected = false;
    if (connected) {
      isConnected = true;
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        isConnected = true; // client connected between CreateNamedPipe and here
      } else if (err == ERROR_IO_PENDING) {
        HANDLE waits[2] = { hEvent, g_hStopEvent };
        DWORD nWaits = (g_hStopEvent ? 2 : 1);
        DWORD w = WaitForMultipleObjects(nWaits, waits, FALSE, idleTimeoutMs);
        if (w == WAIT_TIMEOUT) {
          CancelIo(hPipe);
          DWORD dummy = 0;
          GetOverlappedResult(hPipe, &ol, &dummy, TRUE); // reap aborted connect
          ServerLog(L"Idle timeout, exiting");
          break;
        } else if (w == WAIT_OBJECT_0) {
          isConnected = true;
        } else if (w == WAIT_OBJECT_0 + 1) {
          ServerLog(L"Stop event signaled, exiting");
          break;
        } else {
          break; // wait failed
        }
      } else {
        ServerLog(L"ConnectNamedPipe err=" + std::to_wstring(err));
        break;
      }
    }

    if (isConnected) {
      uint32_t pathByteLen = 0;
      if (!PipeReadExact(hPipe, &pathByteLen, sizeof(pathByteLen), kPipeTimeoutMs)) {
        DisconnectNamedPipe(hPipe);
        continue;
      }
      if (pathByteLen == 0 || pathByteLen > 65536) {
        DisconnectNamedPipe(hPipe);
        continue;
      }
      std::wstring path;
      path.resize(pathByteLen / 2);
      if (!PipeReadExact(hPipe, path.data(), pathByteLen, kPipeTimeoutMs)) {
        DisconnectNamedPipe(hPipe);
        continue;
      }
      uint32_t size = 0;
      if (!PipeReadExact(hPipe, &size, sizeof(size), kPipeTimeoutMs)) {
        DisconnectNamedPipe(hPipe);
        continue;
      }
      if (size == 0 || size > 4096) size = 256;

      // Hand the connected pipe to a worker and grab a fresh instance for the
      // next client (multi-instance overlapping server).
      Enqueue({hPipe, path, size});
      hPipe = INVALID_HANDLE_VALUE;
    }
  }

  // Shutdown: stop accepting, let queued/in-flight tasks drain, then join.
  g_shutdown = true;
  g_taskCv.notify_all();
  for (auto& t : workers) {
    if (t.joinable()) t.join();
  }

  if (g_hStopEvent) { CloseHandle(g_hStopEvent); g_hStopEvent = nullptr; }
  CloseHandle(hEvent);
  if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
  if (SUCCEEDED(coHr)) CoUninitialize();
  ServerLog(L"Server stopped");
  return 0;
}

void QuickView::KillThumbnailServer() {
  // Signal the running server's stop event so it exits gracefully; the next
  // shell request then spawns a fresh process picking up updated settings.
  HANDLE h = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kStopEventName);
  if (h) {
    SetEvent(h);
    CloseHandle(h);
  }
}
