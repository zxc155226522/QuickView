#include "pch.h"
#include "ImageListThumbnailPanel.h"
#include "FileNavigator.h"
#include "ImageLoader.h"
#include "FormatIcons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>

extern float g_uiScale;
extern AppConfig g_config;
extern std::unique_ptr<CImageLoader> g_imageLoader;

// ============================================================================
// [缩略图性能日志] 诊断网络盘大目录缩略图加载慢：
// 每张缩略图记录 文件名 / 结果(loaderName 或 失败) / 解码耗时；
// ShowImageThumbnails 记录列表规模。日志：E:\qv_thumb_perf.log
// ============================================================================
namespace {
std::mutex g_perfLogMutex;

void QvThumbPerfLog(const wchar_t* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_perfLogMutex);
    FILE* fp = _wfopen(L"E:\\qv_thumb_perf.log", L"a");
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(fp, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(fp, fmt, args);
    va_end(args);
    fputws(L"\n", fp);
    fclose(fp);
}
} // namespace

ImageListThumbnailPanel::~ImageListThumbnailPanel() {
    m_thumbRunning = false;
    m_thumbCV.notify_all();
    for (auto& t : m_thumbThreads) {
        if (t.joinable()) t.join();
    }
    m_thumbThreads.clear();
    for (auto& pair : m_imageThumbCache) { if (pair.second) pair.second->Release(); }
    m_imageThumbCache.clear();
    m_imageThumbBg.clear();
    DiscardDeviceResources();
}

void ImageListThumbnailPanel::InitializeEx(HWND hwnd) {
    Initialize(hwnd);
    _wremove(L"E:\\qv_thumb_perf.log"); // 每次启动重新累积
    QvThumbPerfLog(L"[Panel] InitializeEx, %d worker threads",
                   kThumbWorkerThreads);
    // Start background thumbnail worker thread pool
    m_thumbRunning = true;
    m_thumbThreads.reserve(kThumbWorkerThreads);
    for (int i = 0; i < kThumbWorkerThreads; ++i) {
        m_thumbThreads.emplace_back(&ImageListThumbnailPanel::ThumbWorkerLoop, this);
    }
}

void ImageListThumbnailPanel::ShowImageThumbnails(FileNavigator* nav, int currentIndex, uint32_t totalFiles) {
    if (!nav || totalFiles == 0) {
        OnDocumentClosed();
        return;
    }
    // [异步目录扫描] totalFiles==1 允许显示：打开文件时列表处于快速状态
    // （只含当前文件），缩略图栏先显示，全量列表到达后再次调用本函数填入

    bool wasVisible = m_visible && m_isImageMode;
    m_visible = true;
    m_isImageMode = true;
    m_navigator = nav;
    m_currentImageIndex = currentIndex;
    m_totalImages = totalFiles;
    m_totalPages = totalFiles;
    m_currentPage = (uint32_t)std::max(0, currentIndex);

    // Cache file paths for rendering
    m_imagePaths.clear();
    m_imagePaths.resize(totalFiles);
    for (uint32_t i = 0; i < totalFiles; ++i) {
        m_imagePaths[i] = nav->GetFile((int)i);
    }

    // Clear cache
    for (auto& pair : m_imageThumbCache) { if (pair.second) pair.second->Release(); }
    m_imageThumbCache.clear();
    m_imageThumbBg.clear();
    // Clear async queue
    {
        std::lock_guard<std::mutex> lock(m_thumbQueueMutex);
        m_thumbQueue = {};
        m_thumbPending.clear();
    }

    if (!wasVisible) {
        m_scrollY = 0.0f;
        m_targetScrollY = 0.0f;
        m_scrollX = 0.0f;
        m_targetScrollX = 0.0f;
    }
    // Always re-center on the newly opened file, even if the panel was already
    // visible (opening another image in the same/another folder).
    ScrollToCurrentPage(true);
    // [初始加载窗口] 本轮只加载当前项前后各 kInitialLoadRadius 张
    m_initialBurstPending = true;
    QvThumbPerfLog(L"[SHOW] total=%u cur=%d burstRadius=%u",
                   totalFiles, currentIndex, kInitialLoadRadius);
}

