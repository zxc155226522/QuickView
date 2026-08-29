#include "UserChoiceHash.h"

#include <windows.h>
#include <bcrypt.h>
#include <ntstatus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <cwctype>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace QuickView {

namespace {

// ---------------------------------------------------------------- MD5 (bcrypt)

bool ComputeMd5(const std::vector<uint8_t>& data, uint8_t out[16]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, L"MD5", nullptr, 0) != STATUS_SUCCESS) return false;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == STATUS_SUCCESS;
    if (ok) ok = BCryptHashData(hHash, const_cast<PUCHAR>(data.data()), (ULONG)data.size(), 0) == STATUS_SUCCESS;
    if (ok) ok = BCryptFinishHash(hHash, out, 16, 0) == STATUS_SUCCESS;
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ------------------------------------------------------------------- 混合循环

inline int32_t GetLongAt(const std::vector<uint8_t>& bytes, size_t off) {
    int32_t v = 0;
    std::memcpy(&v, bytes.data() + off, sizeof(v));
    return v;
}

inline int32_t GetLongAt(const uint8_t* bytes, size_t off) {
    int32_t v = 0;
    std::memcpy(&v, bytes + off, sizeof(v));
    return v;
}

// 32 位逻辑右移 16（零填充）。注意：参考实现的 Get-ShiftRight 在 64 位中间值上
// XOR 0xFFFF0000，截断到 32 位参与乘法后的净效果就是逻辑右移——算术右移会在
// 负数上产生不同结果（已用 PS-SFTA 实测输出校验）。
inline int64_t shr16(int32_t v) { return (int64_t)((uint32_t)v >> 16); }

inline void PutLongAt(std::vector<uint8_t>& bytes, size_t off, int32_t v) {
    std::memcpy(bytes.data() + off, &v, sizeof(v));
}

inline void PutLongAt(uint8_t* bytes, size_t off, int32_t v) {
    std::memcpy(bytes + off, &v, sizeof(v));
}

