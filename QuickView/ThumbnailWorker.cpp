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
// Persistent server: dual-channel worker pool (parallel + serial).
// ---------------------------------------------------------------------------

struct ServerConfig {
  int threads = 4;
  uint64_t smallFileBytes = 5ULL * 1024 * 1024; // 5 MB default
};

// Pipe I/O timeout for the server side writing the response back to the
// provider (mirrors the provider's kPipeTimeoutMs). Raised from 15s so large /
// complex files don't get their response cut off mid-write.
static constexpr DWORD kPipeTimeoutMs = 60000;

struct PipeTask {
  HANDLE hPipe = INVALID_HANDLE_VALUE; // owned by this task once enqueued
  std::wstring path;
  uint32_t size = 0;
};

static ServerConfig g_cfg;
static std::atomic<bool> g_shutdown{false};

// Hot-reloadable small-file threshold (atomic so the accept thread can update
// it while worker threads read it without locking).
static std::atomic<uint64_t> g_smallFileBytes{5ULL * 1024 * 1024};
// Named event letting the settings UI gracefully stop a running server so it
// respawns with updated settings (e.g. thread count).
static const wchar_t* kStopEventName = L"Local\\QuickViewThumbStop";
static HANDLE g_hStopEvent = nullptr;

// Parallel channel (small, non-CDR/CMX): N worker threads.
static std::queue<PipeTask> g_parallelQ;
static std::mutex g_parallelMtx;
static std::condition_variable g_parallelCv;
static const size_t kParallelCap = 32;

// CDR/CMX dedicated channel: a single worker. Only this thread ever touches
// g_cdrPageCache (ImageLoader.cpp:62), so the shared global stays race-free
// without locking. Splitting it from the large-file channel means CDR
// thumbnails are never starved by (or block) large-file rendering.
static std::queue<PipeTask> g_cdrQ;
static std::mutex g_cdrMtx;
static std::condition_variable g_cdrCv;

// Large-file (non-CDR/CMX) dedicated channel: a single worker. Decoupled from
// CDR so heavy large-file work doesn't serialize behind CDR parsing.
static std::queue<PipeTask> g_largeQ;
static std::mutex g_largeMtx;
static std::condition_variable g_largeCv;

// Capacity of each slow channel. High enough that a folder with dozens of
// CDR/large files queues instead of being dropped stale (the real cause of
// ">16 CDR at once" failures). Overflow beyond this is rare and handled by the
// bounded one-shot fallback in the provider.
static const size_t kSlowCap = 64;

// Number of CDR/CMX worker threads. Each owns its own CImageLoader with
// m_bPopulateCdrCache=false (so it never touches the shared g_cdrPageCache),
// letting CDR render in parallel. The original single-thread limit existed
// only because of that shared global; isolating it makes N threads safe.
static const int kCdrThreads = 2;

// Decode-target cap for large-file thumbnails: render at a smaller size to cut
// decode cost / timeout risk (the on-screen thumbnail is tiny anyway).
static const uint32_t kLargeThumbMax = 128;

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
  if (GetPrivateProfileStringW(L"Thumbnail", L"ThumbnailThreads", L"4", buf, 32, ini.c_str())) {
    int t = wcstol(buf, nullptr, 10);
    if (t >= 1 && t <= 64) cfg.threads = t;
  }
  if (GetPrivateProfileStringW(L"Thumbnail", L"ThumbnailSmallFileThresholdMB", L"5", buf, 32, ini.c_str())) {
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
  if (GetPrivateProfileStringW(L"Thumbnail", L"ThumbnailSmallFileThresholdMB", L"5", buf, 32, ini.c_str())) {
    long mb = wcstol(buf, nullptr, 10);
    if (mb >= 1 && mb <= 1024) g_smallFileBytes.store((uint64_t)mb * 1024 * 1024);
  }
}

static uint64_t GetFileSizeSafe(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0;
  LARGE_INTEGER sz = {};
  BOOL ok = GetFileSizeEx(h, &sz);
  CloseHandle(h);
  return ok ? static_cast<uint64_t>(sz.QuadPart) : 0;
}

// Small AND parallel-safe format => parallel channel. CDR/CMX are excluded
// because they rewrite the shared global g_cdrPageCache (ImageLoader.cpp:62),
// which has no lock. Large files go to their own channel (one at a time).
static bool IsSmallAndParallelSafe(const std::wstring& path) {
  if (GetFileSizeSafe(path) >= g_smallFileBytes.load()) return false;
  std::wstring_view ext = QuickView::ExtensionOf(path);
  if (QuickView::ExtEqualsIgnoreCase(ext, L".cdr") ||
      QuickView::ExtEqualsIgnoreCase(ext, L".cmx")) return false;
  return true;
}

