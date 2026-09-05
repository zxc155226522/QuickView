#include "pch.h"
#include "FileNavigator.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <wrl/client.h>

extern AppConfig g_config;

// [Directory Watcher] Custom window message posted when background scan completes
// defined in header: constexpr UINT WM_NAVIGATOR_DIR_CHANGED = WM_APP + 50;

void FileNavigator::Initialize(const std::wstring& currentPath, HWND hwnd) {
    // Stop existing watcher and pair verification before mutating state
    StopPairVerification();
    StopDirectoryWatcher();
    if (hwnd) m_hwnd = hwnd;

    namespace fs = std::filesystem;

    fs::path p = fs::path(currentPath);
    if (!fs::exists(p)) return;

    m_files.clear();
    m_currentIndex = -1;

    const bool isDirectory = fs::is_directory(p);

    // If a directory is passed in, scan it directly. Otherwise scan the parent directory.
    fs::path dir = isDirectory ? p : p.parent_path();
    if (dir.empty()) return;

    // [RAW+JPEG Pairing] Verification results belong to one folder
    {
        std::wstring dirStr = dir.wstring();
        if (_wcsicmp(dirStr.c_str(), m_verifyDir.c_str()) != 0) {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            m_verifyDone.clear();
            m_verifyUnpaired.clear();
            m_verifyDir = std::move(dirStr);
        }
    }

    // Supported extensions (comprehensive list including RAW formats)
    // using QuickView::SUPPORTED_EXTENSIONS from SupportedExtensions.h

    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
        // Already handled isDirectory
    }

    m_sizes.clear();
    m_ids.clear();
    m_pairedRaws.clear();

    // Archive containers are no longer supported: reject them up front so a
    // directly-opened .zip/.rar leaves an empty playlist instead of trying to
    // browse inside it.
    std::wstring pExt = p.extension().wstring();
    std::transform(pExt.begin(), pExt.end(), pExt.begin(), [](wchar_t c){ return std::towlower(c); });
    if (!isDirectory && QuickView::IsArchiveExtension(pExt)) {
        return;
    }

    // [异步目录扫描] 只建立"快速状态"让界面和主图立即就绪，不再在 UI 线程
    // 同步扫描整个目录（大目录 + 网络盘会卡住窗口数秒）：
    // - 打开文件：列表先只含当前文件，索引 0（导航/胶片条/页面指示立即可用）
    // - 打开目录：只同步探测第一个可显示文件（directory_iterator 取到即停，
    //   网络盘上开销很小），主图立即开始解码；全量列表由后台扫描补入
    // 全量扫描放后台线程，完成后经 WM_NAVIGATOR_DIR_CHANGED →
    // ApplyPendingScanResult 原子换入。
    if (!isDirectory) {
        std::error_code ecSz;
        m_files.assign(1, p.wstring());
        m_sizes.push_back(fs::file_size(p, ecSz));
        m_ids.push_back(ComputePathHash(p.wstring()));
        m_currentIndex = 0;
    } else {
        std::error_code itEc;
        for (const auto& entry : fs::directory_iterator(dir, itEc)) {
            std::error_code feEc;
            if (!entry.is_regular_file(feEc)) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
            if (QuickView::IsArchiveExtension(ext)) continue;
            bool supported = false;
            for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                if (ext == supp) { supported = true; break; }
            }
            if (supported) {
                std::error_code ecSz;
                m_files.assign(1, entry.path().wstring());
                m_sizes.push_back(fs::file_size(entry.path(), ecSz));
                m_ids.push_back(ComputePathHash(entry.path().wstring()));
                m_currentIndex = 0;
                break;
            }
        }
        if (m_files.empty()) m_currentIndex = -1;
    }

    // [Directory Watcher] Start monitoring first: it sets m_watchedDir which
    // PerformDirectoryScan (running on the scan thread) reads.
    if (m_hwnd) {
        std::wstring watchDir = dir.wstring();
        if (!watchDir.empty()) {
            StartDirectoryWatcher(watchDir);
        }
    }

    if (m_watchedDir.empty()) {
        return; // 没有可扫描的目录（异常路径）
    }

    // 唤醒常驻扫描线程执行全量扫描
    {
        std::lock_guard<std::mutex> lock(m_scanWorkMutex);
        m_scanDir = dir.wstring();
        m_scanWorkPending = true;
        ++m_scanGeneration;
    }
    if (!m_scanWorker.joinable()) {
        m_scanWorker = std::thread(&FileNavigator::ScanWorkerLoop, this);
    }
    m_scanWorkCv.notify_one();

    // [RAW+JPEG Pairing] Kick off capture-time verification for fresh pairs
    StartPairVerification();
}

