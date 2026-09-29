// 按需（逐字）加载的 LVGL bin 字体
//
// 背景：LVGL 自带的 lv_font_load() 会把整张字体的**所有**字形位图读进内存，
// 而它的读法极碎 —— 每个字形都要 seek 一次、再把字形表头**逐字节**读出来
// （read_bits() 内部就是 lv_fs_read(...,1,...)），而且整个流程走两遍。
// 一个 4.5 万字形、3.4MB 的 bin 字体因此要发**几十万次** VFS/FATFS/SPI 调用，
// 10MHz 下要几分钟，表现为开机卡死；同时它还会把 3.4MB 常驻 PSRAM。
//
// 本模块换成「按需加载」：
//   · 开机只读 head + cmap + loca 三张索引表（几十~几百 KB，一次大块顺序读，
//     约 0.1 秒），**不读** glyf 里那 3MB 位图；
//   · 渲染到某个字时，才把那个字形的几个字节从 SD 读出来，解码后放进小缓存；
//   · 只实现 LVGL 需要的两个回调（get_glyph_dsc / get_glyph_bitmap），
//     用自己的一级缓存（描述符缓存 + 位图缓存）避免重复读卡。
//
// 内存：索引(约 200KB~400KB) + 缓存(约 50KB)，与字体总大小基本无关。
//
// bin 格式（由 lv_font_conv --format bin 产出，字段布局对照
// lib/lvgl/src/font/lv_font_loader.c 与 lv_font_conv 的 lib/font/table_*.js）：
//     [u32 len]["head"][40B 头] [u32 len]["cmap"][u32 n][n×16B 表][各表数据]
//     [u32 len]["loca"][u32 count][count×offset] [u32 len]["glyf"][字形数据]
//   注意每个 section 的 u32 len 都**包含**它自己那 8 字节的 [len][label] 头。
//
// ⚠ 位序：字形表头与位图是**同一条 MSB-first 的连续位流**
//   （表头 nbits 位之后紧接着就是 box_w*box_h*bpp 位的位图），
//   这也是必须自己写位读取器的原因。

#ifndef LAZY_FONT_H
#define LAZY_FONT_H

#include <stdbool.h>

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 SD 卡上的 bin 字体并构建一个按需加载的 lv_font_t。
   path      : LVGL 风格 / 也可直接用 VFS 路径（内部用 Arduino 的 SD/File 读）
   fallback  : 字体里找不到某个字时回退用的字体（可传 NULL）
   返回 NULL 表示失败（文件不存在 / 格式不符 / 内存不足），调用方应回退内置字体。 */
const lv_font_t *LazyFont_Open(const char *path, const lv_font_t *fallback);

// 关闭并释放（一般不需要调用；重复 Open 会先自动 Close）
void LazyFont_Close(void);

// 统计：描述符缓存未命中次数、位图缓存未命中/命中次数（调试用）
void LazyFont_GetStats(uint32_t *dscMiss, uint32_t *bmpMiss, uint32_t *bmpHit);

// 已占用内存字节数（索引 + 缓存，调试用）
uint32_t LazyFont_GetMemoryUsage(void);

#ifdef __cplusplus
}
#endif

#endif // LAZY_FONT_H
