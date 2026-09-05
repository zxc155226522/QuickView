#pragma once
// ============================================================================
// ImageListThumbnailPanel.h
// Folder image thumbnails sidebar.
// Uses a background worker thread to load shell thumbnails.
// Visually distinct from PdfPageThumbnailPanel: green accent, "图片" title.
// ============================================================================

#include "ThumbnailPanelBase.h"
#include "ImageTypes.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

// [Image Mode] Async thumbnail ready notification (posted from worker thread)
#define WM_IMAGE_THUMB_READY (WM_USER + 111)

class FileNavigator;

class ImageListThumbnailPanel : public ThumbnailPanelBase {
public:
    ImageListThumbnailPanel() = default;
    ~ImageListThumbnailPanel() override;

    // Lifecycle
    void InitializeEx(HWND hwnd);

    // Image mode lifecycle
    void ShowImageThumbnails(FileNavigator* nav, int currentIndex, uint32_t totalFiles);
    void OnDocumentClosed();
    void SetCurrentImageIndex(int index);
    [[nodiscard]] bool IsImageMode() const { return m_isImageMode; }
    // [渐进扫描] 当前面板定位到的文件索引（用于判断列表刷新是否必要）
    [[nodiscard]] int GetCurrentImageIndex() const { return m_currentImageIndex; }

    // Process async thumbnail results (call from main thread via WM_IMAGE_THUMB_READY)
    void ProcessAsyncResults(ID2D1RenderTarget* pRT);
    void ProcessAsyncResults(); // Uses m_currentRT from base class

    // [ThumbnailPanelBase] Full path of the item's file (context menu / tooltip) —
    // kept public so the main window can resolve the right-click target file
    std::wstring GetItemFullPath(uint32_t index) const override;

    // Get adaptive background color calculated for thumbnail (0 if not available)
    uint32_t GetAdaptiveBgColorForIndex(int index) const {
        if (index < 0) return 0;
        auto it = m_imageThumbBg.find(static_cast<uint32_t>(index));
        if (it != m_imageThumbBg.end() && it->second.adaptiveBgColor != 0) {
            return it->second.adaptiveBgColor;
        }
        return 0;
    }

protected:
    // ThumbnailPanelBase interface
    void OnUpdateThumbnailRequests() override;
    void OnProcessThumbnailResults() override {} // handled via WM_IMAGE_THUMB_READY
    bool OnIsLoading() const override;
    void DrawItems(ID2D1RenderTarget* pRT) override;
    int OnItemClick(int index) override;
    void OnDeviceResourcesCreated() override {}
    void OnDeviceResourcesDiscarded() override;
    void OnLayoutChanged() override;

    uint32_t GetItemCount() const override { return m_totalImages; }
    ComPtr<ID2D1Bitmap> GetItemBitmap(uint32_t index) override;
    std::wstring GetItemLabel(uint32_t index) const override;
    const wchar_t* GetPanelTitle() const override { return L"图片"; }
    D2D1::ColorF GetAccentColor() const override { return D2D1::ColorF(0.23f, 0.80f, 0.40f); } // Green

private:
    // Async thumbnail loader
    struct AsyncThumbResult {
        uint32_t pageIndex = 0;
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        int stride = 0;
        bool valid = false;
        bool hasTransparency = false;
        uint32_t adaptiveBgColor = 0;
    };

    struct ThumbBgInfo {
        bool hasTransparency = false;
        uint32_t adaptiveBgColor = 0;
    };

    std::vector<std::thread> m_thumbThreads;
    std::mutex m_thumbQueueMutex;
    std::condition_variable m_thumbCV;
    std::queue<uint32_t> m_thumbQueue;
    std::unordered_map<uint32_t, bool> m_thumbPending;
    std::mutex m_thumbResultMutex;
    std::vector<AsyncThumbResult> m_thumbResults;
    std::atomic<bool> m_thumbRunning{ false };

    void EnqueueThumb(uint32_t idx);
    void ThumbWorkerLoop();

    static constexpr int kThumbWorkerThreads = 4;
    static constexpr size_t kMaxCacheSize = 80;

    // Image mode state
    bool m_isImageMode = false;
    FileNavigator* m_navigator = nullptr;
    int m_currentImageIndex = -1;
    uint32_t m_totalImages = 0;
    std::vector<std::wstring> m_imagePaths;
    std::map<uint32_t, ID2D1Bitmap*> m_imageThumbCache;
    std::map<uint32_t, ThumbBgInfo> m_imageThumbBg;
};