std::wstring FileNavigator::Next(bool /*unused*/) {
    if (m_files.empty()) return L"";

    if (g_runtime.NavTraverse) {
        bool shouldTraverse = false;
        
        // Case 1: We are at the end of the current playlist
        if (m_currentIndex >= (int)m_files.size() - 1) {
            shouldTraverse = true;
        } else {
        // Case 2: The next sibling in the parent directory is a container (folder)
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        std::wstring currentFile = m_files[m_currentIndex];

        namespace fs = std::filesystem;
        fs::path currentPath(currentFile);
        fs::path parentDir = currentPath.parent_path();

        std::vector<std::wstring> siblings = GetSortedSiblings(parentDir);

        auto it = std::find(siblings.begin(), siblings.end(), currentFile);
        int idx = (it == siblings.end()) ? -1 : (int)std::distance(siblings.begin(), it);

        if (idx != -1 && idx < (int)siblings.size() - 1) {
            std::wstring nextSibling = siblings[idx + 1];
            bool isContainer = fs::is_directory(nextSibling);

            if (isContainer) {
                shouldTraverse = true;
            }
        }
        }
        }
        
        if (shouldTraverse) {
            std::wstring nextFolderImg = FindAdjacentFolderImage(true);
            if (!nextFolderImg.empty()) {
                m_crossFolderMessage = L">>> Entering [" + std::filesystem::path(nextFolderImg).parent_path().filename().wstring() + L"] >>>";
                return nextFolderImg;
            }
        }
    }

    if (m_currentIndex >= (int)m_files.size() - 1) {
        if (g_runtime.NavLoop) {
            m_hitEnd = true; // Signal OSD
            m_currentIndex = 0;
            return m_files[m_currentIndex];
        } else {
            m_hitEnd = true;
            return L"";
        }
    }

    m_hitEnd = false;
    m_currentIndex++;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::Previous(bool /*unused*/) {
    if (m_files.empty()) return L"";

    if (g_runtime.NavTraverse) {
        bool shouldTraverse = false;
        
        // Case 1: We are at the beginning of the current playlist
        if (m_currentIndex <= 0) {
            shouldTraverse = true;
        } else {
        // Case 2: The previous sibling in the parent directory is a container (folder)
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        std::wstring currentFile = m_files[m_currentIndex];

        namespace fs = std::filesystem;
        fs::path currentPath(currentFile);
        fs::path parentDir = currentPath.parent_path();

        std::vector<std::wstring> siblings = GetSortedSiblings(parentDir);

        auto it = std::find(siblings.begin(), siblings.end(), currentFile);
        int idx = (it == siblings.end()) ? -1 : (int)std::distance(siblings.begin(), it);

        if (idx > 0) {
            std::wstring prevSibling = siblings[idx - 1];
            bool isContainer = fs::is_directory(prevSibling);

            if (isContainer) {
                shouldTraverse = true;
            }
        }
        }
        }
        
        if (shouldTraverse) {
            std::wstring prevFolderImg = FindAdjacentFolderImage(false);
            if (!prevFolderImg.empty()) {
                m_crossFolderMessage = L"<<< Entering [" + std::filesystem::path(prevFolderImg).parent_path().filename().wstring() + L"] <<<";
                return prevFolderImg;
            }
        }
    }

    if (m_currentIndex <= 0) {
        if (g_runtime.NavLoop) {
            m_hitEnd = true; // Signal OSD
            m_currentIndex = (int)m_files.size() - 1;
            return m_files[m_currentIndex];
        } else {
            m_hitEnd = true;
            return L"";
        }
    }

    m_hitEnd = false;
    m_currentIndex--;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::First() {
    if (m_files.empty()) return L"";
    m_hitEnd = false;
    m_currentIndex = 0;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::Last() {
    if (m_files.empty()) return L"";
    m_hitEnd = false;
    m_currentIndex = (int)m_files.size() - 1;
    return m_files[m_currentIndex];
}

std::wstring FileNavigator::GetCrossFolderMessage() {
    std::wstring msg = m_crossFolderMessage;
    m_crossFolderMessage.clear(); // Consume
    return msg;
}

std::wstring FileNavigator::PeekNext() const {
    if (m_files.empty()) return L"";
    size_t nextIdx = (m_currentIndex + 1) % m_files.size();
    return m_files[nextIdx];
}

std::wstring FileNavigator::PeekPrevious() const {
    if (m_files.empty()) return L"";
    size_t prevIdx = (m_currentIndex - 1 + m_files.size()) % m_files.size();
    return m_files[prevIdx];
}

void FileNavigator::Refresh() {
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        std::error_code ec;
        m_sizes[m_currentIndex] = std::filesystem::file_size(m_files[m_currentIndex], ec);
    }
}

void FileNavigator::SetIndex(int index) {
    if (index >= 0 && index < (int)m_files.size()) {
        m_currentIndex = index;
        m_hitEnd = false;
    }
}

const std::wstring& FileNavigator::GetFile(int index) const {
    static std::wstring empty;
    if (index < 0 || index >= (int)m_files.size()) return empty;
    return m_files[index];
}

std::wstring FileNavigator::GetResolvedPath(const std::wstring& requestedPath) const {
    // [RAW+JPEG Pairing] A RAW folded behind its rendered sibling resolves to
    // that sibling: with pairing enabled the pair is one logical photo and the
    // rendered file is its visible face, regardless of which file was opened.
    if (!m_pairedRaws.empty()) {
        for (const auto& [renderedId, raw] : m_pairedRaws) {
            if (raw.path == requestedPath) {
                for (size_t i = 0; i < m_ids.size(); ++i) {
                    if (m_ids[i] == renderedId) return m_files[i];
                }
                break;
            }
        }
    }

    return requestedPath;
}

