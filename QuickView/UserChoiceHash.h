#pragma once
// ============================================================================
// UserChoiceHash.h — 文件关联 UserChoice 的「合法哈希写入」(强制接管默认应用)
// ============================================================================
// Windows 对 FileExts\<ext>\UserChoice 的 ProgId 附加 Hash 校验：手写 ProgId 而
// 没有正确 Hash 会被 Shell 忽略。本模块实现与系统一致的 Hash 计算算法（逆向自
// Windows，与 SetUserFTA / PS-SFTA 同源），从而可以把任意 ProgId 写成有效
// UserChoice —— 这是在 SetAppAsDefault API 失效的系统上（实测部分 Win10 19045
// 对所有扩展名返回 E_FAIL）唯一能真正接管默认应用、且不被「照片」AppX / Edge
// 自动回收的方式（自动回收只填补"缺失"的 UserChoice，不会覆盖有效值）。
//
// 算法要点：
//   baseInfo = (扩展名 + 用户SID + ProgId + 按分钟取整的FILETIME十六进制 +
//               shell32 内嵌的 "User Choice set via Windows User Experience
//               {GUID}" 字符串) 全部小写，UTF-16LE 编码后补 0x00,0x00，
//               先 MD5，再过两轮自定义乘法混合，取两轮结果异或后的 8 字节做
//               Base64。写入时须在同一分钟内完成（校验对齐 UserChoice 键的
//               最后写入时间，按分钟粒度）。
// ============================================================================

#include <string>

namespace QuickView {

// 把 <ext>（如 L".jpg"）的默认应用 UserChoice 强制写为 <progId>（如 L"QuickView.jpg"）。
// 过程：删除旧 UserChoice 键 → 重建并写入 Hash + ProgId → 写 ApplicationAssociationToasts
// 抑制"新应用可用"提示。全部 HKCU 操作，无需管理员。成功返回 true。
bool ForceUserChoiceAssociation(const std::wstring& ext, const std::wstring& progId);

} // namespace QuickView