void ImageListThumbnailPanel::OnDocumentClosed() {
    m_visible = false;
    m_isImageMode = false;
    m_initialBurstPending = false;
    m_totalPages = 0;
    m_currentPage = 0;
    m_navigator = nullptr;
    m_currentImageIndex = -1;
    m_totalImages = 0;
    m_imagePaths.clear();
    for (auto& pair : m_imageThumbCache) { if (pair.second) pair.second->Release(); }
    m_imageThumbCache.clear();
    m_imageThumbBg.clear();
    m_scrollY = 0.0f;
    m_targetScrollY = 0.0f;
    m_scrollX = 0.0f;
    m_targetScrollX = 0.0f;
    // Clear async queue
    {
        std::lock_guard<std::mutex> lock(m_thumbQueueMutex);
        m_thumbQueue = {};
        m_thumbPending.clear();
    }
}

void ImageListThumbnailPanel::SetCurrentImageIndex(int index) {
    if (index < 0 || index >= (int)m_totalImages) return;
    if (m_currentImageIndex != index) {
        m_currentImageIndex = index;
        m_currentPage = (uint32_t)index;
        ScrollToCurrentPage(false); // keep the selected thumbnail centered
    }
}

void ImageListThumbnailPanel::OnDeviceResourcesDiscarded() {
    // RT changed — all D2D bitmaps in cache are invalid
    for (auto& pair : m_imageThumbCache) { if (pair.second) { pair.second->Release(); pair.second = nullptr; } }
}

void ImageListThumbnailPanel::OnLayoutChanged() {
    // No special action
}

ComPtr<ID2D1Bitmap> ImageListThumbnailPanel::GetItemBitmap(uint32_t index) {
    auto it = m_imageThumbCache.find(index);
    if (it != m_imageThumbCache.end()) {
        ComPtr<ID2D1Bitmap> bitmap;
        bitmap.Attach(it->second); // Take ownership from cache (raw pointer)
        it->second = nullptr;      // Cache no longer owns it
        return bitmap;
    }
    return nullptr;
}

std::wstring ImageListThumbnailPanel::GetItemLabel(uint32_t index) const {
    if (index < m_imagePaths.size()) {
        // Extract filename from path; the label format trims with an ellipsis
        // when it is too wide, so return the full name here
        const std::wstring& path = m_imagePaths[index];
        size_t slashPos = path.find_last_of(L"\\/");
        if (slashPos != std::wstring::npos && slashPos + 1 < path.size()) {
            return path.substr(slashPos + 1);
        }
        return path;
    }
    return L"";
}

std::wstring ImageListThumbnailPanel::GetItemFullPath(uint32_t index) const {
    if (index < m_imagePaths.size()) {
        return m_imagePaths[index];
    }
    return L"";
}

bool ImageListThumbnailPanel::OnIsLoading() const {
    if (m_panelSide == 3) {
        const float itemWidth = BottomItemStride();
        const int visibleStart = std::max(0, static_cast<int>(m_scrollX / itemWidth)) - 2;
        const int visibleEnd = std::min(static_cast<int>(m_totalImages),
            static_cast<int>((m_scrollX + m_panelWidth) / itemWidth) + 3);
        for (int i = visibleStart; i < visibleEnd; ++i) {
            if (i < 0 || i >= (int)m_totalImages) continue;
            uint32_t idx = (uint32_t)i;
            if (m_imageThumbCache.find(idx) == m_imageThumbCache.end()) return true;
        }
        return false;
    }
    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const int visibleStart = std::max(0, static_cast<int>(m_scrollY / itemHeight)) - 2;
    const int visibleEnd = std::min(static_cast<int>(m_totalImages),
        static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 3);
    for (int i = visibleStart; i < visibleEnd; ++i) {
        if (i < 0 || i >= (int)m_totalImages) continue;
        uint32_t idx = (uint32_t)i;
        if (m_imageThumbCache.find(idx) == m_imageThumbCache.end()) return true;
    }
    return false;
}