int FileNavigator::FindIndex(const std::wstring& path) const {
    auto it = std::find(m_files.begin(), m_files.end(), path);
    if (it != m_files.end()) return (int)std::distance(m_files.begin(), it);
    return -1;
}

void FileNavigator::ApplyPendingScanResult() {
    DirectoryScanResult result;
    {
        std::lock_guard<std::mutex> lock(m_scanResultMutex);
        if (!m_pendingScanResult) return;
        result = std::move(*m_pendingScanResult);
        m_pendingScanResult.reset();
    }

    // [防串扰] 目录已切换：丢弃过期扫描结果
    if (result.dir != m_watchedDir) return;

    // Cache current path BEFORE swap for index reconciliation
    std::wstring currentPath;
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
        currentPath = m_files[m_currentIndex];
    }

    // O(1) swap
    m_files = std::move(result.files);
    m_sizes = std::move(result.sizes);
    m_ids = std::move(result.ids);
    m_pairedRaws = std::move(result.pairedRaws);

    // Relocate current index in new list
    if (!currentPath.empty()) {
        auto it = std::find(m_files.begin(), m_files.end(), currentPath);
        if (it != m_files.end()) {
            m_currentIndex = (int)std::distance(m_files.begin(), it);
        } else if (result.partial) {
            // [渐进扫描] 快照还没枚举到当前文件：临时追加到列表尾保持选择
            // 有效（邻居显示为快照尾部文件），最终完整结果到达后自动归位
            m_files.push_back(currentPath);
            m_sizes.push_back(0);
            m_ids.push_back(ComputePathHash(currentPath));
            m_currentIndex = (int)m_files.size() - 1;
        } else {
            // [RAW+JPEG Pairing] The viewed RAW may have just been folded
            // behind its rendered sibling (e.g. its JPG appeared on disk) --
            // relocate to the pair instead of clamping.
            bool redirected = false;
            for (const auto& [renderedId, raw] : m_pairedRaws) {
                if (raw.path == currentPath) {
                    for (size_t i = 0; i < m_ids.size(); ++i) {
                        if (m_ids[i] == renderedId) {
                            m_currentIndex = (int)i;
                            redirected = true;
                            break;
                        }
                    }
                    break;
                }
            }

            // Fallback: file was deleted externally — clamp to nearest valid index
            if (!redirected) {
                if (m_currentIndex >= (int)m_files.size()) {
                    m_currentIndex = (int)m_files.size() - 1;
                }
                if (m_files.empty()) m_currentIndex = -1;
            }
        }
    }

    // [RAW+JPEG Pairing] A rescan may have folded new pairs -- verify them.
    // Already-verified pairs are skipped, so this cannot loop.
    if (!result.partial) StartPairVerification();
}

void FileNavigator::RescanDirectory() {
    if (m_watchedDir.empty()) return; // archive or no folder open
    // Join any in-flight verification pass first: it could otherwise post a
    // result computed before this one right after it.
    StopPairVerification();
    DirectoryScanResult result = PerformDirectoryScan(m_watchedDir);
    {
        std::lock_guard<std::mutex> lock(m_scanResultMutex);
        m_pendingScanResult = std::move(result);
    }
    ApplyPendingScanResult();
}

int64_t FileNavigator::ParseExifDateTime(const std::string& exifDateTime) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf_s(exifDateTime.c_str(), "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (y < 1970 || y > 3000 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    t.tm_isdst = -1;
    const time_t tt = std::mktime(&t); // local time, matching LibRaw's own conversion
    return tt <= 0 ? 0 : (int64_t)tt;
}

// [RAW+JPEG Pairing] Capture time (DateTimeOriginal) of a JPEG file via
// easyexif (a JPEG-only parser: exif.cpp rejects anything not starting with
// FFD8); 0 when unreadable.
static int64_t ReadJpegCaptureTime(const std::wstring& path) {
    FILE* fp = nullptr;
    _wfopen_s(&fp, path.c_str(), L"rb");
    if (!fp) return 0;
    unsigned char buf[65536];
    size_t bytes = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (bytes == 0) return 0;
    easyexif::EXIFInfo info;
    if (info.parseFrom(buf, (unsigned)bytes) != PARSE_EXIF_SUCCESS) return 0;
    return FileNavigator::ParseExifDateTime(info.DateTimeOriginal);
}

