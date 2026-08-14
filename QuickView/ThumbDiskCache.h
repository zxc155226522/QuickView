// ============================================================================
// ThumbDiskCache.h
// Persistent on-disk thumbnail cache for the shell thumbnail provider.
//
// Why: the persistent pipe server already caches results in memory, but that
// cache dies with the server process and with Explorer's own (cold) cache.
// Storing the small thumbnail bitmap on disk keyed by (path + mtime + size +
// cx) means a slow document (e.g. a 5s .ai) is decoded ONCE and then returned
// instantly on every later view — even after a server restart or a cleared
// Explorer cache. Only tiny thumbnails are stored, so a 1 GB cap is ample.
//
// Freshness is implicit in the key: editing a file changes its mtime, which
// changes the cache key, so a stale thumbnail can never be shown.
// ============================================================================
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace QuickView {

class ThumbDiskCache {
public:
    static ThumbDiskCache& Instance();

    // Returns true and fills outBytes (the raw server BMP) if a valid, fresh
    // thumbnail is cached for (path, cx). false on miss or corruption.
    bool Get(const std::wstring& path, UINT cx, std::vector<BYTE>& outBytes);

    // Best-effort store of raw server BMP bytes for (path, cx). A failed stat
    // (e.g. file gone) is a no-op.
    void Put(const std::wstring& path, UINT cx, const BYTE* data, size_t len);

private:
    ThumbDiskCache();
    std::wstring CacheDir();
    bool FileStamp(const std::wstring& path, uint64_t& mtime, uint64_t& size);
    std::wstring HashKey(const std::wstring& path, uint64_t mtime, uint64_t size, UINT cx);
    std::wstring PathForHash(const std::wstring& hash);
    void Touch(const std::wstring& file);
    void EvictIfNeeded();

    std::mutex m_mtx;
    bool m_enabled = false;
    uint64_t m_capBytes = 1024ULL * 1024 * 1024; // 1 GB
    uint64_t m_estBytes = 0;
    std::wstring m_dir;
};

} // namespace QuickView
