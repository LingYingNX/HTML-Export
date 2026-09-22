#pragma once
// 从 HTML 源码里识别导出所需参数：画布尺寸、帧率、时长。
// 只做文本扫描，不执行脚本——真正执行的是 WebView2 里的页面本身。
// 识别不到的项留空，由用户在界面上填。

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include "util.h"

namespace Inspector {

struct Result {
    int width = 0;
    int height = 0;
    std::string sizeSource;
    double duration = 0;      // 秒，0 表示未识别
    int fps = 30;
    std::string fpsSource;
    double tickMs = 0;        // 画面实际变化间隔（如 STEP_INTERVAL_MS）
    int scriptCount = 0;
    int canvasCount = 0;
    std::vector<std::string> warnings;
};

// 在源码里找 "名字 = 数字" 或 "名字: 数字"
inline bool FindNumberAfter(const std::string& src, const std::string& name, double& out) {
    size_t p = 0;
    while ((p = src.find(name, p)) != std::string::npos) {
        size_t q = p + name.size();
        // 名字后面必须是分隔符，避免 TOTAL_DURATION_SEC 命中 DURATION_SEC 之类的前缀误判
        if (q < src.size()) {
            char c = src[q];
            bool boundary = (c == '=' || c == ':' || (unsigned char)c <= ' ' || c == '*' || c == '+' || c == '/');
            if (boundary) {
                while (q < src.size() && ((unsigned char)src[q] <= ' ' || src[q] == '=' || src[q] == ':')) q++;
                // 允许形如 70 * 1000 的写法
                const char* start = src.c_str() + q;
                char* end = nullptr;
                double v = strtod(start, &end);
                if (end != start) {
                    out = v;
                    return true;
                }
            }
        }
        p = q;
    }
    return false;
}

// 找 HTML 属性：width="1700" / width='1700' / width=1700
inline bool FindAttr(const std::string& tag, const std::string& attr, int& out) {
    size_t p = 0;
    while ((p = tag.find(attr, p)) != std::string::npos) {
        size_t q = p + attr.size();
        while (q < tag.size() && ((unsigned char)tag[q] <= ' ')) q++;
        if (q < tag.size() && tag[q] == '=') {
            q++;
            while (q < tag.size() && ((unsigned char)tag[q] <= ' ')) q++;
            char quote = 0;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) { quote = tag[q]; q++; }
            const char* start = tag.c_str() + q;
            char* end = nullptr;
            long v = strtol(start, &end, 10);
            if (end != start && v > 0) {
                if (quote == 0 || (q + (size_t)(end - start) < tag.size() && tag[q + (end - start)] == quote)) {
                    out = (int)v;
                    return true;
                }
            }
        }
        p = q;
    }
    return false;
}