void FileNavigator::StartPairVerification() {
    if (!m_hwnd || m_pairedRaws.empty() || m_watchedDir.empty()) return;

    // Snapshot the pairs that still need a capture-time check
    struct VerifyItem {
        std::wstring renderedPath;
        std::wstring rawPath;
        ImageID renderedId = 0;
    };
    std::vector<VerifyItem> todo;
    {
        std::lock_guard<std::mutex> lock(m_verifyMutex);
        for (const auto& [renderedId, raw] : m_pairedRaws) {
            if (m_verifyDone.find(renderedId) != m_verifyDone.end()) continue;
            for (size_t i = 0; i < m_ids.size(); ++i) {
                if (m_ids[i] == renderedId) {
                    todo.push_back({ m_files[i], raw.path, renderedId });
                    break;
                }
            }
        }
    }
    if (todo.empty()) return;

    StopPairVerification();
    const uint32_t gen = ++m_verifyGeneration;
    m_verifyThread = std::thread([this, gen, todo = std::move(todo)]() {
        bool anyUnpaired = false;
        for (const auto& item : todo) {
            if (m_verifyGeneration.load() != gen) return; // superseded

            // Rendered side: dispatch by extension -- JPEG through easyexif
            // (fastest), everything else (HEIF) straight to the fallback
            // reader (WIC). A JPEG whose date lives only in XMP gets one WIC
            // retry too.
            const std::wstring_view rext = QuickView::ExtensionOf(item.renderedPath);
            const bool isJpeg = QuickView::ExtEqualsIgnoreCase(rext, L".jpg")
                             || QuickView::ExtEqualsIgnoreCase(rext, L".jpeg");
            int64_t tRendered = isJpeg ? ReadJpegCaptureTime(item.renderedPath) : 0;
            if (tRendered == 0 && s_captureTimeFallback) {
                tRendered = s_captureTimeFallback(item.renderedPath.c_str());
            }

            // RAW side: always the fallback reader (LibRaw branch)
            const int64_t tRaw = s_captureTimeFallback ? s_captureTimeFallback(item.rawPath.c_str()) : 0;

            std::lock_guard<std::mutex> lock(m_verifyMutex);
            m_verifyDone.insert(item.renderedId);
            if (PairVerificationFails(tRendered, tRaw)) {
                m_verifyUnpaired.insert(item.renderedId);
                anyUnpaired = true;
            }
        }
        if (!anyUnpaired || m_verifyGeneration.load() != gen) return;

        // Split the mismatched pairs back up: rescan with the blacklist in
        // effect and hand the result to the main thread through the exact
        // channel the directory watcher already uses (one atomic list swap).
        DirectoryScanResult result = PerformDirectoryScan(m_watchedDir);
        if (m_verifyGeneration.load() != gen) return;
        {
            std::lock_guard<std::mutex> lock(m_scanResultMutex);
            m_pendingScanResult = std::move(result);
        }
        PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 0, 0);
    });
}

void FileNavigator::StopPairVerification(bool forceSync) {
    ++m_verifyGeneration; // cancel the running pass, if any
    if (m_verifyThread.joinable()) {
        if (forceSync) {
            m_verifyThread.join();
        } else {
            m_verifyThread.detach();
        }
    }
}

void FileNavigator::ApplyRawJpegPairing(std::vector<SortEntry>& entries,
                                        std::unordered_map<ImageID, PairedRaw>& outPairedRaws,
                                        const std::unordered_set<ImageID>* skipRendered) {
    outPairedRaws.clear();

    // Early exit: pairing is only possible when the folder mixes camera RAWs
    // with whitelisted rendered stills. One cheap in-memory pass, no I/O.
    bool anyRaw = false, anyRendered = false;
    for (const auto& e : entries) {
        anyRaw = anyRaw || QuickView::IsRawExtension(e.t);
        anyRendered = anyRendered || QuickView::IsRenderedPairExtension(e.t);
        if (anyRaw && anyRendered) break;
    }
    if (!anyRaw || !anyRendered) return;

    // Group pairing candidates by lowercase stem (file name minus extension;
    // the scan covers a single directory, so the stem identifies the shot).
    // Non-candidates (e.g. a same-name .png screenshot) neither pair nor
    // block a pair.
    struct Group {
        int rawIdx = -1;
        int renderedIdx = -1;
        int rawCount = 0;
        int renderedCount = 0;
    };
    std::unordered_map<std::wstring, Group> groups;
    groups.reserve(entries.size());
    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& e = entries[i];
        const bool isRaw = QuickView::IsRawExtension(e.t);
        const bool isRendered = !isRaw && QuickView::IsRenderedPairExtension(e.t);
        if (!isRaw && !isRendered) continue;

        const size_t sep = e.p.find_last_of(L"\\/");
        const size_t start = (sep == std::wstring::npos) ? 0 : sep + 1;
        std::wstring stem = e.p.substr(start, e.p.size() - start - e.t.size());
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c){ return std::towlower(c); });

        Group& g = groups[stem];
        if (isRaw) { g.rawCount++; g.rawIdx = i; }
        else       { g.renderedCount++; g.renderedIdx = i; }
    }

    // Strict 1:1: fold only when a stem has exactly one RAW and exactly one
    // rendered still. Ambiguous groups (rename collisions, bracketing
    // leftovers) stay fully visible so the user can see and resolve them.
    std::vector<char> hide(entries.size(), 0);
    for (const auto& [stem, g] : groups) {
        if (g.rawCount != 1 || g.renderedCount != 1) continue;
        const SortEntry& raw = entries[g.rawIdx];
        const SortEntry& rendered = entries[g.renderedIdx];
        const ImageID renderedId = ComputePathHash(rendered.p);
        // Capture-time verification confirmed these are different shots
        if (skipRendered && skipRendered->find(renderedId) != skipRendered->end()) continue;
        outPairedRaws.emplace(renderedId,
                              PairedRaw{ raw.p, raw.s, ComputePathHash(raw.p) });
        hide[g.rawIdx] = 1;
    }
    if (outPairedRaws.empty()) return;

    // Drop the hidden RAW entries, preserving sort order.
    std::vector<SortEntry> kept;
    kept.reserve(entries.size() - outPairedRaws.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!hide[i]) kept.push_back(std::move(entries[i]));
    }
    entries = std::move(kept);
}

