#pragma once
// ffmpeg 封装。
//
// 关键点：把每帧的 PNG 原样写进 ffmpeg 的 stdin（用 image2pipe 解复用器），
// ffmpeg 自己会解码 PNG —— 于是 C++ 侧完全不需要图像解码库。
//
// ffmpeg 查找顺序：exe 同目录的 ffmpeg.exe > PATH 里的 ffmpeg。
// 找不到时 MP4 功能禁用，PNG 序列帧不受影响。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include "util.h"

class FFmpeg {
public:
    FFmpeg() {}
    ~FFmpeg() { if (proc_) Finish(); }

    // 是否可用（探测一次并缓存）
    bool Available() {
        if (probed_) return available_;
        probed_ = true;
        // 优先用 exe 同目录的 ffmpeg.exe
        std::wstring local = util::JoinPath(util::ExeDir(), L"ffmpeg.exe");
        if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) {
            cmd_ = local;
            available_ = true;
            return true;
        }
        // 退回 PATH
        wchar_t found[MAX_PATH] = {0};
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) {
            cmd_ = found;
            available_ = true;
            return true;
        }
        available_ = false;
        return false;
    }

    static std::string VersionString() {
        std::wstring local = util::JoinPath(util::ExeDir(), L"ffmpeg.exe");
        std::wstring cmd = L"ffmpeg.exe";
        if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) cmd = local;
        // 简单跑一次 -version 取首行
        std::wstring full = L"\"" + cmd + L"\" -version";
        FILE* p = _wpopen(full.c_str(), L"r");
        if (!p) return std::string();
        char line[512] = {0};
        std::string first;
        if (fgets(line, sizeof(line), p)) first = line;
        _pclose(p);
        while (!first.empty() && (first.back() == '\n' || first.back() == '\r')) first.pop_back();
        // 只保留版本号部分，避免把整行版权信息塞进界面
        size_t sp = first.find(" Copyright");
        if (sp != std::string::npos) first = first.substr(0, sp);
        return first;
    }

    // 启动管道：从 stdin 读 PNG 序列，编码成 H.264 MP4
    bool StartPipe(const std::wstring& outPath, int width, int height, int fps) {
        if (!Available()) return false;

        std::wstring cmd = L"\"" + cmd_ + L"\" -y -loglevel error"
                           L" -f image2pipe -vcodec png -framerate " + std::to_wstring(fps) +
                           L" -i - -c:v libx264 -preset slow -crf 16"
                           L" -pix_fmt yuv420p -movflags +faststart \"" + outPath + L"\"";

        SECURITY_ATTRIBUTES sa = {0};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {0};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = rd;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi = {0};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);

        BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(rd);
        if (!ok) {
            CloseHandle(wr);
            return false;
        }

        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        stdin_ = wr;
        return true;
    }

    // 写入一帧（PNG 字节原样送入，ffmpeg 自行解码）
    bool WritePngFrame(const unsigned char* data, size_t len) {
        if (!stdin_ || !proc_) return false;
        size_t off = 0;
        while (off < len) {
            DWORD chunk = (DWORD)((len - off) > (1u << 20) ? (1u << 20) : (len - off));
            DWORD written = 0;
            if (!WriteFile(stdin_, data + off, chunk, &written, nullptr) || written == 0) return false;
            off += written;
        }
        return true;
    }

    // 结束输入并等 ffmpeg 收尾
    bool Finish() {
        if (stdin_) { CloseHandle(stdin_); stdin_ = nullptr; }
        if (!proc_) return false;
        DWORD code = 0;
        WaitForSingleObject(proc_, 300000);
        GetExitCodeProcess(proc_, &code);
        CloseHandle(proc_);
        proc_ = nullptr;
        return code == 0;
    }

private:
    bool probed_ = false;
    bool available_ = false;
    std::wstring cmd_;
    HANDLE proc_ = nullptr;
    HANDLE stdin_ = nullptr;
};
