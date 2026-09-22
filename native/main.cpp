// 序列帧导出工具 —— 原生 Windows 窗口程序（C++）
//
// 架构：
//   一个 Win32 窗口 + 一个 WebView2（系统自带 Edge 内核）渲染界面。
//   界面把动画 HTML 注入隐藏 iframe，接管其中的 requestAnimationFrame 与 performance.now，
//   按帧推进并用 canvas.toDataURL('image/png') 取回浏览器编码好的 PNG，
//   经 fetch('/api/...') 交给本进程写盘；勾选 MP4 时把 PNG 流式喂给 ffmpeg。
//
// 为什么这样最省体积：
//   1. 复用系统自带的 WebView2，不捆绑任何 JS 引擎或浏览器内核。
//   2. PNG 编码由浏览器的 canvas 完成，C++ 侧只需 base64 解码，不需要 PNG/zlib 库。
//   3. 界面、样式、逻辑全部内嵌，没有资源文件，静态链接后就是一个独立 exe。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "WebView2.h"
#include "gui_html.h"
#include "util.h"
#include "inspector.h"
#include "ffmpeg.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;
using namespace util;

static const wchar_t* kClass = L"FrameExporterMainWnd";
static const wchar_t* kUrl = L"http://frame-exporter.local/index.html";

static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static std::wstring g_baseDir;
static std::wstring g_pending;
static UINT g_dpi = 96;   // 启动时探测，用于把逻辑像素换算成窗口的物理像素