std::unordered_map<ImageID, size_t> FileNavigator::GetExplorerWindowFileOrder(const std::wstring& targetDir) {
    std::unordered_map<ImageID, size_t> orderMap;
    if (targetDir.empty()) return orderMap;

    namespace fs = std::filesystem;
    std::wstring canonicalTarget = fs::path(targetDir).lexically_normal().wstring();
    while (!canonicalTarget.empty() && (canonicalTarget.back() == L'\\' || canonicalTarget.back() == L'/')) {
        canonicalTarget.pop_back();
    }
    std::transform(canonicalTarget.begin(), canonicalTarget.end(), canonicalTarget.begin(), ::towlower);

    Microsoft::WRL::ComPtr<IShellWindows> pShellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, (void**)&pShellWindows);
    if (FAILED(hr) || !pShellWindows) return orderMap;

    long count = 0;
    pShellWindows->get_Count(&count);

    for (long i = 0; i < count; ++i) {
        VARIANT vIndex;
        vIndex.vt = VT_I4;
        vIndex.lVal = i;

        Microsoft::WRL::ComPtr<IDispatch> pDisp;
        if (FAILED(pShellWindows->Item(vIndex, &pDisp)) || !pDisp) continue;

        Microsoft::WRL::ComPtr<IWebBrowser2> pWebBrowser;
        if (FAILED(pDisp.As(&pWebBrowser)) || !pWebBrowser) continue;

        Microsoft::WRL::ComPtr<IServiceProvider> pServiceProvider;
        if (FAILED(pWebBrowser.As(&pServiceProvider)) || !pServiceProvider) continue;

        Microsoft::WRL::ComPtr<IShellBrowser> pShellBrowser;
        if (FAILED(pServiceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&pShellBrowser))) || !pShellBrowser) continue;

        Microsoft::WRL::ComPtr<IShellView> pShellView;
        if (FAILED(pShellBrowser->QueryActiveShellView(&pShellView)) || !pShellView) continue;

        Microsoft::WRL::ComPtr<IFolderView> pFolderView;
        if (FAILED(pShellView.As(&pFolderView)) || !pFolderView) continue;

        Microsoft::WRL::ComPtr<IPersistFolder2> pPersistFolder;
        if (FAILED(pFolderView->GetFolder(IID_PPV_ARGS(&pPersistFolder))) || !pPersistFolder) continue;

        PIDLIST_ABSOLUTE pidlFolder = nullptr;
        if (FAILED(pPersistFolder->GetCurFolder(&pidlFolder)) || !pidlFolder) continue;

        wchar_t folderPathBuf[MAX_PATH] = { 0 };
        BOOL gotPath = SHGetPathFromIDListW(pidlFolder, folderPathBuf);

        if (!gotPath) {
            CoTaskMemFree(pidlFolder);
            continue;
        }

        std::wstring currentFolder = fs::path(folderPathBuf).lexically_normal().wstring();
        while (!currentFolder.empty() && (currentFolder.back() == L'\\' || currentFolder.back() == L'/')) {
            currentFolder.pop_back();
        }
        std::transform(currentFolder.begin(), currentFolder.end(), currentFolder.begin(), ::towlower);

        if (currentFolder == canonicalTarget) {
            int itemCount = 0;
            if (SUCCEEDED(pFolderView->ItemCount(SVGIO_ALLVIEW, &itemCount)) && itemCount > 0) {
                Microsoft::WRL::ComPtr<IShellFolder> pShellFolder;
                pFolderView->GetFolder(IID_PPV_ARGS(&pShellFolder));

                orderMap.reserve(itemCount);
                for (int j = 0; j < itemCount; ++j) {
                    PITEMID_CHILD pidlItem = nullptr;
                    if (SUCCEEDED(pFolderView->Item(j, &pidlItem)) && pidlItem) {
                        wchar_t itemPathBuf[MAX_PATH] = { 0 };
                        bool pathResolved = false;

                        if (pShellFolder) {
                            STRRET strRet;
                            if (SUCCEEDED(pShellFolder->GetDisplayNameOf(pidlItem, SHGDN_FORPARSING, &strRet))) {
                                if (SUCCEEDED(StrRetToBufW(&strRet, pidlItem, itemPathBuf, MAX_PATH))) {
                                    pathResolved = true;
                                }
                            }
                        }

                        if (!pathResolved) {
                            PIDLIST_ABSOLUTE pidlFull = ILCombine(pidlFolder, pidlItem);
                            if (pidlFull) {
                                SHGetPathFromIDListW(pidlFull, itemPathBuf);
                                ILFree(pidlFull);
                                pathResolved = (itemPathBuf[0] != L'\0');
                            }
                        }

                        CoTaskMemFree(pidlItem);

                        if (pathResolved) {
                            ImageID id = ComputePathHash(itemPathBuf);
                            orderMap.emplace(id, static_cast<size_t>(j));
                        }
                    }
                }
            }
            CoTaskMemFree(pidlFolder);
            break;
        }
        CoTaskMemFree(pidlFolder);
    }

    return orderMap;
}

