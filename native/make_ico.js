// 把多张 PNG 打包成一个 .ico。
//
// 用 PNG 压缩格式（而非传统 BMP）存每个尺寸：Windows Vista 以后都支持，
// 256x256 这一档尤其需要，因为 BMP 格式在那里体积会爆。
//
// .ico 结构：6 字节文件头 + 每张图 16 字节目录项 + 各图数据（PNG 原始字节）。

const fs = require('fs');
const path = require('path');

const DIR = path.join(__dirname, 'icon_tmp');
const OUT = path.join(__dirname, 'app.ico');

// 尺寸从大到小排列：Windows 会挑最合适的一档，把大的放前面便于它优先读到
const SIZES = [256, 128, 96, 64, 48, 40, 32, 24, 20, 16];

const images = [];
for (const s of SIZES) {
  const p = path.join(DIR, `icon_${s}.png`);
  if (!fs.existsSync(p)) { console.log(`  跳过 ${s}（缺少文件）`); continue; }
  const buf = fs.readFileSync(p);
  // 校验确实是 PNG
  if (buf[0] !== 0x89 || buf.toString('latin1', 1, 4) !== 'PNG') {
    console.error(`  ${s}: 不是 PNG，跳过`);
    continue;
  }
  images.push({ size: s, data: buf });
  console.log(`  ${s}x${s}  ${buf.length} 字节`);
}
if (!images.length) { console.error('没有可用的图片'); process.exit(1); }

const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0);              // reserved
header.writeUInt16LE(1, 2);              // type: 1 = icon
header.writeUInt16LE(images.length, 4);  // 图像数量

const entries = [];
let offset = 6 + images.length * 16;
for (const img of images) {
  const e = Buffer.alloc(16);
  // 宽高：0 表示 256（字段只有 1 字节）
  e.writeUInt8(img.size >= 256 ? 0 : img.size, 0);
  e.writeUInt8(img.size >= 256 ? 0 : img.size, 1);
  e.writeUInt8(0, 2);        // 调色板数量
  e.writeUInt8(0, 3);        // reserved
  e.writeUInt16LE(1, 4);     // 颜色平面
  e.writeUInt16LE(32, 6);    // 位深
  e.writeUInt32LE(img.data.length, 8);
  e.writeUInt32LE(offset, 12);
  entries.push(e);
  offset += img.data.length;
}

const ico = Buffer.concat([header, ...entries, ...images.map(i => i.data)]);
fs.writeFileSync(OUT, ico);
console.log(`\napp.ico: ${images.length} 个尺寸，${(ico.length / 1024).toFixed(1)} KB`);