inline int CountOccurrences(const std::string& src, const std::string& needle) {
    int n = 0;
    size_t p = 0;
    while ((p = src.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
    return n;
}

inline Result Analyze(const std::string& rawHtml) {
    Result r;
    std::string html = rawHtml;

    // ---- canvas 标签 ----
    size_t cp = 0;
    int firstW = 0, firstH = 0;
    while ((cp = html.find("<canvas", cp)) != std::string::npos) {
        size_t end = html.find('>', cp);
        if (end == std::string::npos) break;
        std::string tag = html.substr(cp, end - cp);
        r.canvasCount++;
        int w = 0, h = 0;
        bool hw = FindAttr(tag, "width", w);
        bool hh = FindAttr(tag, "height", h);
        if (r.canvasCount == 1) {
            if (hw) firstW = w;
            if (hh) firstH = h;
        }
        cp = end;
    }
    if (firstW > 0 && firstH > 0) {
        r.width = firstW;
        r.height = firstH;
        r.sizeSource = "<canvas> 标签";
    }

    // ---- script 数量与外部脚本检查 ----
    r.scriptCount = CountOccurrences(html, "<script");
    if (CountOccurrences(html, "<script") > 0) {
        // 粗略检测 <script src=...>
        size_t sp = 0;
        while ((sp = html.find("<script", sp)) != std::string::npos) {
            size_t end = html.find('>', sp);
            if (end == std::string::npos) break;
            std::string tag = html.substr(sp, end - sp);
            if (tag.find("src=") != std::string::npos || tag.find("src =") != std::string::npos) {
                r.warnings.push_back("页面引用了外部脚本（src=...），这部分无法运行。");
                break;
            }
            sp = end;
        }
    }

    // ---- 时长 ----
    double v = 0;
    if (FindNumberAfter(html, "TOTAL_DURATION_SEC", v) && v > 0.2 && v < 3600) {
        r.duration = v;
    } else if (FindNumberAfter(html, "TOTAL_DURATION_MS", v) && v > 200 && v < 3600000) {
        r.duration = v / 1000.0;
    } else if (FindNumberAfter(html, "DURATION_SEC", v) && v > 0.2 && v < 3600) {
        r.duration = v;
    } else if (FindNumberAfter(html, "DURATION_MS", v) && v > 200 && v < 3600000) {
        r.duration = v / 1000.0;
    } else if (FindNumberAfter(html, "DURATION", v) && v > 0.2 && v < 3600) {
        r.duration = v;
    }

    // ---- 帧率（只认显式常量；时间驱动的动画没有固定帧率，不猜）----
    bool fpsFound = false;
    const char* fpsNames[] = { "FPS", "FRAME_RATE", "FRAMES_PER_SECOND", "fps" };
    for (const char* n : fpsNames) {
        if (FindNumberAfter(html, n, v) && v >= 1 && v <= 120) {
            r.fps = (int)(v + 0.5);
            fpsFound = true;
            break;
        }
    }
    r.fpsSource = fpsFound ? "源码常量" : "默认（原动画基于时间，无固定帧率）";

    // ---- 画面变化间隔（提示用）----
    const char* tickNames[] = { "STEP_INTERVAL_MS", "STEP_MS", "TICK_MS", "INTERVAL_MS" };
    for (const char* n : tickNames) {
        if (FindNumberAfter(html, n, v) && v > 0 && v < 60000) {
            r.tickMs = v;
            break;
        }
    }

    // ---- 其他提示 ----
    if (r.canvasCount == 0) r.warnings.push_back("没有 <canvas> 标签，尺寸需要手动填写。");
    {
        std::string lower = util::Lower(html);
        if (lower.find("type=\"module\"") != std::string::npos || lower.find("type='module'") != std::string::npos)
            r.warnings.push_back("含 type=\"module\" 脚本，在本地环境可能无法运行。");
    }

    return r;
}

inline std::string AnalyzeToJson(const std::string& html) {
    Result r = Analyze(html);
    std::string j = "{";
    j += "\"width\":" + std::to_string(r.width);
    j += ",\"height\":" + std::to_string(r.height);
    j += ",\"sizeSource\":\"" + util::JEsc(r.sizeSource) + "\"";
    {
        char b[64];
        sprintf_s(b, ",\"duration\":%.4f", r.duration);
        j += b;
    }
    j += ",\"fps\":" + std::to_string(r.fps);
    j += ",\"fpsSource\":\"" + util::JEsc(r.fpsSource) + "\"";
    if (r.tickMs > 0) {
        char b[128];
        sprintf_s(b, ",\"tick\":{\"ms\":%.4g,\"perSec\":%.4g}", r.tickMs, 1000.0 / r.tickMs);
        j += b;
    } else {
        j += ",\"tick\":null";
    }
    j += ",\"scriptCount\":" + std::to_string(r.scriptCount);
    j += ",\"canvasCount\":" + std::to_string(r.canvasCount);
    j += ",\"warnings\":[";
    for (size_t i = 0; i < r.warnings.size(); i++) {
        if (i) j += ",";
        j += "\"" + util::JEsc(r.warnings[i]) + "\"";
    }
    j += "]}";
    return j;
}

}  // namespace Inspector