void FileNavigator::SortEntries(std::vector<SortEntry>& entries, int sortOrder, bool sortDesc, const std::wstring& dirPath) {
    // Helper to get pointer to null-terminated file/entry name substring to avoid dynamic allocations
    auto getSortNamePtr = [](const std::wstring& path) -> LPCWSTR {
        size_t lastPipe = path.find_last_of(L'|');
        if (lastPipe != std::wstring::npos) {
            return path.c_str() + lastPipe + 1;
        }
        size_t lastSlash = path.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return path.c_str() + lastSlash + 1;
        }
        return path.c_str();
    };

    std::unordered_map<ImageID, size_t> explorerOrder;
    if (sortOrder == 0 && !dirPath.empty()) {
        explorerOrder = GetExplorerWindowFileOrder(dirPath);
    }

    std::sort(entries.begin(), entries.end(), [sortOrder, sortDesc, &getSortNamePtr, &explorerOrder](const SortEntry& a, const SortEntry& b){
        int cmp = 0;
        LPCWSTR nameA = getSortNamePtr(a.p);
        LPCWSTR nameB = getSortNamePtr(b.p);
        switch (sortOrder) {
            case 0: // Auto (Explorer Order)
                if (!explorerOrder.empty()) {
                    ImageID idA = ComputePathHash(a.p);
                    ImageID idB = ComputePathHash(b.p);
                    auto itA = explorerOrder.find(idA);
                    auto itB = explorerOrder.find(idB);
                    if (itA != explorerOrder.end() && itB != explorerOrder.end()) {
                        if (itA->second < itB->second) cmp = -1;
                        else if (itA->second > itB->second) cmp = 1;
                    } else if (itA != explorerOrder.end()) {
                        cmp = -1;
                    } else if (itB != explorerOrder.end()) {
                        cmp = 1;
                    } else {
                        cmp = StrCmpLogicalW(nameA, nameB);
                    }
                    break;
                }
                [[fallthrough]];
            case 1: // Name
                cmp = StrCmpLogicalW(nameA, nameB);
                break;
            case 2: // Modified
                if (a.m < b.m) cmp = -1;
                else if (a.m > b.m) cmp = 1;
                else cmp = StrCmpLogicalW(nameA, nameB); // Fallback
                break;
            case 3: // Date Taken
                if (a.exifDate.empty() && !b.exifDate.empty()) cmp = 1; // Empty goes last
                else if (!a.exifDate.empty() && b.exifDate.empty()) cmp = -1;
                else {
                    cmp = a.exifDate.compare(b.exifDate);
                    if (cmp == 0) cmp = StrCmpLogicalW(nameA, nameB);
                }
                break;
            case 4: // Size
                if (a.s < b.s) cmp = -1;
                else if (a.s > b.s) cmp = 1;
                else cmp = StrCmpLogicalW(nameA, nameB);
                break;
            case 5: // Type
                cmp = StrCmpLogicalW(a.t.c_str(), b.t.c_str());
                if (cmp == 0) cmp = StrCmpLogicalW(nameA, nameB);
                break;
        }

        if (sortDesc) return cmp > 0;
        return cmp < 0;
    });
}

__declspec(noinline) std::vector<std::wstring> FileNavigator::GetSortedSiblings(const std::filesystem::path& parentDir) {
    std::vector<std::wstring> siblings;
    std::error_code ec;
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(parentDir, ec)) {
        if (entry.is_directory(ec)) {
            siblings.push_back(entry.path().wstring());
        } else if (entry.is_regular_file(ec)) {
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
            for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
                if (ext == supp) {
                    siblings.push_back(entry.path().wstring());
                    break;
                }
            }
        }
    }
    std::sort(siblings.begin(), siblings.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    return siblings;
}

std::wstring FileNavigator::FindAdjacentFolderImage(bool next) {
    if (m_files.empty() || m_currentIndex < 0 || m_currentIndex >= (int)m_files.size()) return L"";

    namespace fs = std::filesystem;
    std::wstring currentFile = m_files[m_currentIndex];

    fs::path currentPath(currentFile);
    fs::path parentDir = currentPath.parent_path();
    if (parentDir.empty() || parentDir == currentPath) return L"";

    std::vector<std::wstring> siblings = GetSortedSiblings(parentDir);
    if (siblings.empty()) return L"";

    std::wstring physicalStr = currentPath.wstring();
    auto it = std::find(siblings.begin(), siblings.end(), physicalStr);
    int idx = (it == siblings.end()) ? -1 : (int)std::distance(siblings.begin(), it);

    if (idx == -1) return L"";

    int nextIdx = next ? idx + 1 : idx - 1;
    
    // Boundary logic
    if (nextIdx < 0 || nextIdx >= (int)siblings.size()) {
        if (g_runtime.NavLoop) {
            nextIdx = (nextIdx < 0) ? (int)siblings.size() - 1 : 0;
        } else {
            return L"";
        }
    }

    int startIdx = nextIdx;
    while (true) {
        std::wstring sib = siblings[nextIdx];
        // Only folders are containers now — archives are no longer supported
        bool isContainer = fs::is_directory(sib);

        if (isContainer) {
            FileNavigator tempNav;
            tempNav.Initialize(sib);
            if (tempNav.Count() > 0) {
                return next ? tempNav.First() : tempNav.Last();
            }
        } else {
            return sib;
        }

        if (next) nextIdx++; else nextIdx--;
        if (nextIdx < 0 || nextIdx >= (int)siblings.size()) {
            if (g_runtime.NavLoop) {
                nextIdx = (nextIdx < 0) ? (int)siblings.size() - 1 : 0;
            } else {
                return L"";
            }
        }
        if (nextIdx == startIdx) break;
    }

    return L"";
}