// CDR/CMX are routed to their own dedicated single-threaded channel so they
// never contend with large-file rendering.
static bool IsCdr(const std::wstring& path) {
  std::wstring_view ext = QuickView::ExtensionOf(path);
  return QuickView::ExtEqualsIgnoreCase(ext, L".cdr") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cmx");
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

static void Enqueue(PipeTask t) {
  if (IsSmallAndParallelSafe(t.path)) {
    std::unique_lock<std::mutex> lk(g_parallelMtx);
    while (g_parallelQ.size() >= kParallelCap) {
      PipeTask old = std::move(g_parallelQ.front());
      g_parallelQ.pop();
      lk.unlock();
      StaleAndClose(old);
      lk.lock();
    }
    g_parallelQ.push(std::move(t));
    g_parallelCv.notify_one();
  } else if (IsCdr(t.path)) {
    std::unique_lock<std::mutex> lk(g_cdrMtx);
    while (g_cdrQ.size() >= kSlowCap) {
      PipeTask old = std::move(g_cdrQ.front());
      g_cdrQ.pop();
      lk.unlock();
      StaleAndClose(old);
      lk.lock();
    }
    g_cdrQ.push(std::move(t));
    g_cdrCv.notify_one();
  } else {
    std::unique_lock<std::mutex> lk(g_largeMtx);
    while (g_largeQ.size() >= kSlowCap) {
      PipeTask old = std::move(g_largeQ.front());
      g_largeQ.pop();
      lk.unlock();
      StaleAndClose(old);
      lk.lock();
    }
    g_largeQ.push(std::move(t));
    g_largeCv.notify_one();
  }
}

// Render one request and write the response on its pipe. Each worker owns its
// own CImageLoader instance, so there is no shared per-instance mutable state
// across threads. The only process-global mutable state (g_cdrPageCache) is
// reached solely via the serial worker (CDR/CMX), hence race-free.
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
            (ok ? L" OK " : L" FAIL ") + L"(" + std::to_wstring(static_cast<int>(ms)) + L"ms)");
  // Byte-mode pipes discard any bytes the client has not yet read when
  // DisconnectNamedPipe runs. Flush first so the caller receives the full
  // response (status + bmpLen + bmp); otherwise it sees a truncated/EOF read
  // and the thumbnail silently fails (intermittent in production).
  FlushFileBuffers(t.hPipe);
  DisconnectNamedPipe(t.hPipe);
  CloseHandle(t.hPipe);
}

static void ParallelWorker() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
  while (true) {
    PipeTask t;
    {
      std::unique_lock<std::mutex> lk(g_parallelMtx);
      g_parallelCv.wait(lk, [] { return g_shutdown.load() || !g_parallelQ.empty(); });
      if (g_shutdown.load() && g_parallelQ.empty()) break;
      if (g_parallelQ.empty()) continue;
      t = std::move(g_parallelQ.front());
      g_parallelQ.pop();
    }
    RenderAndRespond(t, loader, false);
  }
  CoUninitialize();
}

static void CdrWorker() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  CImageLoader loader;
  loader.m_bPopulateCdrCache = false; // server never uses the page-nav cache
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
      std::unique_lock<std::mutex> lk(g_cdrMtx);
      g_cdrCv.wait(lk, [] { return g_shutdown.load() || !g_cdrQ.empty(); });
      if (g_shutdown.load() && g_cdrQ.empty()) break;
      if (g_cdrQ.empty()) continue;
      t = std::move(g_cdrQ.front());
      g_cdrQ.pop();
    }
    RenderAndRespond(t, loader, false);
  }
  CoUninitialize();
}

static void LargeWorker() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
  while (true) {
    PipeTask t;
    {
      std::unique_lock<std::mutex> lk(g_largeMtx);
      g_largeCv.wait(lk, [] { return g_shutdown.load() || !g_largeQ.empty(); });
      if (g_shutdown.load() && g_largeQ.empty()) break;
      if (g_largeQ.empty()) continue;
      t = std::move(g_largeQ.front());
      g_largeQ.pop();
    }
    // Large files render at a reduced target size (see kLargeThumbMax) to
    // avoid decode timeouts on very big images.
    RenderAndRespond(t, loader, true);
  }
  CoUninitialize();
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

  // Worker pool: N parallel threads + 1 serial thread. Each owns its own
  // CImageLoader instance so there is no shared per-instance mutable state.
  std::vector<std::thread> workers;
  for (int i = 0; i < g_cfg.threads; ++i)
    workers.emplace_back(ParallelWorker);
  for (int i = 0; i < kCdrThreads; ++i)
    workers.emplace_back(CdrWorker);
  workers.emplace_back(LargeWorker);

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
  g_parallelCv.notify_all();
  g_cdrCv.notify_all();
  g_largeCv.notify_all();
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