// 自检模式：FrameExporter.exe --selftest [输出日志路径]
// 依次验证 WebView2 初始化、界面加载、API 往返、以及小规模导出，
// 把每一步结果写进日志。用于确认程序在目标机器上能正常工作。
static bool g_selftest = false;
static std::wstring g_selftestLog;
static std::wstring g_selftestHtml;   // 用真实 HTML 做自检时指定
static int g_selftestFrames = 3;      // 真实模式下导出多少帧
static bool g_selftestMp4 = false;    // 自检时是否一并验证 MP4 编码
static bool g_measureUi = false;      // 只测量界面布局尺寸，不导出
static void SelfLog(const std::string& line) {
    if (g_selftestLog.empty()) return;
    HANDLE h = CreateFileW(g_selftestLog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string s = line + "\r\n";
    DWORD w = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// 导出会话状态（一次导出对应一个会话）
// ---------------------------------------------------------------------------
struct Session {
    bool active = false;
    bool makePng = false;
    bool makeMp4 = false;
    int width = 0;
    int height = 0;
    int fps = 30;
    int frameCount = 0;
    std::wstring outDir;
    std::wstring framesDir;
    std::wstring mp4Path;
    std::string baseName;
    FFmpeg ff;
    bool mp4Open = false;
    int firstUniform = -1;
    std::vector<unsigned char> pendingFrame;   // 最近一帧 PNG 数据
};
static Session g_sess;

static std::string Timestamp() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    char b[32];
    sprintf_s(b, "%04d%02d%02d_%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
              lt.tm_hour, lt.tm_min, lt.tm_sec);
    return b;
}

static std::string SafeBase(const std::string& name) {
    std::string b = name;
    size_t dot = b.find_last_of('.');
    if (dot != std::string::npos && EndsWithNoCase8(b, ".html")) b = b.substr(0, dot);
    else if (dot != std::string::npos && EndsWithNoCase8(b, ".htm")) b = b.substr(0, dot);
    for (char& c : b) if (strchr("\\/:*?\"<>|", c)) c = '_';
    if (b.empty()) b = "animation";
    if (b.size() > 80) b = b.substr(0, 80);
    return b;
}

static void WriteInfoFile() {
    std::wstring p = JoinPath(g_sess.outDir, L"导出说明.txt");
    std::string t = "\xEF\xBB\xBF";   // BOM，方便记事本正确识别 UTF-8
    t += "序列帧导出工具 导出说明\r\n";
    t += "画布尺寸：" + std::to_string(g_sess.width) + " x " + std::to_string(g_sess.height) + "\r\n";
    t += "帧率：" + std::to_string(g_sess.fps) + " fps\r\n";
    t += "帧数：" + std::to_string(g_sess.frameCount) + "\r\n";
    if (g_sess.makePng) t += "序列帧：frames\\ 目录\r\n";
    if (g_sess.makeMp4 && g_sess.mp4Open) t += "视频：" + g_sess.baseName + ".mp4\r\n";
    if (g_sess.firstUniform >= 0) t += "画面填满于第 " + std::to_string(g_sess.firstUniform + 1) + " 帧\r\n";
    WriteFileBytes(p, t.data(), t.size());
}

static void SessionCleanup() {
    if (g_sess.mp4Open) {
        g_sess.ff.Finish();
        g_sess.mp4Open = false;
    }
    g_sess.active = false;
}

// ---------------------------------------------------------------------------
// API 处理
// ---------------------------------------------------------------------------
struct ApiResp {
    int status = 200;
    std::string body = "{}";
    std::string ctype = "application/json; charset=utf-8";
};

static ApiResp ApiState() {
    ApiResp r;
    std::string ver = FFmpeg::VersionString();
    r.body = "{\"toolDir\":\"" + JEsc(W2U8(g_baseDir)) + "\",\"ffmpeg\":{\"ok\":" +
             (ver.empty() ? "false" : "true") + ",\"version\":\"" + JEsc(ver) + "\"}," +
             "\"rendering\":" + (g_sess.active ? "true" : "false") + "}";
    return r;
}

static ApiResp ApiPending() {
    ApiResp r;
    if (g_pending.empty()) {
        if (g_selftest) SelfLog("   /api/pending: no pending file");
        r.body = "{}";
        return r;
    }
    std::wstring p = ToWinPath(g_pending);
    g_pending.clear();
    std::string data, err;
    if (!ReadFileBytes(p, data, &err) || data.empty()) {
        if (g_selftest) SelfLog("   /api/pending: read failed - " + err + " : " + W2U8(p));
        r.body = "{}";
        return r;
    }
    if (g_selftest) {
        char b[256];
        sprintf_s(b, "   /api/pending: served %s (%u bytes)", W2U8(p).c_str(), (unsigned)data.size());
        SelfLog(b);
    }
    std::string name = W2U8(p);
    size_t sl = name.find_last_of("\\/");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    r.body = "{\"file\":{\"name\":\"" + JEsc(name) + "\",\"path\":\"" + JEsc(W2U8(p)) +
             "\",\"text\":\"" + JEsc(data) + "\",\"info\":" + Inspector::AnalyzeToJson(data) + "}}";
    return r;
}

static ApiResp ApiReadFile(const std::string& body) {
    ApiResp r;
    std::string p = JStr(body, "path");
    if (p.empty()) { r.status = 400; r.body = "{\"error\":\"路径为空\"}"; return r; }
    std::string data, err;
    if (!ReadFileBytes(U82W(p), data, &err)) {
        r.status = 400;
        r.body = "{\"error\":\"" + JEsc(err + "：" + p) + "\"}";
        return r;
    }
    std::string name = p;
    size_t sl = name.find_last_of("\\/");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    r.body = "{\"name\":\"" + JEsc(name) + "\",\"text\":\"" + JEsc(data) +
             "\",\"info\":" + Inspector::AnalyzeToJson(data) + "}";
    return r;
}

static ApiResp ApiInspect(const std::string& body) {
    ApiResp r;
    std::string html = JStr(body, "htmlText");
    if (html.empty()) { r.status = 400; r.body = "{\"error\":\"没有收到 HTML 内容\"}"; return r; }
    r.body = Inspector::AnalyzeToJson(html);
    return r;
}

static ApiResp ApiExportBegin(const std::string& body) {
    ApiResp r;
    if (g_sess.active) { r.status = 409; r.body = "{\"error\":\"已有导出任务在进行中。\"}"; return r; }

    g_sess = Session();
    g_sess.baseName = SafeBase(JStr(body, "sourceName", "animation.html"));
    g_sess.makePng = JBool(body, "makePng", true);
    g_sess.makeMp4 = JBool(body, "makeMp4", false);
    g_sess.width = (int)JNum(body, "width", 0);
    g_sess.height = (int)JNum(body, "height", 0);
    g_sess.fps = (int)JNum(body, "fps", 30);
    if (g_sess.fps < 1) g_sess.fps = 1;
    if (g_sess.fps > 120) g_sess.fps = 120;
    if (g_sess.width <= 0 || g_sess.height <= 0) {
        r.status = 400;
        r.body = "{\"error\":\"画布尺寸无效\"}";
        return r;
    }

    g_sess.outDir = JoinPath(g_baseDir, U82W(g_sess.baseName + "_" + Timestamp()));
    g_sess.framesDir = JoinPath(g_sess.outDir, L"frames");
    CreateDirectoryW(g_sess.outDir.c_str(), nullptr);
    if (g_sess.makePng) CreateDirectoryW(g_sess.framesDir.c_str(), nullptr);

    g_sess.active = true;
    g_sess.frameCount = 0;
    g_sess.firstUniform = -1;

    if (g_sess.makeMp4) {
        if (g_sess.ff.Available()) {
            g_sess.mp4Path = JoinPath(g_sess.outDir, U82W(g_sess.baseName + ".mp4"));
            g_sess.mp4Open = g_sess.ff.StartPipe(g_sess.mp4Path, g_sess.width, g_sess.height, g_sess.fps);
        }
    }

    r.body = "{\"outDir\":\"" + JEsc(W2U8(g_sess.outDir)) + "\",\"mp4\":" +
             (g_sess.mp4Open ? "true" : "false") + "}";
    return r;
}

static ApiResp ApiExportFrame(const std::string& body) {
    ApiResp r;
    if (!g_sess.active) { r.status = 409; r.body = "{\"error\":\"没有进行中的导出任务\"}"; return r; }

    int index = (int)JNum(body, "index", 0);
    std::string b64 = JStr(body, "data");
    if (b64.empty()) { r.status = 400; r.body = "{\"error\":\"帧数据为空\"}"; return r; }

    std::vector<unsigned char> png = Base64Decode(b64);
    if (png.empty()) { r.status = 400; r.body = "{\"error\":\"帧数据解码失败\"}"; return r; }

    if (g_sess.makePng) {
        wchar_t fn[32];
        swprintf_s(fn, L"frame_%05d.png", index);
        if (!WriteFileBytes(JoinPath(g_sess.framesDir, fn), png.data(), png.size())) {
            r.status = 500;
            r.body = "{\"error\":\"写 PNG 失败\"}";
            return r;
        }
    }

    // MP4 需要原始像素：把刚拿到的 PNG 交给 ffmpeg（它自己会解码）
    if (g_sess.mp4Open) g_sess.ff.WritePngFrame(png.data(), png.size());

    g_sess.frameCount = index;
    r.body = "{\"ok\":true}";
    return r;
}

static ApiResp ApiExportEnd() {
    ApiResp r;
    if (!g_sess.active) { r.status = 409; r.body = "{\"error\":\"没有进行中的导出任务\"}"; return r; }

    if (g_sess.mp4Open) {
        g_sess.ff.Finish();
        g_sess.mp4Open = false;
    }
    WriteInfoFile();

    std::string mp4u8 = (g_sess.makeMp4 && !g_sess.mp4Path.empty()) ? W2U8(g_sess.mp4Path) : "";
    std::string out = W2U8(g_sess.outDir);
    int n = g_sess.frameCount;
    g_sess.active = false;

    r.body = "{\"outDir\":\"" + JEsc(out) + "\",\"frameCount\":" + std::to_string(n) +
             ",\"mp4\":\"" + JEsc(mp4u8) + "\"}";
    return r;
}

static ApiResp ApiExportAbort() {
    ApiResp r;
    SessionCleanup();
    r.body = "{\"ok\":true}";
    return r;
}

static ApiResp ApiReveal(const std::string& body) {
    ApiResp r;
    std::string p = JStr(body, "path");
    if (!p.empty()) {
        std::wstring wp = ToWinPath(U82W(p));
        if (GetFileAttributesW(wp.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ShellExecuteW(nullptr, L"open", wp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    r.body = "{\"ok\":true}";
    return r;
}

static ApiResp Dispatch(const std::string& path, const std::string& method, const std::string& body) {
    if (path == "/api/state") return ApiState();
    if (path == "/api/pending") return ApiPending();
    if (path == "/api/readfile") return ApiReadFile(body);
    if (path == "/api/inspect") return ApiInspect(body);
    if (path == "/api/export/begin") return ApiExportBegin(body);
    if (path == "/api/export/frame") return ApiExportFrame(body);
    if (path == "/api/export/end") return ApiExportEnd();
    if (path == "/api/export/abort") return ApiExportAbort();
    if (path == "/api/measure" && method == "POST") {
        // 布局测量结果：原样写进日志，供开发时核对紧凑程度
        SelfLog("3) layout measurements (CSS px):");
        SelfLog("   " + body);
        ApiResp r;
        r.body = "{\"ok\":true}";
        if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return r;
    }

    if (path == "/api/reveal") return ApiReveal(body);
    if (path == "/api/selftest") {
        // 界面把自检结果回传到这里，写进日志文件后退出
        std::string result = JStr(body, "result");
        std::string outDir = JStr(body, "outDir");
        SelfLog("3) GUI selftest:");
        {
            size_t p = 0;
            while (p <= result.size()) {
                size_t nl = result.find('\n', p);
                std::string line = (nl == std::string::npos) ? result.substr(p) : result.substr(p, nl - p);
                if (!line.empty()) SelfLog("   " + line);
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
        }
        if (!outDir.empty()) {
            SelfLog("4) verifying files on disk:");
            std::wstring fd = JoinPath(U82W(outDir), L"frames");
            wchar_t pat[MAX_PATH];
            swprintf_s(pat, L"%s\\frame_*.png", fd.c_str());
            WIN32_FIND_DATAW fdata;
            HANDLE hf = FindFirstFileW(pat, &fdata);
            int n = 0;
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    n++;
                    if (n <= 3) {
                        char b[512];
                        sprintf_s(b, "   %s  %u bytes", W2U8(fdata.cFileName).c_str(), fdata.nFileSizeLow);
                        SelfLog(b);
                    }
                } while (FindNextFileW(hf, &fdata));
                FindClose(hf);
            }
            char b[128];
            sprintf_s(b, "   total %d frame files", n);
            SelfLog(b);
            SelfLog(n > 0 ? "SELFTEST OK" : "SELFTEST FAILED (no frames written)");
        }
        ApiResp r;
        r.body = "{\"ok\":true}";
        if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return r;
    }
    if (path == "/api/quit") {
        ApiResp r;
        r.body = "{\"ok\":true}";
        if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return r;
    }
    ApiResp r;
    r.status = 404;
    r.body = "{\"error\":\"未知接口\"}";
    return r;
}

// ---------------------------------------------------------------------------
// WebView2 请求拦截：界面本体从内存喂，/api/ 就地处理，其余一律拒绝（本程序不联网）
// ---------------------------------------------------------------------------
class RequestHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                           ICoreWebView2WebResourceRequestedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebResourceRequestedEventArgs* args) override {
        ComPtr<ICoreWebView2WebResourceRequest> req;
        if (FAILED(args->get_Request(&req)) || !req) return S_OK;

        LPWSTR uriRaw = nullptr;
        req->get_Uri(&uriRaw);
        std::wstring uri = uriRaw ? uriRaw : L"";
        if (uriRaw) CoTaskMemFree(uriRaw);

        // 取 Environment 需要 ICoreWebView2_2（基础接口上没有这个方法）
        ComPtr<ICoreWebView2_2> wv2;
        ComPtr<ICoreWebView2Environment> env;
        if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&wv2))) && wv2) {
            wv2->get_Environment(&env);
        }
        if (!env) return S_OK;

        auto respond = [&](const std::string& data, int status, const wchar_t* ctype) {
            IStream* s = SHCreateMemStream((const BYTE*)data.data(), (UINT)data.size());
            ComPtr<ICoreWebView2WebResourceResponse> resp;
            env->CreateWebResourceResponse(s, status, L"OK", ctype, &resp);
            args->put_Response(resp.Get());
            if (s) s->Release();
        };

        // 界面本体
        if (uri.find(L"/api/") == std::wstring::npos) {
            respond(std::string(GUI_HTML), 200, L"Content-Type: text/html; charset=utf-8");
            return S_OK;
        }

        // API
        size_t p = uri.find(L"/api/");
        std::string apiPath = W2U8(uri.substr(p));

        LPWSTR mRaw = nullptr;
        req->get_Method(&mRaw);
        std::string method = mRaw ? W2U8(mRaw) : "GET";
        if (mRaw) CoTaskMemFree(mRaw);

        std::string bodyText;
        ComPtr<IStream> content;
        if (SUCCEEDED(req->get_Content(&content)) && content) {
            char buf[8192];
            ULONG read = 0;
            while (SUCCEEDED(content->Read(buf, sizeof(buf), &read)) && read > 0) {
                bodyText.append(buf, read);
                if (bodyText.size() > 64u * 1024 * 1024) break;
            }
        }

        ApiResp ar;
        try {
            if (g_selftest) SelfLog("   api call: " + apiPath + " [" + method + "]");
            ar = Dispatch(apiPath, method, bodyText);
        } catch (const std::exception& e) {
            ar.status = 500;
            ar.body = "{\"error\":\"" + JEsc(e.what()) + "\"}";
        } catch (...) {
            ar.status = 500;
            ar.body = "{\"error\":\"内部错误\"}";
        }
        respond(ar.body, ar.status, U8W(ar.ctype).c_str());
        return S_OK;
    }