FileNavigator::DirectoryScanResult FileNavigator::PerformDirectoryScan(const std::wstring& dir) {
    DirectoryScanResult result;
    result.dir = dir;
    namespace fs = std::filesystem;
    std::error_code ec;

    const int scanSortOrder = g_runtime.SortOrder;

    // [扫描提速] 枚举与 SortEntry 构建合并为一次遍历：directory_entry 自带
    // 枚举时缓存的文件大小/修改时间，避免逐文件按路径 last_write_time 重查
    // ——网络盘上那是每个文件一次元数据往返，几千文件的目录要多扫好几秒。
    // [渐进扫描] 枚举本身是流式的（NTFS/SMB 按名称序分批返回）：每积累
    // kScanProgressiveBatchFiles 个文件且距上次投递超过 kScanProgressiveMinMs
    // 就把已枚举部分排序为快照发给 UI，当前文件 ±20 邻居在枚举完成前就能显示。
    constexpr size_t kScanProgressiveBatchFiles = 256;
    constexpr uint64_t kScanProgressiveMinMs = 300;
    std::vector<SortEntry> entries;
    size_t lastPostedSize = 0;
    uint64_t lastPostTick = GetTickCount64();
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });

        // Skip archive container files from the flat folder slideshow playlist
        if (QuickView::IsArchiveExtension(ext)) continue;

        bool supported = false;
        for (const auto& supp : QuickView::SUPPORTED_EXTENSIONS) {
            if (ext == supp) { supported = true; break; }
        }
        if (!supported) continue;

        SortEntry e;
        e.p = entry.path().wstring();
        e.s = entry.file_size(ec);
        std::error_code tmEc;
        e.m = entry.last_write_time(tmEc);
        e.t = std::move(ext);

        if (scanSortOrder == 3) {
            FILE* fp = nullptr;
            _wfopen_s(&fp, e.p.c_str(), L"rb");
            if (fp) {
                unsigned char buf[65536];
                size_t bytes = fread(buf, 1, sizeof(buf), fp);
                fclose(fp);
                if (bytes > 0) {
                    easyexif::EXIFInfo info;
                    if (info.parseFrom(buf, (unsigned)bytes) == PARSE_EXIF_SUCCESS) {
                        e.exifDate = info.DateTimeOriginal;
                    }
                }
            }
        }

        entries.push_back(std::move(e));

        // [渐进扫描] 首批不限时间：SMB 大包瞬时返回时若也要求"距上次投递
        // 超 300ms"，第一次投递会被拦掉，冷缓存下邻居要等整个枚举结束
        const bool firstPost = (lastPostedSize == 0);
        if (entries.size() - lastPostedSize >= kScanProgressiveBatchFiles &&
            (firstPost || GetTickCount64() - lastPostTick >= kScanProgressiveMinMs)) {
            PostPartialScanResult(dir, entries);
            lastPostedSize = entries.size();
            lastPostTick = GetTickCount64();
        }
    }

    int sortOrder = scanSortOrder;
    bool sortDesc = g_runtime.SortDescending;
    SortEntries(entries, sortOrder, sortDesc, dir);

    // [RAW+JPEG Pairing] Same fold as Initialize (watcher rescan path)
    if (g_config.PairRawJpeg) {
        std::unordered_set<ImageID> skip;
        {
            std::lock_guard<std::mutex> lock(m_verifyMutex);
            skip = m_verifyUnpaired;
        }
        ApplyRawJpegPairing(entries, result.pairedRaws, skip.empty() ? nullptr : &skip);
    }

    result.files.clear();
    result.sizes.clear();
    for (const auto& e : entries) {
        result.files.push_back(e.p);
        result.sizes.push_back(e.s);
    }

    result.ids.reserve(result.files.size());
    for (const auto& f : result.files) {
        result.ids.push_back(ComputePathHash(f));
    }

    return result;
}

// [渐进扫描] 已枚举部分排序成部分快照投递给 UI。快照前缀稳定（NTFS/SMB
// 枚举按名称序返回），UI 换入后当前文件 ±20 邻居即可显示，不等全部枚举完。
void FileNavigator::PostPartialScanResult(const std::wstring& dir,
                                          const std::vector<SortEntry>& entries) {
    DirectoryScanResult partial;
    partial.dir = dir;
    partial.partial = true;

    std::vector<SortEntry> sorted = entries;
    SortEntries(sorted, g_runtime.SortOrder, g_runtime.SortDescending, dir);
    partial.files.reserve(sorted.size());
    partial.sizes.reserve(sorted.size());
    for (const auto& e : sorted) {
        partial.files.push_back(e.p);
        partial.sizes.push_back(e.s);
    }
    partial.ids.reserve(partial.files.size());
    for (const auto& f : partial.files) {
        partial.ids.push_back(ComputePathHash(f));
    }

    const size_t partialCount = partial.files.size();
    {
        std::lock_guard<std::mutex> lock(m_scanResultMutex);
        m_pendingScanResult = std::move(partial);
    }
    if (FILE* fp = _wfopen(L"E:\\qv_thumb_perf.log", L"a")) {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(fp, L"[%02u:%02u:%02u.%03u] [SCAN-PARTIAL] %ls -> %zu files\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 dir.c_str(), partialCount);
        fclose(fp);
    }
    if (m_hwnd) PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 1, 0);
}