// baseInfo 的 UTF-16LE 字节（含 0x00,0x00 结尾）+ 其 MD5 → 8 字节哈希（写入 outHashBase）
bool MixHash(const std::vector<uint8_t>& baseInfoBytes, const uint8_t md5[16], uint8_t outHashBase[8]) {
    // lengthBase = 字节数（含结尾 2 个 0）
    const size_t lengthBase = baseInfoBytes.size();
    const int length = (int)(((lengthBase & 4) == 0 ? 1 : 0) + (lengthBase >> 2) - 1);
    const int counterStart = ((length - 2) >> 1) + 1;
    if (counterStart <= 0) return false; // 数据过短（不会发生在真实扩展名上）

    std::vector<uint8_t> outHash(16, 0);

    // ---- 第一轮 ----
    {
        int64_t md51 = (int64_t)(GetLongAt(md5, 0) | 1) + 0x69FB0000LL;
        int64_t md52 = (int64_t)(GetLongAt(md5 + 4, 0) | 1) + 0x13DB0000LL;
        int32_t outhash1 = 0, outhash2 = 0;
        int64_t cache = 0;
        size_t pdata = 0;
        int counter = counterStart;
        while (counter) {
            int32_t r0 = (int32_t)((int64_t)GetLongAt(baseInfoBytes, pdata) + outhash1);
            int32_t r1_0 = GetLongAt(baseInfoBytes, pdata + 4);
            pdata += 8;
            int32_t r2_0 = (int32_t)((int64_t)r0 * md51 - 0x10FA9605LL * shr16(r0));
            int32_t r2_1 = (int32_t)(0x79F8A395LL * (int64_t)r2_0 + 0x689B6B9FLL * shr16(r2_0));
            int32_t r3 = (int32_t)(0xEA970001LL * (int64_t)r2_1 - 0x3C101569LL * shr16(r2_1));
            int32_t r4_0 = (int32_t)((int64_t)r3 + r1_0);
            int32_t r5_0 = (int32_t)(cache + r3);
            int32_t r6_0 = (int32_t)((int64_t)r4_0 * md52 - 0x3CE8EC25LL * shr16(r4_0));
            int32_t r6_1 = (int32_t)(0x59C3AF2DLL * (int64_t)r6_0 - 0x2232E0F1LL * shr16(r6_0));
            outhash1 = (int32_t)(0x1EC90001LL * (int64_t)r6_1 + 0x35BD1EC9LL * shr16(r6_1));
            outhash2 = (int32_t)((int64_t)r5_0 + (int64_t)outhash1);
            cache = (int64_t)outhash2;
            counter--;
        }
        PutLongAt(outHash, 0, outhash1);
        PutLongAt(outHash, 4, outhash2);
    }

    // ---- 第二轮 ----
    {
        int32_t md51 = GetLongAt(md5, 0) | 1;
        int32_t md52 = GetLongAt(md5 + 4, 0) | 1;
        int32_t outhash1 = 0, outhash2 = 0;
        int64_t cache = 0;
        size_t pdata = 0;
        int counter = counterStart;
        while (counter) {
            int32_t r0 = (int32_t)((int64_t)GetLongAt(baseInfoBytes, pdata) + outhash1);
            pdata += 8;
            int32_t r1_0 = (int32_t)((int64_t)r0 * (int64_t)md51);
            int32_t r1_1 = (int32_t)(0xB1110000LL * (int64_t)r1_0 - 0x30674EEFLL * shr16(r1_0));
            int32_t r2_0 = (int32_t)(0x5B9F0000LL * (int64_t)r1_1 - 0x78F7A461LL * shr16(r1_1));
            int32_t r2_1 = (int32_t)(0x12CEB96DLL * shr16(r2_0) - 0x46930000LL * (int64_t)r2_0);
            int32_t r3 = (int32_t)(0x1D830000LL * (int64_t)r2_1 + 0x257E1D83LL * shr16(r2_1));
            int32_t r4_0 = (int32_t)((int64_t)md52 * ((int64_t)r3 + GetLongAt(baseInfoBytes, pdata - 4)));
            int32_t r4_1 = (int32_t)(0x16F50000LL * (int64_t)r4_0 - 0x5D8BE90BLL * shr16(r4_0));
            int32_t r5_0 = (int32_t)(0x96FF0000LL * (int64_t)r4_1 - 0x2C7C6901LL * shr16(r4_1));
            int32_t r5_1 = (int32_t)(0x2B890000LL * (int64_t)r5_0 + 0x7C932B89LL * shr16(r5_0));
            outhash1 = (int32_t)(0x9F690000LL * (int64_t)r5_1 - 0x405B6097LL * shr16(r5_1));
            outhash2 = (int32_t)((int64_t)outhash1 + cache + (int64_t)r3);
            cache = (int64_t)outhash2;
            counter--;
        }
        PutLongAt(outHash, 8, outhash1);
        PutLongAt(outHash, 12, outhash2);
    }

    // ---- 两轮结果异或 → 8 字节 ----
    int32_t h1 = GetLongAt(outHash, 8) ^ GetLongAt(outHash, 0);
    int32_t h2 = GetLongAt(outHash, 12) ^ GetLongAt(outHash, 4);
    PutLongAt(outHashBase, 0, h1);
    PutLongAt(outHashBase, 4, h2);
    return true;
}

// ------------------------------------------------------------------ 组成部分

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

// 当前用户 SID（小写字符串，如 "s-1-5-21-..."）
bool GetUserSidLower(std::wstring& sidOut) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    std::vector<uint8_t> buf(len ? len : 1, 0);
    bool ok = GetTokenInformation(hToken, TokenUser, buf.data(), len, &len) != FALSE;
    CloseHandle(hToken);
    if (!ok) return false;
    const TOKEN_USER* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
    LPWSTR str = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &str)) return false;
    sidOut = ToLower(str);
    LocalFree(str);
    return true;
}

