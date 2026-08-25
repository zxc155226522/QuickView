#include "pch.h"
#include "ThumbnailManager.h"
#include <algorithm>
#include <cwctype>
#include "FileNavigator.h"
extern FileNavigator& g_navigator;

ThumbnailManager::ThumbnailManager() {}

ThumbnailManager::~ThumbnailManager() {
    Shutdown();
}

void ThumbnailManager::Initialize(HWND hwnd, CImageLoader* pLoader) {
    m_hwnd = hwnd;
    m_pLoader = pLoader;
    m_running = true;
    m_workerThreadFast = std::thread(&ThumbnailManager::WorkerLoopFast, this);
    m_workerThreadSlow = std::thread(&ThumbnailManager::WorkerLoopSlow, this);
}

void ThumbnailManager::Shutdown() {
    m_running = false;
    m_cvFast.notify_all();
    m_cvSlow.notify_all();
    if (m_workerThreadFast.joinable()) m_workerThreadFast.join();
    if (m_workerThreadSlow.joinable()) m_workerThreadSlow.join();
    ClearCache();
}

void ThumbnailManager::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_l1Cache.clear();
    m_l2Cache.clear();
    m_lruList.clear();
    m_lruMap.clear();
    m_currentCacheSize = 0;
    
    // Also clear queue?
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    m_currentGeneration++; // Invalidate pending work
    m_pendingTasks.clear();
    m_fastQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();

    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
}

ComPtr<ID2D1Bitmap> ThumbnailManager::GetThumbnail(size_t imageId, LPCWSTR /*filePath*/, ID2D1RenderTarget* pRT) {
    // [v6.0.7] Split locking: L2 hit is a read-only fast path; L1→L2 promotion
    // creates a GPU bitmap OUTSIDE the cache mutex to avoid blocking the worker.

    // 1. Check L2 Cache (GPU Bitmap) — read-only, brief lock
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto itL2 = m_l2Cache.find(imageId);
        if (itL2 != m_l2Cache.end()) {
            TouchLRU(imageId);
            return itL2->second;
        }
    }

    // 2. Check L1 Cache (Raw Data) — copy data out under lock, create bitmap outside lock
    CImageLoader::ThumbData l1Data;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto itL1 = m_l1Cache.find(imageId);
        if (itL1 == m_l1Cache.end()) return nullptr;       // Not in any cache
        TouchLRU(imageId);
        if (itL1->second.isFailed) return nullptr;
        l1Data = itL1->second; // copy raw pixels out (will be consumed by CreateBitmap)
    }

    // GPU bitmap creation outside the cache mutex (no blocking worker threads)
    ComPtr<ID2D1Bitmap> bmp;
    if (pRT && !l1Data.pixels.empty()) {
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        D2D1_SIZE_U size = D2D1::SizeU(l1Data.width, l1Data.height);
        HRESULT hr = pRT->CreateBitmap(size, l1Data.pixels.data(), l1Data.stride, &props, &bmp);
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_l2Cache[imageId] = bmp;
        } else {
            // Mark L1 entry as failed to avoid repeated attempts
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            auto itL1 = m_l1Cache.find(imageId);
            if (itL1 != m_l1Cache.end()) {
                itL1->second.isFailed = true;
                itL1->second.pixels.clear();
                itL1->second.pixels.shrink_to_fit();
            }
        }
    }
    return bmp;
}

void ThumbnailManager::UpdateOptimizedPriority(int startIdx, int endIdx, int priorityCenter) {
    (void)startIdx;
    (void)endIdx;
    (void)priorityCenter;
    // Queue requests are driven per-item by GalleryOverlay::Render.
    // Clearing pending work here causes large images to be re-queued every frame
    // while they are still decoding, which manifests as persistent flicker.
}

// Helper (Internal or Public?) - Let's make it Public for Overlay to use iteratively
// Actually, let's change UpdateOptimizedPriority to accept a list of needed thumbs?
// Or just let Overlay loop and call "QueueIfNotCached".
// Let's add `QueueRequest(int index, LPCWSTR path, int priority)` to public API.
// And `ClearQueue()` for the "Fast Scroll" cancellation.

void ThumbnailManager::EvictLRU() {
    // Must be called with m_cacheMutex locked
    while (!m_lruList.empty() && (m_currentCacheSize > MAX_CACHE_SIZE || m_lruMap.size() > MAX_CACHE_COUNT)) {
        size_t idxToRemove = m_lruList.back();
        
        // [Fix] Correctly track memory reduction before erasing
        auto itL1 = m_l1Cache.find(idxToRemove);
        if (itL1 != m_l1Cache.end()) {
            size_t size = itL1->second.pixels.size();
            if (m_currentCacheSize >= size) m_currentCacheSize -= size;
            else m_currentCacheSize = 0;
            m_l1Cache.erase(itL1);
        }
        
        m_l2Cache.erase(idxToRemove);
        m_lruMap.erase(idxToRemove);
        m_lruList.pop_back();
    }
}

void ThumbnailManager::AddToLRU(size_t imageId, size_t size) {
    auto it = m_lruMap.find(imageId);
    if (it != m_lruMap.end()) {
        // [Fix] If exists, just move to front. 
        // We assume size doesn't change significantly for the same ID once in L1.
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
        return;
    }
    
    m_currentCacheSize += size;
    m_lruList.push_front(imageId);
    m_lruMap[imageId] = m_lruList.begin();
    
    EvictLRU();
}

void ThumbnailManager::TouchLRU(size_t imageId) {
    auto it = m_lruMap.find(imageId);
    if (it != m_lruMap.end()) {
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
    }
}

