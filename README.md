# HTML Export

把 HTML 动画导出为 PNG 序列帧或 MP4 的 Windows 小工具。选一个 HTML 文件，自动识别尺寸、帧率和时长，逐帧渲染导出。

![界面](docs/screenshot.png)

## 下载使用

从 [Releases](../../releases/latest) 下载 `FrameExporter.exe`，双击运行。

把 HTML 文件拖到 exe 图标上可直接载入；程序已在运行时拖第二个文件会转交给已开窗口。

导出目录建在 exe 所在目录下，命名 `<文件名>_<时间戳>\`。两个按钮各管一种输出：PNG 序列帧（`frames\` 目录）或 MP4。

## 环境要求

- Windows 10 1809+（64 位）
- Microsoft Edge WebView2 Runtime（Win10/11 通常已自带）
- MP4 需要 ffmpeg：放在 exe 同目录或 PATH 里。缺省时 PNG 导出不受影响

## 从源码构建

需要 VS2022 生成工具（含"使用 C++ 的桌面开发"）和 Node.js。

```bat
cd native
setup_sdk.bat      :: 下载 WebView2 SDK（首次构建需要）
build_native.bat   :: 编译到 ..\dist\FrameExporter.exe
```

改界面编辑 `native\gui.html` 后重新构建即可；`gui_html.h` 由脚本生成，勿直接改。

## 实现要点

- **复用系统 Edge 内核（WebView2）**执行页面脚本并渲染，不捆绑浏览器内核或 JS 引擎，exe 仅 396 KB
- **PNG 编码交给浏览器**：每帧取 `canvas.toDataURL('image/png')`，C++ 只做 base64 解码写盘，程序内无图像库
- **逐帧确定**：注入脚本接管 `requestAnimationFrame` 与 `performance.now`，帧推进由程序控制，同一文件每次导出结果相同
- 载入时扫描源码识别 `TOTAL_DURATION_SEC`、`FPS` 等常量，识别不到的项给默认值且可手改

## 已知限制

- 仅支持 2D canvas；WebGL 页面可能取不到内容
- 含 `type="module"` 脚本的页面在本地文件环境下可能无法运行

## 许可

代码可自由使用。WebView2 SDK 属微软所有，未随仓库分发，由 `setup_sdk.bat` 从官方 NuGet 源获取。