// 本机 shell32.dll 内嵌的 UserExperience 字符串（GUID 随系统版本而异，须现场提取）
// 取 32 位壳（SysWOW64；32 位系统即 System32），与参考实现一致。
bool GetUserExperienceString(std::wstring& expOut) {
    wchar_t sysX86[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_SYSTEMX86, nullptr, 0, sysX86))) return false;
    std::wstring dllPath = std::wstring(sysX86) + L"\\shell32.dll";
    HANDLE hFile = CreateFileW(dllPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    constexpr DWORD kReadBytes = 5u * 1024u * 1024u;
    std::vector<uint8_t> bytes(kReadBytes);
    DWORD read = 0;
    ReadFile(hFile, bytes.data(), kReadBytes, &read, nullptr);
    CloseHandle(hFile);
    if (read < 2) return false;

    const std::wstring data((const wchar_t*)bytes.data(), read / 2);
    const std::wstring needle = L"User Choice set via Windows User Experience";
    size_t p1 = data.find(needle);
    if (p1 == std::wstring::npos) return false;
    size_t p2 = data.find(L'}', p1);
    if (p2 == std::wstring::npos || p2 - p1 + 1 > 512) return false;
    expOut = data.substr(p1, p2 - p1 + 1);
    return true;
}

// 当前时间按分钟取整后的 FILETIME，格式化 hi(8hex)+lo(8hex) 小写字符串
bool GetHexDateTime(std::wstring& out) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    st.wSecond = 0;
    st.wMilliseconds = 0;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    const uint64_t u = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    wchar_t buf[32];
    swprintf_s(buf, L"%08x%08x", (unsigned)(u >> 32), (unsigned)(u & 0xFFFFFFFFu));
    out = buf;
    return true;
}

std::string Base64_8Bytes(const uint8_t in[8]) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(12);
    for (size_t i = 0; i < 8; i += 3) {
        uint32_t chunk = in[i] << 16;
        if (i + 1 < 8) chunk |= in[i + 1] << 8;
        if (i + 2 < 8) chunk |= in[i + 2];
        out += kAlphabet[(chunk >> 18) & 0x3F];
        out += kAlphabet[(chunk >> 12) & 0x3F];
        out += (i + 1 < 8) ? kAlphabet[(chunk >> 6) & 0x3F] : '=';
        out += (i + 2 < 8) ? kAlphabet[chunk & 0x3F] : '=';
    }
    return out;
}

} // namespace

bool ForceUserChoiceAssociation(const std::wstring& ext, const std::wstring& progId) {
    std::wstring sid, experience, hexTime;
    if (!GetUserSidLower(sid)) { return false; }
    if (!GetUserExperienceString(experience)) {
        experience = L"User Choice set via Windows User Experience {D18B6DD5-6124-4341-9318-804003BAFA0B}";
    }
    if (!GetHexDateTime(hexTime)) { return false; }

    // baseInfo = ext + sid + progId + time + experience，整体转小写（与系统算法一致）
    std::wstring baseInfo = ToLower(ext + sid + progId + hexTime + experience);

    std::vector<uint8_t> bytes;
    bytes.reserve(baseInfo.size() * 2 + 2 + 8);
    for (wchar_t c : baseInfo) {
        bytes.push_back((uint8_t)(c & 0xFF));
        bytes.push_back((uint8_t)(c >> 8));
    }
    bytes.push_back(0x00);
    bytes.push_back(0x00);

    uint8_t md5[16] = {};
    if (!ComputeMd5(bytes, md5)) { return false; }

    uint8_t hashBase[8] = {};
    if (!MixHash(bytes, md5, hashBase)) { return false; }
    const std::string hash = Base64_8Bytes(hashBase);

    // ---- 写注册表：删除旧键 → 重建（键时间戳与哈希时间须同一分钟）→ Hash/ProgId ----
    const std::wstring ucKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext +
                               L"\\UserChoice";
    RegDeleteTreeW(HKEY_CURRENT_USER, ucKey.c_str());

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ucKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) !=
        ERROR_SUCCESS) { return false; }
    RegSetValueExW(hKey, L"Hash", 0, REG_SZ, (const BYTE*)hash.c_str(), (DWORD)(hash.size() + 1) * sizeof(char));
    RegSetValueExW(hKey, L"ProgId", 0, REG_SZ, (const BYTE*)progId.c_str(), (DWORD)(progId.size() + 1) * sizeof(wchar_t));
    RegCloseKey(hKey);

    // 抑制"此应用的新版本可用/新的打开方式"提示气泡
    HKEY hToasts = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hToasts, nullptr) == ERROR_SUCCESS) {
        const std::wstring toastValue = progId + L"_" + ext;
        DWORD zero = 0;
        RegSetValueExW(hToasts, toastValue.c_str(), 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
        RegCloseKey(hToasts);
    }
    return true;
}

} // namespace QuickView