void ThumbnailManager::WorkerLoopFast() {
    // Thumbnail extraction (especially via WIC/Shell) relies on COM components
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvFast.wait(lock, [this] { return !m_fastQueue.empty() || !m_running; });
            
            if (!m_running) break;
            
            // Double check empty in case of spurious wake or stolen task (unlikely with one consumer per queue, but safe)
            if (m_fastQueue.empty()) continue;

            task = m_fastQueue.top();
            m_fastQueue.pop();
            
        }
        
        // [Fix] Check if task is still valid for current view state
        if (task.generation != m_currentGeneration) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
            continue;
        }

        int targetSize = 300; 
        CImageLoader::ThumbData data;
        HRESULT hr = m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data);
        if (FAILED(hr) || !data.isValid) {
            data.isValid = true; 
            data.isFailed = true;
            data.width = 1; data.height = 1; data.stride = 4;
            data.pixels = { 0x80, 0x80, 0x80, 0xFF }; 
            data.loaderName = L"Failure Placeholder";
        }
        if (data.isValid) {
            // Re-check generation after potentially long extraction
            if (task.generation == m_currentGeneration) {
                {
                    std::lock_guard<std::mutex> lock(m_cacheMutex);
                    size_t size = data.pixels.size();
                    m_l1Cache[task.imageId] = std::move(data);
                    AddToLRU(task.imageId, size);
                }
                PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.imageId, 0);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
        }
    }
    
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

void ThumbnailManager::WorkerLoopSlow() {
    // Thumbnail extraction (especially via WIC/Shell) relies on COM components
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvSlow.wait(lock, [this] { return !m_slowQueue.empty() || !m_running; });
            
            if (!m_running) break;
            
            if (m_slowQueue.empty()) continue;

            task = m_slowQueue.top();
            m_slowQueue.pop();
            
        }
        
        // [Fix] Check if task is still valid for current view state
        if (task.generation != m_currentGeneration) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
            continue;
        }

        int targetSize = 300; 
        CImageLoader::ThumbData data;
        HRESULT hr = m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data, true);
        if (FAILED(hr) || !data.isValid) {
            data.isValid = true; 
            data.isFailed = true;
            data.width = 1; data.height = 1; data.stride = 4;
            data.pixels = { 0x80, 0x80, 0x80, 0xFF }; 
            data.loaderName = L"Failure Placeholder (Archive)";
        }
        if (data.isValid) {
            // Re-check generation after potentially long extraction
            if (task.generation == m_currentGeneration) {
                {
                    std::lock_guard<std::mutex> lock(m_cacheMutex);
                    size_t size = data.pixels.size();
                    m_l1Cache[task.imageId] = std::move(data);
                    AddToLRU(task.imageId, size);
                }
                PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.imageId, 0);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
        }
    }
    
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

// Added to match planned API changes
#include "FileNavigator.h"

void ThumbnailManager::QueueRequest(size_t imageId, LPCWSTR path, int priority) {
    // [v6.0.7] Fast non-locking check: if already in L2 cache, skip entirely.
    // This avoids acquiring m_cacheMutex + m_queueMutex on every cached item.
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        if (m_l1Cache.count(imageId) || m_l2Cache.count(imageId)) {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_pendingTasks.count(imageId)) return; // Already queued
    
    Task t;
    t.imageId = imageId;
    t.path = path;
    t.priorityDistance = priority;
    t.generation = m_currentGeneration;

    // Detect if this is a virtual archive path
    std::wstring archivePath;
    size_t archiveIndex = 0;
    if (FileNavigator::ParseVirtualPath(path, archivePath, archiveIndex)) {
        t.isArchive = true;
        t.archiveIndex = (int)archiveIndex;
        t.archivePathHash = ComputePathHash(archivePath);
    }

    // Detect format for Lane Selection
    // Fast Lane: JPEG (Optimized) + RAW/HEIC/PSD (Embedded Preview) + WebP (Scaled)
    const std::wstring_view e = QuickView::ExtensionOf(path);
    const bool isFast =
        QuickView::ExtEqualsIgnoreCase(e, L".jpg") || QuickView::ExtEqualsIgnoreCase(e, L".jpeg") ||
        QuickView::ExtEqualsIgnoreCase(e, L".jpe") || QuickView::ExtEqualsIgnoreCase(e, L".jfif") ||
        QuickView::IsRawExtension(e) || QuickView::IsHeifExtension(e) ||
        QuickView::ExtEqualsIgnoreCase(e, L".avif") ||
        QuickView::ExtEqualsIgnoreCase(e, L".psd") || QuickView::ExtEqualsIgnoreCase(e, L".psb") ||
        QuickView::ExtEqualsIgnoreCase(e, L".webp");
    t.isFastLane = isFast;

    if (isFast) {
        m_fastQueue.push(t);
        m_cvFast.notify_one();
    } else {
        m_slowQueue.push(t);
        m_cvSlow.notify_one();
    }
    
    m_pendingTasks[imageId] = true;
}

void ThumbnailManager::ClearQueue() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_currentGeneration++; // [Fix] Increment generation to invalidate pending background extractions
    m_fastQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_pendingTasks.clear();

    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_pendingTasks.clear();
}




ThumbnailManager::ImageInfo ThumbnailManager::GetImageInfo(size_t imageId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_l1Cache.find(imageId);
    if (it != m_l1Cache.end()) {
        ImageInfo info;
        info.origWidth = it->second.origWidth;
        info.origHeight = it->second.origHeight;
        info.fileSize = it->second.fileSize;
        info.isValid = true;
        info.isFailed = it->second.isFailed;
        return info;
    }
    return { 0, 0, 0, false, false };
}

