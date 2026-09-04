// ============================================================================
// ThumbDiskCache.cpp — see ThumbDiskCache.h for the design rationale.
// Best-effort throughout: any failure disables or skips the operation rather
// than risk blocking the shell thumbnail thread.
// ============================================================================
#include "ThumbDiskCache.h"
#include <algorithm>
#include <cstdio>
#include <shlobj.h>
#include <strsafe.h>

namespace QuickView {

ThumbDiskCache& ThumbDiskCache::Instance() {
    static ThumbDiskCache s_inst;
    return s_inst;
}

ThumbDiskCache::ThumbDiskCache() {
    m_dir = CacheDir();
    m_enabled = !m_dir.empty();
    // Seed the size estimate by scanning existing files once.
    if (m_enabled) {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW((m_dir + L"\\*.bmp").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    m_estBytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
}

std::wstring ThumbDiskCache::CacheDir() {
    wchar_t ad[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, ad)))
        return L"";
    // CreateDirectoryW only creates the final level; create the parent
    // "QuickView" directory explicitly first, otherwise the two-level path
    // fails with ERROR_PATH_NOT_FOUND and the cache silently stays disabled.
    std::wstring base = std::wstring(ad) + L"\\QuickView";
    std::wstring dir  = base + L"\\ThumbCache\\v4";
    // [Cache Versioning] v4 = thumbnails with margin padding on square card adaptive background
    // (smart light/dark backing & 1:1 square canvas with呼吸感边距). Purge v3, v2 and legacy directories.
    auto purgeDir = [](const std::wstring& p) {
        WIN32_FIND_DATAW lfd = {};
        HANDLE lh = FindFirstFileW((p + L"\\*.bmp").c_str(), &lfd);
        if (lh != INVALID_HANDLE_VALUE) {
            do {
                if (!(lfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    DeleteFileW((p + L"\\" + lfd.cFileName).c_str());
            } while (FindNextFileW(lh, &lfd));
            FindClose(lh);
            RemoveDirectoryW(p.c_str());
        }
    };
    purgeDir(base + L"\\ThumbCache\\v3");
    purgeDir(base + L"\\ThumbCache\\v2");
    purgeDir(base + L"\\ThumbCache");
    if (!CreateDirectoryW(base.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return L""; // unwritable -> disabled
    }
    // Create intermediate "ThumbCache" level, then versioned "v4".
    std::wstring verMid = base + L"\\ThumbCache";
    if (!CreateDirectoryW(verMid.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return L"";
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return L""; // unwritable -> disabled
    }
    return dir;
}

bool ThumbDiskCache::FileStamp(const std::wstring& path, uint64_t& mtime, uint64_t& size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft = {};
    LARGE_INTEGER sz = {};
    BOOL ok = GetFileTime(h, nullptr, nullptr, &ft) && GetFileSizeEx(h, &sz);
    CloseHandle(h);
    if (!ok) return false;
    mtime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    size = static_cast<uint64_t>(sz.QuadPart);
    return true;
}

// FNV-1a 64-bit -> 16 hex chars. Collisions are astronomically unlikely and
// would at worst produce a mismatched thumbnail that the caller's decode
// rejects (we only store valid BMPs and re-check on read).
static std::wstring Fnv1aHex(const std::wstring& s) {
    uint64_t h = 14695981039346656037ULL;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    wchar_t buf[17];
    StringCchPrintfW(buf, 17, L"%016llX", h);
    return buf;
}

std::wstring ThumbDiskCache::HashKey(const std::wstring& path, uint64_t mtime,
                                     uint64_t size, UINT cx) {
    std::wstring k = path + L"|" + std::to_wstring(mtime) + L"|" +
                     std::to_wstring(size) + L"|" + std::to_wstring(cx);
    // Normalize drive letter case so C:\ and c:\ map to the same key.
    if (k.size() >= 2 && k[1] == L':') k[0] = (wchar_t)::towupper((wint_t)k[0]);
    return Fnv1aHex(k);
}

std::wstring ThumbDiskCache::PathForHash(const std::wstring& hash) {
    return m_dir + L"\\" + hash + L".bmp";
}

void ThumbDiskCache::Touch(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, nullptr, &now, &now);
    CloseHandle(h);
}

void ThumbDiskCache::EvictIfNeeded() {
    if (m_estBytes <= m_capBytes) return;
    // Enumerate, sort by LastWriteTime (oldest first), delete until under cap.
    struct Entry { std::wstring path; uint64_t wt; uint64_t size; };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((m_dir + L"\\*.bmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint64_t wt = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
        uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        entries.push_back({m_dir + L"\\" + fd.cFileName, wt, sz});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.wt < b.wt; });
    size_t deleted = 0;
    for (const auto& e : entries) {
        if (m_estBytes <= m_capBytes) break;
        if (DeleteFileW(e.path.c_str())) {
            m_estBytes -= e.size;
            if (++deleted >= 400) break; // bound work per call
        }
    }
}

bool ThumbDiskCache::Get(const std::wstring& path, UINT cx, std::vector<BYTE>& outBytes) {
    if (!m_enabled) return false;
    uint64_t mtime = 0, size = 0;
    if (!FileStamp(path, mtime, size)) return false;
    std::wstring file = PathForHash(HashKey(path, mtime, size, cx));
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false; // miss
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 64 * 1024 * 1024;
    if (ok) {
        outBytes.resize(static_cast<size_t>(sz.QuadPart));
        DWORD rd = 0;
        ok = ReadFile(h, outBytes.data(), static_cast<DWORD>(outBytes.size()), &rd, nullptr) &&
             rd == outBytes.size();
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(file.c_str()); // corrupt -> drop
        outBytes.clear();
        return false;
    }
    Touch(file); // mark recently used so LRU keeps it
    return true;
}

void ThumbDiskCache::Put(const std::wstring& path, UINT cx, const BYTE* data, size_t len) {
    if (!m_enabled || !data || len == 0 || len > 64 * 1024 * 1024) return;
    uint64_t mtime = 0, size = 0;
    if (!FileStamp(path, mtime, size)) return;
    std::wstring file = PathForHash(HashKey(path, mtime, size, cx));
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    bool ok = WriteFile(h, data, static_cast<DWORD>(len), &wr, nullptr) && wr == len;
    CloseHandle(h);
    if (!ok) { DeleteFileW(file.c_str()); return; }
    std::lock_guard<std::mutex> lock(m_mtx);
    m_estBytes += len;
    EvictIfNeeded();
}

} // namespace QuickView
