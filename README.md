# HTML Export

把 HTML 动画导出为 PNG 序列帧或 MP4 视频的 Windows 小工具。

选一个 HTML 文件（点击选择、拖进窗口、或把文件拖到 exe 图标上），工具会自动识别画布尺寸、帧率和时长，然后逐帧渲染导出。适合把 canvas 动画、CSS 动画这类网页动效交给后期合成软件使用。

![界面](docs/screenshot.png)

## 特点

- **单文件 exe，394 KB**：不捆绑浏览器内核，也不依赖 Node.js / Python / .NET
- **渲染结果与浏览器一致**：在真实的 Chromium 引擎（系统自带的 WebView2）里逐帧驱动动画，不是自己实现的近似渲染器
- **逐帧确定**：接管页面里的 `requestAnimationFrame` 与 `performance.now`，帧的推进由程序控制，不受机器快慢影响，同一文件每次导出结果相同
- **导出目录跟随程序位置**：建在 exe 所在目录下，命名 `<文件名>_<时间戳>\`

## 使用

### 直接使用

下载 [最新版](../../releases/latest) 的 `FrameExporter.exe`，双击运行。

把 HTML 文件拖到 exe 图标上可以直接载入；程序已经在运行时拖第二个文件，会转交给已打开的窗口，不会重复启动。

### 导出格式

界面上有两个独立按钮，按需选择：

| 按钮 | 产出 |
|---|---|
| 导出 PNG 序列帧 | `frames\` 目录，命名为 `frame_00001.png` 起 |
| 导出 MP4 | 单个 `.mp4` 文件（H.264，yuv420p，可调帧率） |

导出完成后，日志里会打印完整路径，复制即可。

## 环境要求

- **Windows 10 1809 及以上**（64 位）
- **Microsoft Edge WebView2 Runtime**

WebView2 Runtime 在 Windows 10/11 上通常已随系统或 Office 安装。如果没有，程序启动时会提示，从[微软官网](https://developer.microsoft.com/microsoft-edge/webview2/)免费下载安装即可。

**MP4 导出需要 ffmpeg**：把 `ffmpeg.exe` 放在与 `FrameExporter.exe` 相同的目录，或让它出现在 PATH 里。没有 ffmpeg 时 PNG 序列帧导出不受影响，界面上的 MP4 按钮会自动禁用。

## 从源码构建

需要：

- Visual Studio 2022 生成工具（勾选"使用 C++ 的桌面开发"）
- Node.js（用于把界面 HTML 转成 C++ 头文件）
- PowerShell 与 curl（Windows 自带，用于拉取 WebView2 SDK）

```bat
cd native
setup_sdk.bat        :: 下载 WebView2 SDK 到 sdk\（首次构建需要）
build_native.bat     :: 编译，产物输出到 ..\dist\FrameExporter.exe
```

`build_native.bat` 会自动完成四步：生成界面头文件 → 编译资源（图标与版本信息）→ 编译 → 输出。

### 改界面

界面是普通的 HTML/CSS/JS，源码在 `native\gui.html`。直接编辑它，然后重新跑 `build_native.bat` 即可。

不要直接改 `gui_html.h`——那个文件由 `build_ui.js` 从 `gui.html` 生成，会被覆盖。

## 实现说明

### 为什么用 WebView2

这个工具必须执行 HTML 里的 JavaScript 才能渲染动画。C++ 本身没有 JS 引擎，可选方案有三种：

1. 捆绑一个 JS 引擎（QuickJS 等）——体积增加，且渲染结果与浏览器不一致
2. 捆绑整个浏览器内核（Electron 方案）——体积 100 MB 以上
3. 复用系统自带的 Edge 内核（WebView2）——体积几乎不增加，渲染结果就是浏览器结果

选了第三种。

### PNG 编码交给浏览器

每帧通过 `canvas.toDataURL('image/png')` 取回浏览器已经编码好的 PNG，C++ 侧只做 base64 解码后写盘。这样程序里不需要 PNG 编码器、zlib 或任何图像库——这是能把 exe 压到 394 KB 的关键。

MP4 同理：把 PNG 原样写进 ffmpeg 的 stdin（`-f image2pipe`），由 ffmpeg 解码，C++ 侧不需要图像解码能力。

### 逐帧驱动

界面把动画 HTML 注入一个隐藏的 iframe，并在写入文档**之前**就覆盖掉 `window.requestAnimationFrame` 与 `window.performance.now`：

- `requestAnimationFrame` 的回调被收进队列，不立即执行
- 每帧由程序调用 `__step(毫秒)` 主动触发队列里的回调，并把虚拟时间设成 `帧号 / 帧率 × 1000`

于是动画的时间轴由程序完全控制，与真实渲染耗时无关。这也让导出结果可复现。

### 参数识别

载入文件时扫描 HTML 源码，识别这些常见常量：

| 项目 | 识别的写法 |
|---|---|
| 画布尺寸 | `<canvas width= height=>` 标签 |
| 帧率 | `FPS`、`FRAME_RATE`、`FRAMES_PER_SECOND` |
| 时长 | `TOTAL_DURATION_SEC`、`DURATION_SEC`、`TOTAL_DURATION_MS` 等 |
| 变化间隔 | `STEP_INTERVAL_MS`、`TICK_MS` 等（仅作提示） |

识别不到的项目会给出默认值并在界面上说明，数值都可以手动改。原动画如果是时间驱动（没有固定帧率），帧率按 30 处理。

## 已知限制

- 只支持 2D canvas 动画。用 WebGL 的页面可能无法通过 `toDataURL` 取到内容。
- 画布尺寸极大时（如 4K 以上）内存占用会明显上升。
- 含 `type="module"` 脚本的页面在本地文件环境下可能无法运行。
- 单实例机制靠窗口类名查找，不跨用户会话。

## 目录结构

```
dist\
  FrameExporter.exe        程序本体
native\
  gui.html                 界面源码（改这个）
  gui_html.h               由 gui.html 生成，勿直接编辑
  main.cpp                 窗口、WebView2 宿主、API 处理
  util.h                   字符串/JSON/base64/文件读写
  inspector.h              参数识别
  ffmpeg.h                 ffmpeg 调用封装
  app.rc / app.ico         图标与版本信息
  build_native.bat         一键构建
  build_ui.js              界面 HTML → C++ 头文件
  make_ico.js              PNG → 多尺寸 ico
  setup_sdk.bat            下载 WebView2 SDK
  sdk\                     构建时由 setup_sdk.bat 生成
```

## 许可

本仓库的代码可自由使用。

WebView2 SDK 属于微软，未包含在本仓库中，由 `setup_sdk.bat` 从官方 NuGet 源获取，遵循其各自的许可条款。