void ImageListThumbnailPanel::DrawItems(ID2D1RenderTarget* pRT) {
    if (!m_visible || !pRT) return;
    uint32_t itemCount = m_totalImages;
    if (itemCount == 0) return;

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float itemWidthBottom = BottomItemStride();
    const int startPage = m_panelSide == 3
        ? std::max(0, static_cast<int>(m_scrollX / itemWidthBottom) - 1)
        : std::max(0, static_cast<int>(m_scrollY / itemHeight) - 1);
    const int endPage = m_panelSide == 3
        ? std::min(static_cast<int>(itemCount),
            static_cast<int>((m_scrollX + m_panelWidth) / itemWidthBottom) + 2)
        : std::min(static_cast<int>(itemCount),
            static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 2);

    for (int i = startPage; i < endPage; ++i) {
        const uint32_t pageIndex = static_cast<uint32_t>(i);
        const D2D1_RECT_F itemRect = GetItemRect(pageIndex);
        if (m_panelSide == 3) {
            if (itemRect.right < m_panelRect.left || itemRect.left > m_panelRect.right) continue;
        } else {
            if (itemRect.bottom < m_panelRect.top || itemRect.top > m_panelRect.bottom) continue;
        }

        const bool isCurrentPage = (pageIndex == m_currentPage);
        const bool isHovered = (static_cast<int>(pageIndex) == m_hoverIndex);

        // Hover background
        if (!isCurrentPage && isHovered && m_brushHover) {
            D2D1_ROUNDED_RECT hovRect = D2D1::RoundedRect(itemRect, 4.0f * g_uiScale, 4.0f * g_uiScale);
            pRT->FillRoundedRectangle(hovRect, m_brushHover.Get());
        }

        // Thumbnail image
        const D2D1_RECT_F thumbRect = GetThumbnailRect(itemRect);
        ID2D1Bitmap* pBitmap = nullptr;
        auto it = m_imageThumbCache.find(pageIndex);
        if (it != m_imageThumbCache.end()) {
            pBitmap = it->second;
        }

        if (pBitmap) {
            // [Adaptive Contrast Background on Square Card]
            // 铺设规整正方形微圆角卡片底板，根据图像主体明暗智能自动切换黑/白/灰衬底
            auto bgIt = m_imageThumbBg.find(pageIndex);
            if (m_brushThumbnailBg) {
                uint32_t bg = 0xFFF5F5F7; // 默认亮白微灰卡片底
                if (bgIt != m_imageThumbBg.end() && bgIt->second.adaptiveBgColor != 0) {
                    bg = bgIt->second.adaptiveBgColor;
                }
                const float r = static_cast<float>((bg >> 16) & 0xFF) / 255.0f;
                const float g = static_cast<float>((bg >> 8) & 0xFF) / 255.0f;
                const float b = static_cast<float>(bg & 0xFF) / 255.0f;
                const D2D1_COLOR_F oldColor = m_brushThumbnailBg->GetColor();
                m_brushThumbnailBg->SetColor(D2D1::ColorF(r, g, b, 1.0f));
                D2D1_ROUNDED_RECT card = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
                pRT->FillRoundedRectangle(card, m_brushThumbnailBg.Get());
                m_brushThumbnailBg->SetColor(oldColor);
            }

            // Letterbox inside the square cell — aspect preserved, no distortion
            // 内部留出适度内边距（pad），使图像精致居中于卡片底板内部，消除贴边感
            const float pad = 2.5f * g_uiScale;
            const D2D1_RECT_F innerThumbRect = D2D1::RectF(
                thumbRect.left + pad, thumbRect.top + pad,
                thumbRect.right - pad, thumbRect.bottom - pad);
            const D2D1_SIZE_F bs = pBitmap->GetSize();
            const D2D1_RECT_F drawRect = FitRectInside(innerThumbRect, bs.width, bs.height);

            pRT->DrawBitmap(pBitmap, drawRect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            if (m_brushThumbnailBg) {
                const D2D1_COLOR_F oldColor = m_brushThumbnailBg->GetColor();
                if (pageIndex < m_imagePaths.size() && QuickView::WantsTransparentBackground(m_imagePaths[pageIndex])) {
                    m_brushThumbnailBg->SetColor(D2D1::ColorF(0.96f, 0.96f, 0.97f, 1.0f));
                }
                D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
                pRT->FillRoundedRectangle(bgRect, m_brushThumbnailBg.Get());
                m_brushThumbnailBg->SetColor(oldColor);
            }
        }

        // Current page border (green)
        if (isCurrentPage && m_brushBorder) {
            D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
            pRT->DrawRoundedRectangle(borderRect, m_brushBorder.Get(), 2.0f * g_uiScale);
        }

        // Type Badge (top-right capsule, category-colored)
        if (pageIndex < m_imagePaths.size()) {
            const std::wstring& path = m_imagePaths[pageIndex];
            size_t dot = path.rfind(L'.');
            if (dot != std::wstring::npos && dot + 1 < path.size()) {
                std::wstring ext = path.substr(dot + 1);
                for (auto& c : ext) c = (wchar_t)std::towupper((wint_t)c);
                if (!ext.empty()) {
                    D2D1_COLOR_F clr = QuickView::BadgeColorFor(ext);
                    const float bw = (6.0f + 5.5f * (float)ext.length()) * g_uiScale;
                    const float bh = 13.0f * g_uiScale;
                    const float bm = 3.0f * g_uiScale;
                    const float br = bh * 0.5f;
                    D2D1_RECT_F badge = D2D1::RectF(thumbRect.right - bm - bw, thumbRect.top + bm,
                                                   thumbRect.right - bm, thumbRect.top + bm + bh);
                    if (m_brushBadgeBg && m_brushBadgeText && m_textFormatBadge) {
                        m_brushBadgeBg->SetColor(D2D1::ColorF(clr.r, clr.g, clr.b, 0.85f));
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), m_brushBadgeBg.Get());
                        pRT->DrawText(ext.c_str(), static_cast<UINT32>(ext.length()), m_textFormatBadge.Get(),
                                     badge, m_brushBadgeText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
        }

        // File name label (single line, ellipsis when too long)
        if (m_textFormatLabel && m_brushText) {
            std::wstring label = GetItemLabel(pageIndex);
            D2D1_RECT_F labelRect = D2D1::RectF(itemRect.left, thumbRect.bottom + 2.0f * g_uiScale,
                                                 itemRect.right, itemRect.bottom);
            pRT->DrawText(label.c_str(), static_cast<UINT32>(label.size()), m_textFormatLabel.Get(),
                         labelRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

int ImageListThumbnailPanel::OnItemClick(int index) {
    // Return the file index — main.cpp will handle navigation
    return index;
}

void ImageListThumbnailPanel::OnUpdateThumbnailRequests() {
    if (!m_visible || m_totalImages == 0 || !m_navigator) return;

    if (m_panelSide == 3) {
        const float itemWidth = BottomItemStride();
        int start = std::max(0, static_cast<int>(m_scrollX / itemWidth)) - 2;
        int end = std::min(static_cast<int>(m_totalImages),
            static_cast<int>((m_scrollX + m_panelWidth) / itemWidth) + 3);
        // [初始加载窗口] 首屏只加载当前项前后各 kInitialLoadRadius 张，
        // 入队后恢复常规的可视范围按需加载
        if (m_initialBurstPending) {
            const int cur = std::max(0, m_currentImageIndex);
            start = std::max(start, cur - static_cast<int>(kInitialLoadRadius));
            end = std::min(end, cur + static_cast<int>(kInitialLoadRadius) + 1);
            m_initialBurstPending = false;
        }

        for (int i = start; i < end; ++i) {
            if (i < 0 || i >= (int)m_totalImages) continue;
            uint32_t idx = (uint32_t)i;
            if (m_imageThumbCache.find(idx) == m_imageThumbCache.end()) {
                EnqueueThumb(idx);
            }
        }
        // Clean up off-screen cache entries
        if (m_imageThumbCache.size() > kMaxCacheSize) {
            int center = static_cast<int>(m_scrollX / itemWidth) + static_cast<int>(m_panelWidth / itemWidth / 2);
            for (auto it = m_imageThumbCache.begin(); it != m_imageThumbCache.end(); ) {
                int dist = std::abs(static_cast<int>(it->first) - center);
                if (dist > 30) {
                    if (it->second) it->second->Release();
                    m_imageThumbBg.erase(it->first);
                    it = m_imageThumbCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return;
    }

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    int start = std::max(0, static_cast<int>(m_scrollY / itemHeight)) - 2;
    int end = std::min(static_cast<int>(m_totalImages),
        static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 3);
    // [初始加载窗口] 同底部条：首屏只加载当前项前后各 kInitialLoadRadius 张
    if (m_initialBurstPending) {
        const int cur = std::max(0, m_currentImageIndex);
        start = std::max(start, cur - static_cast<int>(kInitialLoadRadius));
        end = std::min(end, cur + static_cast<int>(kInitialLoadRadius) + 1);
        m_initialBurstPending = false;
    }

    for (int i = start; i < end; ++i) {
        if (i < 0 || i >= (int)m_totalImages) continue;
        uint32_t idx = (uint32_t)i;
        if (m_imageThumbCache.find(idx) == m_imageThumbCache.end()) {
            EnqueueThumb(idx);
        }
    }

    if (m_imageThumbCache.size() > kMaxCacheSize) {
        int center = static_cast<int>(m_scrollY / itemHeight) + static_cast<int>(m_panelHeight / itemHeight / 2);
        for (auto it = m_imageThumbCache.begin(); it != m_imageThumbCache.end(); ) {
            int dist = std::abs(static_cast<int>(it->first) - center);
            if (dist > 30) {
                if (it->second) it->second->Release();
                m_imageThumbBg.erase(it->first);
                it = m_imageThumbCache.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void ImageListThumbnailPanel::ProcessAsyncResults(ID2D1RenderTarget* pRT) {
    if (m_thumbResults.empty()) return;

    std::vector<AsyncThumbResult> results;
    {
        std::lock_guard<std::mutex> lock(m_thumbResultMutex);
        results.swap(m_thumbResults);
    }
    std::vector<uint32_t> consumedPages;
    for (auto& r : results) {
        consumedPages.push_back(r.pageIndex);
        // [防竞态] 列表在解码期间被换入（切换目录/全量列表到达）：
        // 结果与当前列表不一致时直接丢弃，不污染新缓存
        if (r.pageIndex >= m_imagePaths.size() ||
            m_imagePaths[r.pageIndex] != r.sourcePath) {
            continue;
        }
        if (!r.valid || r.width <= 0 || r.height <= 0 || r.pixels.empty()) {
            m_imageThumbCache[r.pageIndex] = nullptr;
            continue;
        }
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        D2D1_SIZE_U size = D2D1::SizeU(r.width, r.height);
        ComPtr<ID2D1Bitmap> bmp;
        HRESULT hr = pRT->CreateBitmap(size, r.pixels.data(), r.stride, &props, &bmp);
        if (SUCCEEDED(hr)) {
            auto it = m_imageThumbCache.find(r.pageIndex);
            if (it != m_imageThumbCache.end() && it->second) {
                it->second->Release();
            }
            m_imageThumbCache[r.pageIndex] = bmp.Detach();
            m_imageThumbBg[r.pageIndex] = { r.hasTransparency, r.adaptiveBgColor };
            m_needsRepaint = true;
        } else {
            m_imageThumbCache[r.pageIndex] = nullptr;
            m_imageThumbBg.erase(r.pageIndex);
        }
    }
    // [防重复加载] 结果已入缓存（或已丢弃），此刻才解除 pending 标记，
    // 关闭"解码完成→结果入缓存"空窗期内的重复入队
    if (!consumedPages.empty()) {
        std::lock_guard<std::mutex> lock(m_thumbQueueMutex);
        for (uint32_t p : consumedPages) m_thumbPending.erase(p);
    }
}

void ImageListThumbnailPanel::ProcessAsyncResults() {
    if (!m_currentRT) return;
    ProcessAsyncResults(m_currentRT);
}

// ============================================================================
// Async thumbnail loading — runs on background thread
// ============================================================================

void ImageListThumbnailPanel::EnqueueThumb(uint32_t idx) {
    if (idx >= m_imagePaths.size()) return;
    std::lock_guard<std::mutex> lock(m_thumbQueueMutex);
    if (m_thumbPending.count(idx)) return;
    m_thumbQueue.push(idx);
    m_thumbPending[idx] = true;
    m_thumbCV.notify_one();
}

void ImageListThumbnailPanel::ThumbWorkerLoop() {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CImageLoader loader;
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

    while (m_thumbRunning) {
        uint32_t idx;
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(m_thumbQueueMutex);
            m_thumbCV.wait(lock, [this] { return !m_thumbQueue.empty() || !m_thumbRunning; });
            if (!m_thumbRunning) break;
            if (m_thumbQueue.empty()) continue;
            idx = m_thumbQueue.front();
            m_thumbQueue.pop();
            // [防竞态] 路径在锁内拷贝：UI 线程可能在 ShowImageThumbnails 中改写 m_imagePaths
            if (idx < m_imagePaths.size()) path = m_imagePaths[idx];
        }

        AsyncThumbResult result;
        result.pageIndex = idx;
        result.sourcePath = path;

        const uint64_t decodeStart = GetTickCount64();
        if (!path.empty()) {
            const bool wantTrans = QuickView::WantsTransparentBackground(path);
            CImageLoader::ThumbData thumbData;
            HRESULT hr = loader.LoadThumbnail(
                path.c_str(), kThumbnailTargetWidth, &thumbData, true, wantTrans);
            const uint64_t decodeMs = GetTickCount64() - decodeStart;
            {
                size_t slash = path.find_last_of(L"\\/");
                const wchar_t* name = (slash != std::wstring::npos) ? path.c_str() + slash + 1 : path.c_str();
                if (SUCCEEDED(hr) && thumbData.isValid) {
                    QvThumbPerfLog(L"[THUMB] #%u %ls -> %ls, %llu ms",
                                   idx, name,
                                   thumbData.loaderName.c_str(),
                                   (unsigned long long)decodeMs);
                } else {
                    QvThumbPerfLog(L"[THUMB] #%u %ls -> FAILED hr=0x%08X, %llu ms",
                                   idx, name, (unsigned)hr,
                                   (unsigned long long)decodeMs);
                }
            }
            if (SUCCEEDED(hr) && thumbData.isValid && !thumbData.pixels.empty()) {
                result.pixels = std::move(thumbData.pixels);
                result.width = thumbData.width;
                result.height = thumbData.height;
                result.stride = thumbData.stride;
                result.valid = true;
                result.hasTransparency = thumbData.hasTransparency;
                result.adaptiveBgColor = thumbData.adaptiveBgColor;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_thumbResultMutex);
            m_thumbResults.push_back(std::move(result));
        }

        if (m_hwnd) {
            PostMessage(m_hwnd, WM_IMAGE_THUMB_READY, 0, 0);
        }

        // [防重复加载] pending 标记保持到主线程把结果收进缓存为止
        //（ProcessAsyncResults 中清除）。若在"解码完成→结果入缓存"的空窗期
        // 清除，加载调度器会认为该项既不在缓存也不在 pending 而重新入队，
        // 导致整批缩略图加载两遍。
    }

    if (SUCCEEDED(coInit)) CoUninitialize();
}