void FileNavigator::WatcherThreadProc() {
    HANDLE hNotify = FindFirstChangeNotificationW(
        m_watchedDir.c_str(),
        FALSE,                          // Non-recursive (current directory only)
        FILE_NOTIFY_CHANGE_FILE_NAME    // File create, delete, rename only
    );
    if (hNotify == INVALID_HANDLE_VALUE) return;

    HANDLE handles[2] = { hNotify, m_hCancelEvent };

    while (true) {
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 + 1) break;  // Cancel event signaled
        if (wait != WAIT_OBJECT_0) break;       // Error or abandoned

        // === Coalescing / Debounce Loop (300ms) ===
        // Drain all rapid-fire events until 300ms of silence
        bool cancelled = false;
        while (true) {
            if (!FindNextChangeNotification(hNotify)) {
                cancelled = true; // Directory removed or device ejected
                break;
            }
            DWORD r = WaitForMultipleObjects(2, handles, FALSE, 300);
            if (r == WAIT_OBJECT_0 + 1) { cancelled = true; break; } // Cancel
            if (r == WAIT_TIMEOUT) break; // 300ms silence — proceed to scan
            if (r != WAIT_OBJECT_0) { cancelled = true; break; } // Error
            // r == WAIT_OBJECT_0: more changes arrived, loop again (reset timer)
        }
        if (cancelled) break;

        // === Background Scan (on this thread, zero UI impact) ===
        auto scanResult = PerformDirectoryScan(m_watchedDir);

        {
            std::lock_guard<std::mutex> lock(m_scanResultMutex);
            m_pendingScanResult = std::move(scanResult);
        }
        PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 0, 0);

        // Re-arm for next batch of changes
        if (!FindNextChangeNotification(hNotify)) break; // Directory gone
    }

    FindCloseChangeNotification(hNotify);
}

void FileNavigator::StartDirectoryWatcher(const std::wstring& dirPath) {
    m_watchedDir = dirPath;

    // Create manual-reset event (initially non-signaled) for graceful shutdown
    m_hCancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hCancelEvent) return;

    m_watcherThread = std::thread(&FileNavigator::WatcherThreadProc, this);
}

void FileNavigator::StopInitialScan() {
    {
        std::lock_guard<std::mutex> lock(m_scanWorkMutex);
        m_scanWorkerStop = true;
    }
    m_scanWorkCv.notify_all();
    if (m_scanWorker.joinable()) {
        m_scanWorker.join();
    }
}

// 常驻扫描线程：处理 Initialize 提交的全量目录扫描请求（串行，最新优先）
void FileNavigator::ScanWorkerLoop() {
    for (;;) {
        std::wstring dir;
        uint32_t genAtPick = 0;
        {
            std::unique_lock<std::mutex> lock(m_scanWorkMutex);
            m_scanWorkCv.wait(lock, [this] { return m_scanWorkPending || m_scanWorkerStop; });
            if (m_scanWorkerStop) return;
            m_scanWorkPending = false;
            dir = m_scanDir;
            genAtPick = m_scanGeneration.load();
        }

        const uint64_t scanStart = GetTickCount64();
        DirectoryScanResult result = PerformDirectoryScan(dir);
        const uint64_t scanMs = GetTickCount64() - scanStart;
        // [扫描提速验证] 记录全量目录扫描耗时（与缩略图性能日志同文件）
        if (FILE* fp = _wfopen(L"E:\\qv_thumb_perf.log", L"a")) {
            SYSTEMTIME st; GetLocalTime(&st);
            fwprintf(fp, L"[%02u:%02u:%02u.%03u] [SCAN] %ls -> %zu files, %llu ms\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     dir.c_str(), result.files.size(),
                     (unsigned long long)scanMs);
            fclose(fp);
        }

        if (m_scanGeneration.load() != genAtPick) continue; // 已被新导航取代

        {
            std::lock_guard<std::mutex> lock(m_scanResultMutex);
            m_pendingScanResult = std::move(result);
        }
        if (m_hwnd) PostMessageW(m_hwnd, WM_NAVIGATOR_DIR_CHANGED, 0, 0);
    }
}

void FileNavigator::StopDirectoryWatcher() {
    if (m_hCancelEvent) {
        SetEvent(m_hCancelEvent); // Signal cancellation
    }
    if (m_watcherThread.joinable()) {
        m_watcherThread.join();   // Wait for clean exit
    }
    if (m_hCancelEvent) {
        CloseHandle(m_hCancelEvent);
        m_hCancelEvent = nullptr;
    }
    // Discard any unprocessed result
    std::lock_guard<std::mutex> lock(m_scanResultMutex);
    m_pendingScanResult.reset();
}
