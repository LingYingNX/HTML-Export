#pragma once
// 基础工具：字符串转换、JSON 读写、base64 解码。
// 全部手写，不依赖任何第三方库——这是把 exe 控制在 200KB 以内的前提。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace util {

// ---------------- 字符串 ----------------
inline std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::wstring U82W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

inline std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}

inline std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

inline bool EndsWithNoCase(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

inline bool EndsWithNoCase8(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    if (s.size() < n) return false;
    return _stricmp(s.c_str() + s.size() - n, suf) == 0;
}

// ---------------- JSON 输出转义 ----------------
inline std::string JEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// ---------------- JSON 读取（够用即可：本工具只解析自己前端发来的固定结构）----------------

// 找到 "key" 之后的值起始位置（跳过冒号和空白）
inline size_t FindValue(const std::string& j, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = 0;
    while ((p = j.find(pat, p)) != std::string::npos) {
        size_t q = p + pat.size();
        while (q < j.size() && ((unsigned char)j[q] <= ' ')) q++;
        if (q < j.size() && j[q] == ':') {
            q++;
            while (q < j.size() && ((unsigned char)j[q] <= ' ')) q++;
            return q;
        }
        p = q;
    }
    return std::string::npos;
}

// 解析 JSON 字符串字面量（支持 \" \\ \n \r \t \uXXXX）
inline std::string ParseJsonString(const std::string& j, size_t p) {
    if (p >= j.size() || j[p] != '"') return std::string();
    std::string out;
    for (size_t i = p + 1; i < j.size(); i++) {
        char c = j[i];
        if (c == '\\' && i + 1 < j.size()) {
            char n = j[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 < j.size()) {
                        char hex[5] = { j[i+1], j[i+2], j[i+3], j[i+4] };
                        unsigned cp = (unsigned)strtoul(hex, nullptr, 16);
                        i += 4;
                        // 只处理 BMP 内的码点，转成 UTF-8
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out += n;
            }
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

inline std::string JStr(const std::string& j, const std::string& key, const std::string& def = "") {
    size_t p = FindValue(j, key);
    if (p == std::string::npos) return def;
    if (j[p] == '"') return ParseJsonString(j, p);
    return def;
}

inline double JNum(const std::string& j, const std::string& key, double def) {
    size_t p = FindValue(j, key);
    if (p == std::string::npos) return def;
    if (j[p] == '"') {
        std::string s = ParseJsonString(j, p);
        return s.empty() ? def : atof(s.c_str());
    }
    const char* start = j.c_str() + p;
    char* end = nullptr;
    double v = strtod(start, &end);
    return end == start ? def : v;
}

inline bool JBool(const std::string& j, const std::string& key, bool def) {
    size_t p = FindValue(j, key);
    if (p == std::string::npos) return def;
    if (j.compare(p, 4, "true") == 0) return true;
    if (j.compare(p, 5, "false") == 0) return false;
    return def;
}

// ---------------- base64 ----------------
// canvas.toDataURL() 返回的是 base64 编码的 PNG，这里解码后直接写盘，
// 省掉了在 C++ 里实现 PNG 编码器和 zlib 压缩。
inline std::vector<unsigned char> Base64Decode(const std::string& in) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) table[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) table[(unsigned char)a[i]] = (int8_t)i;
        init = true;
    }
    std::vector<unsigned char> out;
    out.reserve(in.size() / 4 * 3 + 3);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=' ) break;
        int8_t d = table[c];
        if (d < 0) continue;   // 跳过换行等空白
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((unsigned char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// ---------------- 文件 ----------------

// CreateFileW 等 Win32 API 不认正斜杠形式的绝对路径（"C:/a/b.html" 会失败），
// 而命令行、拖拽、浏览器传来的路径常带正斜杠，所以统一转成反斜杠。
inline std::wstring ToWinPath(std::wstring p) {
    for (wchar_t& c : p) if (c == L'/') c = L'\\';
    // 去掉 Win32 不接受的 \\?\ 之外的前缀空白
    return p;
}

inline bool ReadFileBytes(const std::wstring& pathIn, std::string& out, std::string* err = nullptr) {
    std::wstring path = ToWinPath(pathIn);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "找不到文件";
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 64ll * 1024 * 1024) {
        CloseHandle(h);
        if (err) *err = "文件过大";
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        if (err) *err = "读取失败";
        return false;
    }
    out.resize(read);
    // 去掉 UTF-8 BOM
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

inline bool WriteFileBytes(const std::wstring& pathIn, const void* data, size_t len) {
    std::wstring path = ToWinPath(pathIn);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = len == 0 ? TRUE : WriteFile(h, data, (DWORD)len, &written, nullptr);
    CloseHandle(h);
    return ok && written == len;
}

}  // namespace util
