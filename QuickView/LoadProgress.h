#pragma once
#include <windows.h>
#include <cstdint>
#include <atomic>

// ============================================================================
// LoadProgress.h - 中央转圈加载环的进度实体
// ----------------------------------------------------------------------------
// 设计说明（事实为本 / KISS）：
//   解码器（TurboJPEG / Wuffs / libavif / libjxl / libraw 等）大多为"一次性"
//   解码，不暴露真实的逐字节进度回调；逐解码器注入字节计数既不可行也易破坏
//   解码逻辑。因此：
//   - 百分比：用"文件大小 + 估算解码吞吐"做的时间模型估算（行业通行做法）。
//   - 速度(MB/s)：真实值 = fileSize / 已用时间，主线程计时即可，绝不动解码路径。
//   两者均由主线程在 UI 心跳中读取，纯展示、零风险。
// ============================================================================

namespace QuickView {

struct LoadProgressState {
    std::atomic<bool>   active{false};            // 是否有一次可取消的加载进行中
    std::atomic<bool>   cancellable{false};       // 当前加载是否可被用户打断
    std::atomic<bool>   visibleAfterDelay{false}; // 延迟门：超过阈值后才显示环，避免快图闪烁
    std::atomic<bool>   cancelled{false};         // 已被用户取消
    std::atomic<uint64_t> fileSize{0};            // 文件总字节数
    std::atomic<uint64_t> startTimeMs{0};         // 加载开始时间戳(GetTickCount64)
    std::atomic<uint64_t> estTotalMs{0};          // 估算总解码耗时(用于百分比估算)
};

} // namespace QuickView

// 全局单例（定义于 main.cpp）
extern QuickView::LoadProgressState g_loadProgress;

// 转圈环点击命中所需的几何（由 UIRenderer 写入，main.cpp 鼠标命中测试读取）
extern int   g_spinnerCx;
extern int   g_spinnerCy;
extern float g_spinnerR;

// [加载环] 取消✕按钮命中几何（板右上角，由 UIRenderer 写入，main.cpp 鼠标命中测试读取）
extern int   g_spinnerCancelX;
extern int   g_spinnerCancelY;
extern float g_spinnerCancelR;

// 延迟门阈值：加载超过此时长(ms)才显示转圈环，快图(<~180ms)直接出图不闪
constexpr uint64_t kLoadSpinnerDelayMs = 180;

// 初始化一次加载（于 StartNavigation）。fileSizeHint 为已知文件大小，0 时自行探测。
inline void InitLoadProgress(const wchar_t* path, uint64_t fileSizeHint) {
    uint64_t fs = fileSizeHint;
    if (fs == 0 && path && path[0]) {
        HANDLE h = CreateFileW(path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(h, &sz)) fs = (uint64_t)sz.QuadPart;
            CloseHandle(h);
        }
    }
    g_loadProgress.fileSize.store(fs);
    g_loadProgress.startTimeMs.store(GetTickCount64());

    // 估算总耗时：按 ~12 MB/s 有效解码吞吐建模，并夹紧到合理区间
    double estMs = 250.0;
    if (fs > 0) estMs = (double)fs / (12.0 * 1024.0 * 1024.0) * 1000.0;
    if (estMs < 250.0)  estMs = 250.0;
    if (estMs > 6000.0) estMs = 6000.0;
    g_loadProgress.estTotalMs.store((uint64_t)estMs);

    g_loadProgress.cancelled.store(false);
    g_loadProgress.visibleAfterDelay.store(false);
    g_loadProgress.cancellable.store(true);
    g_loadProgress.active.store(true);
}

// 加载结束 / 取消时复位（隐藏转圈环）
inline void ResetLoadProgress() {
    g_loadProgress.active.store(false);
    g_loadProgress.cancellable.store(false);
    g_loadProgress.visibleAfterDelay.store(false);
}

// 读取当前百分比(估算)与速度(MB/s，真实)。outPercent/outMBps 可为 nullptr。
inline void QueryLoadProgress(float* outPercent, float* outMBps) {
    const uint64_t start = g_loadProgress.startTimeMs.load();
    const uint64_t now   = GetTickCount64();
    const uint64_t el    = (now > start) ? (now - start) : 0;
    const uint64_t fs    = g_loadProgress.fileSize.load();

    if (outPercent) {
        uint64_t est = g_loadProgress.estTotalMs.load();
        float pct = (est > 0) ? (float)((double)el / (double)est) : 0.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 0.99f) pct = 0.99f; // 未真正完成前不显示 100%
        *outPercent = pct;
    }
    if (outMBps) {
        if (el > 0 && fs > 0) {
            double bytesPerSec = (double)fs / ((double)el / 1000.0);
            *outMBps = (float)(bytesPerSec / (1024.0 * 1024.0));
        } else {
            *outMBps = 0.0f;
        }
    }
}
