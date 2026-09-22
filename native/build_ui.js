// 把 gui.html 转成 gui_html.h。
// 需要分块：MSVC 单个字符串字面量上限 16380 字节，界面 HTML 会超过；
// C++ 会把相邻的字符串字面量自动拼接，结果与单块完全相同。
// 分块按 UTF-8 字节数计算（中文占 3 字节，按字符数算会切错）。
const fs = require('fs');
const path = require('path');

const SRC = path.join(__dirname, 'gui.html');
const DST = path.join(__dirname, 'gui_html.h');
const LIMIT = 8000;   // 保守值，给编译器的转义处理留余量

const html = fs.readFileSync(SRC, 'utf8');
const lines = html.split('\n');
const parts = [];
let cur = '', curBytes = 0;

for (const ln of lines) {
  const piece = ln + '\n';
  const pb = Buffer.byteLength(piece, 'utf8');
  if (curBytes + pb > LIMIT && cur) { parts.push(cur); cur = ''; curBytes = 0; }
  cur += piece;
  curBytes += pb;
}
if (cur) parts.push(cur);

const header = `#pragma once
// 内嵌的前端界面（由 main.cpp 通过 WebView2 请求拦截喂给窗口，不读磁盘、不经过浏览器）。
//
// 界面自己做全部编排：把动画 HTML 注入隐藏 iframe，接管其中的 requestAnimationFrame
// 与 performance.now，按帧推进并用 canvas.toDataURL() 取回浏览器编码好的 PNG，
// 经 fetch('/api/...') 交给 C++ 写盘。于是只需一个 WebView2 实例，
// C++ 侧也不需要 PNG 编码器或图像库——这是 exe 能压到 300KB 以内的关键。
//
// 这个文件由 build_ui.js 从 gui.html 生成，请勿直接编辑；改界面请改 gui.html。
// HTML 超过 MSVC 单个字符串字面量上限（16380 字节），故拆成多块，
// C++ 会把相邻的字符串字面量自动拼接。

static const char* GUI_HTML =
`;

let out = header;
parts.forEach((p, i) => {
  out += '    R"GUI' + i + '(' + p + ')GUI' + i + '"';
  out += (i < parts.length - 1) ? '\n' : ';\n';
});

fs.writeFileSync(DST, out, 'utf8');

const total = Buffer.byteLength(html, 'utf8');
console.log('gui.html -> gui_html.h');
console.log('  HTML: ' + total + ' 字节，切成 ' + parts.length + ' 块');
parts.forEach((p, i) => {
  const b = Buffer.byteLength(p, 'utf8');
  console.log('    P' + i + ': ' + b + ' 字节' + (b > 16380 ? '  !! 超过 MSVC 上限' : ''));
});
if (parts.some(p => Buffer.byteLength(p, 'utf8') > 16380)) {
  console.error('有分块超过 MSVC 上限，请调小 LIMIT');
  process.exit(1);
}