private:
    static std::wstring U8W(const std::string& s) { return util::U82W(s); }
};

class EnvDone : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override;
};

class CtrlDone : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                     ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* c) override;
};

HRESULT EnvDone::Invoke(HRESULT hr, ICoreWebView2Environment* env) {
    if (FAILED(hr) || !env) {
        if (g_selftest) {
            char b[128];
            sprintf_s(b, "   WebView2 init FAILED hr=0x%08X", (unsigned)hr);
            SelfLog(b);
            SelfLog("SELFTEST FAILED (no WebView2 Runtime)");
            if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            return hr;
        }
        MessageBoxW(g_hwnd,
            L"无法初始化 WebView2 组件。\n\n"
            L"本程序需要 Microsoft Edge WebView2 Runtime（Windows 10/11 通常已自带）。\n"
            L"可从微软官网免费下载安装后再运行。",
            L"序列帧导出", MB_ICONERROR);
        return hr;
    }
    if (g_selftest) SelfLog("   WebView2 environment OK");
    env->CreateCoreWebView2Controller(g_hwnd, Make<CtrlDone>().Get());
    return S_OK;
}

HRESULT CtrlDone::Invoke(HRESULT hr, ICoreWebView2Controller* c) {
    if (FAILED(hr) || !c) return hr;
    g_controller = c;
    c->get_CoreWebView2(&g_webview);

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    c->put_Bounds(rc);
    c->put_IsVisible(TRUE);

    ComPtr<ICoreWebView2Settings> s;
    if (SUCCEEDED(g_webview->get_Settings(&s)) && s) {
        s->put_AreDefaultContextMenusEnabled(FALSE);
        s->put_IsStatusBarEnabled(FALSE);
        s->put_AreDevToolsEnabled(FALSE);
        s->put_IsZoomControlEnabled(FALSE);
        s->put_IsScriptEnabled(TRUE);
    }

    EventRegistrationToken tok;
    g_webview->add_WebResourceRequested(Make<RequestHandler>().Get(), &tok);
    g_webview->AddWebResourceRequestedFilter(L"http://frame-exporter.local/*",
                                             COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

    // 自检：界面加载完成后，让页面自己跑一次导出并回报结果
    if (g_selftest) {
        if (g_measureUi) {
            // 布局测量模式：读出界面上各元素的真实盒模型尺寸，写进日志。
            // 这样不必截图就能确认紧凑程度（也避免受其他窗口遮挡影响）。
            class NavDoneMeasure : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                                       ICoreWebView2NavigationCompletedEventHandler> {
            public:
                HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* wv, ICoreWebView2NavigationCompletedEventArgs*) override {
                    SelfLog("2) GUI page loaded (layout measure mode)");
                    const wchar_t* js =
                        L"(function(){"
                        // 先让参数区显示出来，否则量到的都是隐藏元素
                        L"var meta=document.getElementById('meta');if(meta)meta.classList.add('on');"
                        L"var fn=document.getElementById('fn');if(fn)fn.innerHTML='<b>已载入</b> snake-shortest-path.html';"
                        L"var log=document.getElementById('log');if(log){log.classList.add('on');"
                        L"log.textContent='导出目录 C:\\\\dist\\\\snake_20260921_175348\\\\n完成：C:\\\\dist\\\\snake_20260921_175348';}"
                        L"var bar=document.getElementById('bar');if(bar)bar.classList.add('on');"
                        L"var st=document.getElementById('st');if(st)st.classList.add('on');"
                        L"var ph=document.getElementById('ph');if(ph)ph.textContent='完成';"
                        L"var ct=document.getElementById('ct');if(ct)ct.innerHTML='<b>2100</b> 帧';"
                        L"function box(sel){var e=document.querySelector(sel);if(!e)return null;"
                        L"var r=e.getBoundingClientRect();var cs=getComputedStyle(e);"
                        L"return {h:Math.round(r.height),w:Math.round(r.width),"
                        L"mt:Math.round(parseFloat(cs.marginTop)||0),mb:Math.round(parseFloat(cs.marginBottom)||0),"
                        L"pt:Math.round(parseFloat(cs.paddingTop)||0),pb:Math.round(parseFloat(cs.paddingBottom)||0)};}"
                        L"var out={};"
                        L"['body','.card','.drop','.pr','#meta','#fn','.pg','#vsize','#vfps','#vdur','#vfr',"
                        L"'#warn','.out','.acts','#bPng','#bar','#st','#log'].forEach(function(s){out[s]=box(s)});"
                        L"out['docH']=Math.round(document.documentElement.scrollHeight);"
                        L"fetch('/api/measure',{method:'POST',headers:{'Content-Type':'application/json'},"
                        L"body:JSON.stringify(out)});"
                        L"return 'ok';})()";
                    wv->ExecuteScript(js, nullptr);
                    return S_OK;
                }
            };
            g_webview->add_NavigationCompleted(Make<NavDoneMeasure>().Get(), &tok);
            g_webview->Navigate(kUrl);
            return S_OK;
        }
        if (!g_selftestHtml.empty()) {
            // 真实文件模式：先把文件放进 pending，界面会自动载入
            g_pending = g_selftestHtml;
            class NavDoneReal : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                                    ICoreWebView2NavigationCompletedEventHandler> {
            public:
                HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* wv, ICoreWebView2NavigationCompletedEventArgs*) override {
                    SelfLog("2) GUI page loaded (real-file mode)");
                    // 等界面取走 pending 并完成识别，再启动导出
                    const wchar_t* pre = g_selftestMp4 ? L"window.__selftestMp4=true;" : L"";
                    wv->ExecuteScript(pre, nullptr);
                    wv->ExecuteScript(L"setTimeout(function(){window.__selftestReal()},600)", nullptr);
                    return S_OK;
                }
            };
            g_webview->add_NavigationCompleted(Make<NavDoneReal>().Get(), &tok);
        } else {
            class NavDoneSelf : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                                                    ICoreWebView2NavigationCompletedEventHandler> {
            public:
                HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* wv, ICoreWebView2NavigationCompletedEventArgs*) override {
                    SelfLog("2) GUI page loaded");
                    // 构造一个 8x8 的 canvas 动画，导出 3 帧 PNG，验证整条链路
                    const wchar_t* js =
                        L"(function(){"
                        L"var html='<!DOCTYPE html><canvas id=c width=8 height=8></canvas><script>"
                        L"var g=document.getElementById(\"c\").getContext(\"2d\");var n=0;"
                        L"function f(){n++;g.fillStyle=n%2?\"#ff0000\":\"#00ff00\";g.fillRect(0,0,8,8);requestAnimationFrame(f);}"
                        L"requestAnimationFrame(f);<\\/script>';"
                        L"window.__smokeHtml=html;"
                        L"return window.__selftestSmoke();"
                        L"})()";
                    wv->ExecuteScript(js, nullptr);
                    return S_OK;
                }
            };
            g_webview->add_NavigationCompleted(Make<NavDoneSelf>().Get(), &tok);
        }
    }

    g_webview->Navigate(kUrl);
    return S_OK;
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (g_controller) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_controller->put_Bounds(rc);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* m = (MINMAXINFO*)lp;
            m->ptMinTrackSize.x = MulDiv(400, g_dpi, 96);
            m->ptMinTrackSize.y = MulDiv(340, g_dpi, 96);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP d = (HDROP)wp;
            wchar_t path[MAX_PATH] = {0};
            if (DragQueryFileW(d, 0, path, MAX_PATH)) {
                std::wstring p = path;
                if (EndsWithNoCase(p, L".html") || EndsWithNoCase(p, L".htm")) {
                    g_pending = p;
                    if (g_webview) g_webview->Reload();
                }
            }
            DragFinish(d);
            return 0;
        }

        case WM_COPYDATA: {
            // 第二个实例把文件转交过来
            COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lp;
            if (cds && cds->dwData == 1 && cds->lpData) {
                g_pending = (const wchar_t*)cds->lpData;
                if (g_webview) g_webview->Reload();
            }
            return TRUE;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            SessionCleanup();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // 必须在创建任何窗口之前声明 DPI 感知，否则系统会把窗口当位图拉伸，
    // 在高缩放显示器上界面会又大又糊。
    // 优先用 Per-Monitor V2（Win10 1703+），不支持时退回 System DPI 感知。
    {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        bool done = false;
        if (user32) {
            auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setCtx) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
                done = setCtx((HANDLE)-4) != FALSE;
            }
            if (!done) {
                typedef BOOL (WINAPI *SetAwareFn)(void);
                auto setAware = (SetAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
                if (setAware) done = setAware() != FALSE;
            }
        }
    }

    g_baseDir = ExeDir();

    // 命令行参数：--selftest [日志路径]，以及拖到 exe 图标上的 HTML
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; i++) {
                std::wstring a = argv[i];
                if (a == L"--selftest") {
                    g_selftest = true;
                    if (i + 1 < argc) {
                        std::wstring nx = argv[i + 1];
                        if (!nx.empty() && nx[0] != L'-') { g_selftestLog = nx; i++; }
                    }
                    if (g_selftestLog.empty()) {
                        wchar_t tmp[MAX_PATH] = {0};
                        GetTempPathW(MAX_PATH, tmp);
                        g_selftestLog = JoinPath(tmp, L"frame-exporter-selftest.log");
                    }
                } else if (a == L"--html") {
                    if (i + 1 < argc) { g_selftestHtml = argv[i + 1]; i++; }
                } else if (a == L"--frames") {
                    if (i + 1 < argc) { g_selftestFrames = _wtoi(argv[i + 1]); i++; }
                } else if (a == L"--mp4") {
                    g_selftestMp4 = true;
                } else if (a == L"--measure-ui") {
                    g_selftest = true;
                    g_measureUi = true;
                    // 可选：紧跟一个日志路径
                    if (i + 1 < argc) {
                        std::wstring nx = argv[i + 1];
                        if (!nx.empty() && nx[0] != L'-' && !EndsWithNoCase(nx, L".html")) {
                            g_selftestLog = nx;
                            i++;
                        }
                    }
                    if (g_selftestLog.empty()) {
                        wchar_t tmp[MAX_PATH] = {0};
                        GetTempPathW(MAX_PATH, tmp);
                        g_selftestLog = JoinPath(tmp, L"frame-exporter-measure.log");
                    }
                } else if (EndsWithNoCase(a, L".html") || EndsWithNoCase(a, L".htm")) {
                    g_pending = a;
                }
            }
            LocalFree(argv);
        }
    }
    if (g_selftest) {
        DeleteFileW(g_selftestLog.c_str());
        SelfLog("1) FrameExporter selftest");
        SelfLog("   exe dir: " + W2U8(g_baseDir));
        std::string fv = FFmpeg::VersionString();
        SelfLog("   ffmpeg: " + (fv.empty() ? std::string("not found (MP4 disabled)") : fv));
    }

    // 单实例：已在运行则把文件转交给它，不再开新窗口。
    // 自检模式跳过这个检查——它需要自己独立跑一遍完整流程。
    HANDLE mtx = nullptr;
    if (!g_selftest) {
        mtx = CreateMutexW(nullptr, TRUE, L"FrameExporter_SingleInstance_v2");
        bool already = mtx && GetLastError() == ERROR_ALREADY_EXISTS;
        if (already) {
            HWND prev = FindWindowW(kClass, nullptr);
            if (prev) {
                if (!g_pending.empty()) {
                    COPYDATASTRUCT cds;
                    cds.dwData = 1;
                    cds.cbData = (DWORD)((g_pending.size() + 1) * sizeof(wchar_t));
                    cds.lpData = (PVOID)g_pending.c_str();
                    SendMessageW(prev, WM_COPYDATA, 0, (LPARAM)&cds);
                }
                SetForegroundWindow(prev);
            }
            if (mtx) CloseHandle(mtx);
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(248, 250, 252));   // 与界面底色一致，避免加载时闪黑
    wc.lpszClassName = kClass;
    // 图标来自 app.rc 里的 MAINICON（多尺寸 ico）。
    // 大图标取 32x32、小图标取 16x16，让任务栏和标题栏各自清晰。
    wc.hIcon = (HICON)LoadImageW(hInst, L"MAINICON", IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, L"MAINICON", IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // 窗口尺寸按 DPI 缩放：界面是 CSS 像素（逻辑像素），窗口是物理像素。
    // 不缩放的话，在 150% 缩放的显示器上窗口会显得过小、内容被挤扁。
    {
        HDC screen = GetDC(nullptr);
        if (screen) { g_dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen); }
        if (g_dpi < 96) g_dpi = 96;
    }
    // 尺寸按实测内容高度设定：卡片约 350 CSS px + 边距，留出标题栏与少量余量。
    // 有日志时内容会变高，所以给足余量并允许用户拉大。
    int winW = MulDiv(430, g_dpi, 96);
    int winH = MulDiv(452, g_dpi, 96);

    g_hwnd = CreateWindowExW(0, kClass, L"序列帧导出", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // 测量布局时需要窗口可见，否则 WebView2 不渲染、量到的尺寸全是 0
    ShowWindow(g_hwnd, (g_selftest && !g_measureUi) ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_hwnd);
    DragAcceptFiles(g_hwnd, TRUE);

    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring userData = JoinPath(tmp, L"frame-exporter-webview2");
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr, Make<EnvDone>().Get());

    MSG msg;
    // 测量模式只需几秒就能拿到结果，给它更短的超时
    DWORD deadline = g_measureUi ? GetTickCount() + 15000
                   : (g_selftest ? GetTickCount() + 90000 : 0);
    while (true) {
        BOOL got = FALSE;
        if (g_selftest) {
            // 自检模式带超时，避免异常时挂死
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                got = TRUE;
            }
            if ((int)(deadline - GetTickCount()) <= 0) {
                SelfLog("SELFTEST TIMEOUT");
                break;
            }
            if (!got) Sleep(5);
            if (g_selftest && !IsWindow(g_hwnd)) break;
        } else {
            if (!GetMessageW(&msg, nullptr, 0, 0)) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_webview.Reset();
    g_controller.Reset();
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
