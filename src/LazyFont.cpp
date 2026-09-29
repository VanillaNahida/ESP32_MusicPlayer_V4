// 按需（逐字）加载的 LVGL bin 字体 —— 实现，设计说明见 include/LazyFont.h
//
// 目录：
//   1) 常量与数据结构
//   2) 内存 / 位读取 / 查找 小工具
//   3) 读卡层（含 SPI 总线锁与断卡重试）
//   4) 索引解析（head / cmap / loca）
//   5) 两个 LVGL 回调（get_glyph_dsc / get_bitmap）
//   6) 对外接口
//
// 所有与 LVGL 语义相关的细节（adv_w 的 4 位小数、cmap 四种格式的查表、
// ofs_y 的含义）都严格对照：
//   lib/lvgl/src/font/lv_font_loader.c   （bin 的读法）
//   lib/lvgl/src/font/lv_font_fmt_txt.c  （cmap 查找与 dsc 填充）

#include "LazyFont.h"

/* ── 编译期开关（见 platformio.ini 的 ENABLE_EXTERNAL_FONT）──
   只有打开时才编译本模块。关闭（默认）时整个文件编译为空：
   固件里没有任何按需读卡代码，也不占 Flash。 */
#ifndef ENABLE_EXTERNAL_FONT
#define ENABLE_EXTERNAL_FONT 0
#endif

#if ENABLE_EXTERNAL_FONT

#include <Arduino.h>
#include <SD.h>

#include "AudioBusLock.h"
#include "esp_heap_caps.h"

/* ==================== 1) 常量与数据结构 ==================== */

// 描述符缓存（一级直接映射）：键是字形 id。取模用位与，必须是 2 的幂。
// 为什么给这么大：LVGL 每次布局/绘制都会问每个字的 dsc，未命中就要读卡，
// 而在渲染路径里读 SD 必然掉帧。4096 项 × 12B = 48KB PSRAM，
// 足够装下任何一屏文本的工作集。
#define DSC_CACHE_N 4096u
#define DSC_CACHE_MASK (DSC_CACHE_N - 1u)

/* 位图缓存（一级直接映射）。
   ⚠ 这个值直接决定「读外部字库后 UI 卡不卡」：
   播放列表一屏大约有 200~300 个不同汉字。缓存若小于工作集，直接映射会
   每帧把它们互相驱逐 —— 结果是**每帧、每个字都去读一次 SD**，界面必然卡。
   1024 项（元数据 12KB + 位图约 100KB，都在 PSRAM）足以容纳一屏文本加滚动
   余量，稳定后基本零读卡。 */
#define BMP_CACHE_N 1024u
#define BMP_CACHE_MASK (BMP_CACHE_N - 1u)

// 单个字形位图的字节上限（12px/4bpp 的中文字形约 80 字节；给足余量）
#define LAZY_MAX_BMP 8192u

// 断卡后重试打开文件的最小间隔
#define REOPEN_RETRY_MS 2000u

// LVGL 的 cmap 类型（顺序与 lv_font_fmt_txt.h 的匿名枚举一致）
#define CMAP_FORMAT0_FULL 0
#define CMAP_SPARSE_FULL 1
#define CMAP_FORMAT0_TINY 2
#define CMAP_SPARSE_TINY 3

typedef struct
{
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t *unicode_list;      // 稀疏表用（PSRAM）
    void *glyph_id_ofs_list;     // FULL 表用（PSRAM；FORMAT0 是 uint8，SPARSE 是 uint16）
    uint16_t list_length;
    uint8_t type;
} LazyCmap;

typedef struct
{
    // 来自 head
    uint8_t bpp;
    uint8_t xy_bits;
    uint8_t wh_bits;
    uint8_t adv_w_bits;
    uint8_t adv_w_format; // 0: 存的值要 *16
    uint8_t index_to_loc;
    uint16_t default_adv_w;
    uint16_t nbits;        // 字形表头位数
    uint16_t header_bytes; // ceil(nbits/8)

    // 段偏移（都是文件绝对偏移）
    uint32_t glyf_start;
    uint32_t glyf_len;

    // loca
    uint32_t loca_count;
    uint32_t *offsets; // PSRAM，loca_count 个

    // cmap
    uint16_t cmap_num;
    LazyCmap *cmaps; // PSRAM

    // 文件
    /* ⚠ 这里存的是**相对挂载点**的路径（如 /fonts/x.bin），直接交给 SD.open()。
       Arduino 的 VFSImpl::open() 内部会 snprintf("%s%s", _mountpoint, fpath)，
       也就是自己把 "/sd" 补在前面。这里若再拼一次挂载点，实际就会去找
       /sd/sd/fonts/x.bin —— 表现就是「文件明明在卡里却打不开」。 */
    char sd_path[96];
    File file;
    bool open;
    uint32_t nextOpenTryMs;

    // 统计
    uint32_t memBytes;
} LazyFontData;

typedef struct
{
    uint32_t gid; // 0 = 空槽（字形 id 0 是 LVGL 保留的“未找到”）
    uint16_t adv_w; // 原始值（1/16 px）
    uint16_t box_w;
    uint16_t box_h;
    int16_t ofs_x;
    int16_t ofs_y;
} DscCacheEnt;

typedef struct
{
    uint32_t gid;
    uint8_t *bmp;
    uint16_t len;
    uint16_t cap;
} BmpCacheEnt;

static LazyFontData s_lf;
static lv_font_t s_font;
static bool s_ready = false;

/* 缓存放在 PSRAM（刻意不用静态数组）：本项目内部 RAM 极其紧张 ——
   内部堆一旦只剩几 KB，newlib 的 fopen() 就分不到 FILE 结构体而 abort()
   （见 main.cpp / lv_conf.h 里的血泪说明）。24KB 的静态描述符缓存足以
   把内部堆压到危险区，所以这里改成按需从 PSRAM 分配。 */
static DscCacheEnt *s_dscCache = nullptr;
static BmpCacheEnt *s_bmpCache = nullptr;

static uint32_t s_dscMiss = 0, s_bmpMiss = 0, s_bmpHit = 0;

// 解码字形时的可复用缓冲（读整个字形用）
static uint8_t *s_glyphBuf = nullptr;
static uint32_t s_glyphBufCap = 0;

/* ==================== 2) 小工具 ==================== */

static void *psramAlloc(uint32_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == nullptr)
    {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

/* MSB-first 位读取器。字形表头与位图是同一条连续位流，必须按位取。 */
typedef struct
{
    const uint8_t *p;
    uint32_t bitpos;
} BitRd;

static uint32_t brRead(BitRd *b, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t byte = b->p[b->bitpos >> 3];
        uint32_t bit = (byte >> (7u - (b->bitpos & 7u))) & 1u;
        v = (v << 1) | bit;
        b->bitpos++;
    }
    return v;
}

static int32_t brReadSigned(BitRd *b, uint32_t n)
{
    if (n == 0)
    {
        return 0;
    }
    uint32_t v = brRead(b, n);
    if (v & (1u << (n - 1u)))
    {
        v |= ~0u << n; // 符号扩展（与 lv_font_loader 的 read_bits_signed 一致）
    }
    return (int32_t)v;
}

// 在升序的 uint16 数组里二分查找，返回下标或 -1（等价 _lv_utils_bsearch 的用途）
static int bsearchU16(const uint16_t *arr, uint16_t n, uint16_t key)
{
    int lo = 0;
    int hi = (int)n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        uint16_t v = arr[mid];
        if (v == key)
        {
            return mid;
        }
        if (v < key)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return -1;
}

static uint16_t rdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t rdI16(const uint8_t *p) { return (int16_t)rdU16(p); }
static uint32_t rdU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ==================== 3) 读卡层 ==================== */

// 打开文件（在锁外调用；内部加锁）
static bool lfOpenFile(void)
{
    if (s_lf.open)
    {
        return true;
    }
    uint32_t now = millis();
    if (s_lf.nextOpenTryMs != 0 && (int32_t)(now - s_lf.nextOpenTryMs) < 0)
    {
        return false; // 还没到重试时间（插拔卡时避免每次渲染都去试）
    }

    AudioBusLock();
    s_lf.file = SD.open(s_lf.sd_path, FILE_READ);
    bool ok = (bool)s_lf.file;
    AudioBusUnlock();

    if (ok)
    {
        s_lf.open = true;
        s_lf.nextOpenTryMs = 0;
    }
    else
    {
        s_lf.nextOpenTryMs = now + REOPEN_RETRY_MS;
    }
    return ok;
}

static void lfCloseFileLocked(void)
{
    if (s_lf.open)
    {
        AudioBusLock();
        s_lf.file.close();
        AudioBusUnlock();
        s_lf.open = false;
    }
}

/* 从文件中读 len 字节（持 SPI 总线锁）。失败会关闭句柄，下次自动重开。 */
static bool lfRead(uint32_t off, void *buf, uint32_t len)
{
    if (len == 0)
    {
        return true;
    }
    if (!lfOpenFile())
    {
        return false;
    }

    bool ok = false;
    AudioBusLock();
    if (s_lf.open && s_lf.file.seek(off))
    {
        ok = (s_lf.file.read((uint8_t *)buf, len) == len);
    }
    AudioBusUnlock();

    if (!ok)
    {
        /* 读失败基本就是卡被拔了：把句柄关掉，交给 lfOpenFile 按节流重试。
           注意不能在持锁时 close（close 会走 FAT），所以放在解锁之后。 */
        lfCloseFileLocked();
        s_lf.nextOpenTryMs = millis() + REOPEN_RETRY_MS;
    }
    return ok;
}

/* 初始化时可一次性读多段：调用方已知要读哪几段，这里把锁的开销摊薄。
   实现上用 lfRead 即可（每次一小段），init 阶段总计约 200KB。 */

/* ==================== 4) 索引解析 ==================== */

static void lfFreeAll(void)
{
    if (s_bmpCache != nullptr)
    {
        for (uint32_t i = 0; i < BMP_CACHE_N; i++)
        {
            if (s_bmpCache[i].bmp != nullptr)
            {
                heap_caps_free(s_bmpCache[i].bmp);
            }
        }
        heap_caps_free(s_bmpCache);
        s_bmpCache = nullptr;
    }
    if (s_dscCache != nullptr)
    {
        heap_caps_free(s_dscCache);
        s_dscCache = nullptr;
    }
    if (s_lf.cmaps != nullptr)
    {
        for (uint16_t i = 0; i < s_lf.cmap_num; i++)
        {
            if (s_lf.cmaps[i].unicode_list != nullptr)
            {
                heap_caps_free(s_lf.cmaps[i].unicode_list);
            }
            if (s_lf.cmaps[i].glyph_id_ofs_list != nullptr)
            {
                heap_caps_free(s_lf.cmaps[i].glyph_id_ofs_list);
            }
        }
        heap_caps_free(s_lf.cmaps);
        s_lf.cmaps = nullptr;
    }
    if (s_lf.offsets != nullptr)
    {
        heap_caps_free(s_lf.offsets);
        s_lf.offsets = nullptr;
    }
    if (s_glyphBuf != nullptr)
    {
        heap_caps_free(s_glyphBuf);
        s_glyphBuf = nullptr;
        s_glyphBufCap = 0;
    }
    /* ⚠ 这里**不能** memset(&s_lf, ...)：结构体里含 C++ 对象 File，
       整块清零会绕过它 shared_ptr 的析构（文件句柄泄漏，之后再赋值/使用是 UB）。
       只重置标量字段；File 的生命周期由 lfCloseFileLocked() 管。 */
    s_lf.cmaps = nullptr;
    s_lf.cmap_num = 0;
    s_lf.offsets = nullptr;
    s_lf.loca_count = 0;
    s_lf.glyf_start = 0;
    s_lf.glyf_len = 0;
    s_lf.memBytes = 0;
    s_lf.open = false;
    s_lf.nextOpenTryMs = 0;
    s_lf.sd_path[0] = 0;
    s_lf.nbits = 0;
    s_lf.header_bytes = 0;
    s_ready = false;
}

/* 解析 head（返回 ascent/descent，供设置 line_height/base_line） */
static bool lfParseHead(int16_t *ascent, int16_t *descent, int16_t *ulPos, int16_t *ulThick)
{
    uint8_t h[48];
    if (!lfRead(0, h, sizeof(h)))
    {
        return false;
    }
    if (memcmp(h + 4, "head", 4) != 0)
    {
        Serial.println("[LAZYFONT] 不是 LVGL bin 字体（缺 head 表）");
        return false;
    }
    if (rdU32(h) != 48)
    {
        Serial.printf("[LAZYFONT] head 长度异常: %u（期望 48）\n", (unsigned)rdU32(h));
        return false;
    }
    const uint8_t *f = h + 8; // font_header_bin_t
    s_lf.bpp = f[29];
    s_lf.xy_bits = f[30];
    s_lf.wh_bits = f[31];
    s_lf.adv_w_bits = f[32];
    s_lf.index_to_loc = f[26];
    s_lf.adv_w_format = f[28];
    s_lf.default_adv_w = rdU16(f + 22);
    *ascent = rdI16(f + 8);
    *descent = rdI16(f + 10);
    *ulPos = rdI16(f + 36);
    *ulThick = (int16_t)rdU16(f + 38);

    s_lf.nbits = (uint16_t)(s_lf.adv_w_bits + 2u * s_lf.xy_bits + 2u * s_lf.wh_bits);
    s_lf.header_bytes = (uint16_t)((s_lf.nbits + 7u) / 8u);

    if (s_lf.bpp != 1 && s_lf.bpp != 2 && s_lf.bpp != 4 && s_lf.bpp != 8)
    {
        Serial.printf("[LAZYFONT] 不支持的 bpp=%u\n", (unsigned)s_lf.bpp);
        return false;
    }
    if (f[33] != 0)
    {
        Serial.printf("[LAZYFONT] 位图是压缩格式(compression_id=%u)，本模块只支持未压缩\n", f[33]);
        return false;
    }
    return true;
}

/* 解析 cmap：把 213/56 个子表建成 LazyCmap[]（与 lv_font_loader 的 load_cmaps_tables 等价） */
static bool lfParseCmaps(uint32_t cmaps_start)
{
    uint8_t hdr[12];
    if (!lfRead(cmaps_start, hdr, sizeof(hdr)))
    {
        return false;
    }
    if (memcmp(hdr + 4, "cmap", 4) != 0)
    {
        Serial.println("[LAZYFONT] 缺 cmap 表");
        return false;
    }
    uint32_t cmap_num = rdU32(hdr + 8);
    if (cmap_num == 0 || cmap_num > 4096)
    {
        Serial.printf("[LAZYFONT] cmap 子表数异常: %u\n", (unsigned)cmap_num);
        return false;
    }
    s_lf.cmap_num = (uint16_t)cmap_num;
    s_lf.cmaps = (LazyCmap *)psramAlloc(sizeof(LazyCmap) * cmap_num);
    if (s_lf.cmaps == nullptr)
    {
        return false;
    }
    memset(s_lf.cmaps, 0, sizeof(LazyCmap) * cmap_num);
    s_lf.memBytes += sizeof(LazyCmap) * cmap_num;

    // 子表描述数组：cmap_num × 16 字节
    uint32_t tblBytes = 16u * cmap_num;
    uint8_t *tbl = (uint8_t *)psramAlloc(tblBytes);
    if (tbl == nullptr)
    {
        return false;
    }
    bool ok = lfRead(cmaps_start + 12, tbl, tblBytes);
    if (!ok)
    {
        heap_caps_free(tbl);
        return false;
    }

    for (uint32_t i = 0; i < cmap_num; i++)
    {
        const uint8_t *t = tbl + i * 16;
        uint32_t data_offset = rdU32(t);
        LazyCmap *c = &s_lf.cmaps[i];
        c->range_start = rdU32(t + 4);
        c->range_length = rdU16(t + 8);
        c->glyph_id_start = rdU16(t + 10);
        uint16_t entries = rdU16(t + 12);
        c->type = t[14];

        // 子表数据在 cmaps_start + data_offset
        uint32_t at = cmaps_start + data_offset;
        if (c->type == CMAP_FORMAT0_FULL)
        {
            /* ⚠ LVGL 官方 loader 这里把长度存成 uint8_t（>255 会溢出截断），
               本实现用 32 位，修掉了那个隐患。 */
            uint32_t n = entries;
            uint8_t *buf = (uint8_t *)psramAlloc(n ? n : 1);
            if (buf == nullptr || !lfRead(at, buf, n))
            {
                if (buf) heap_caps_free(buf);
                heap_caps_free(tbl);
                return false;
            }
            c->glyph_id_ofs_list = buf;
            c->list_length = c->range_length;
            s_lf.memBytes += n;
        }
        else if (c->type == CMAP_SPARSE_FULL || c->type == CMAP_SPARSE_TINY)
        {
            uint32_t n = entries;
            uint16_t *ul = (uint16_t *)psramAlloc(2u * (n ? n : 1));
            if (ul == nullptr || !lfRead(at, ul, 2u * n))
            {
                if (ul) heap_caps_free(ul);
                heap_caps_free(tbl);
                return false;
            }
            c->unicode_list = ul;
            c->list_length = (uint16_t)n;
            s_lf.memBytes += 2u * n;

            if (c->type == CMAP_SPARSE_FULL)
            {
                uint16_t *ofs = (uint16_t *)psramAlloc(2u * (n ? n : 1));
                if (ofs == nullptr || !lfRead(at + 2u * n, ofs, 2u * n))
                {
                    if (ofs) heap_caps_free(ofs);
                    heap_caps_free(tbl);
                    return false;
                }
                c->glyph_id_ofs_list = ofs;
                s_lf.memBytes += 2u * n;
            }
        }
        else if (c->type != CMAP_FORMAT0_TINY)
        {
            Serial.printf("[LAZYFONT] 未知 cmap 格式 %u\n", (unsigned)c->type);
            heap_caps_free(tbl);
            return false;
        }
    }
    heap_caps_free(tbl);
    Serial.printf("[LAZYFONT] cmap 子表 %u 个已建立索引\n", (unsigned)cmap_num);
    return true;
}

/* 解析 loca：把每个字形的偏移读成 uint32 数组（PSRAM） */
static bool lfParseLoca(uint32_t loca_start, uint32_t *glyf_start)
{
    uint8_t hdr[12];
    if (!lfRead(loca_start, hdr, sizeof(hdr)))
    {
        return false;
    }
    if (memcmp(hdr + 4, "loca", 4) != 0)
    {
        Serial.println("[LAZYFONT] 缺 loca 表");
        return false;
    }
    uint32_t loca_len = rdU32(hdr);
    uint32_t count = rdU32(hdr + 8);
    if (count < 2 || count > 400000)
    {
        Serial.printf("[LAZYFONT] loca count 异常: %u\n", (unsigned)count);
        return false;
    }

    s_lf.loca_count = count;
    s_lf.offsets = (uint32_t *)psramAlloc(sizeof(uint32_t) * count);
    if (s_lf.offsets == nullptr)
    {
        return false;
    }
    s_lf.memBytes += sizeof(uint32_t) * count;

    if (s_lf.index_to_loc == 0)
    {
        // 16 位偏移
        uint8_t *tmp = (uint8_t *)psramAlloc(2u * count);
        if (tmp == nullptr || !lfRead(loca_start + 12, tmp, 2u * count))
        {
            if (tmp) heap_caps_free(tmp);
            return false;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            s_lf.offsets[i] = rdU16(tmp + 2u * i);
        }
        heap_caps_free(tmp);
    }
    else if (s_lf.index_to_loc == 1)
    {
        // 32 位偏移：直接读进 offsets（小端，正好可直接用）
        if (!lfRead(loca_start + 12, s_lf.offsets, 4u * count))
        {
            return false;
        }
    }
    else
    {
        Serial.printf("[LAZYFONT] 未知 index_to_loc=%u\n", (unsigned)s_lf.index_to_loc);
        return false;
    }

    *glyf_start = loca_start + loca_len;

    uint8_t gh[8];
    if (!lfRead(*glyf_start, gh, sizeof(gh)) || memcmp(gh + 4, "glyf", 4) != 0)
    {
        Serial.println("[LAZYFONT] 缺 glyf 表");
        return false;
    }
    s_lf.glyf_len = rdU32(gh);
    s_lf.glyf_start = *glyf_start;

    // 基本健全性检查：偏移必须单调不减，且落在 glyf 段内
    if (s_lf.offsets[0] >= s_lf.glyf_len || s_lf.offsets[count - 1] > s_lf.glyf_len)
    {
        Serial.println("[LAZYFONT] loca 偏移越界，字体可能损坏");
        return false;
    }
    for (uint32_t i = 1; i < count; i++)
    {
        if (s_lf.offsets[i] < s_lf.offsets[i - 1])
        {
            Serial.printf("[LAZYFONT] loca 偏移非单调 @%u\n", (unsigned)i);
            return false;
        }
    }
    return true;
}

/* ==================== 5) cmap 查找 与 字形解码 ==================== */

/* 与 lv_font_fmt_txt.c 的 get_glyph_dsc_id() 等价 */
static uint32_t lfLookupGid(uint32_t letter)
{
    if (letter == 0)
    {
        return 0;
    }
    for (uint16_t i = 0; i < s_lf.cmap_num; i++)
    {
        const LazyCmap *c = &s_lf.cmaps[i];
        uint32_t rcp = letter - c->range_start; // 下溢时自然变成大数，会被下面 continue
        if (rcp > c->range_length)              // ⚠ 与 LVGL 一致（是 > 不是 >=）
        {
            continue;
        }
        if (c->type == CMAP_FORMAT0_TINY)
        {
            return (uint32_t)c->glyph_id_start + rcp;
        }
        if (c->type == CMAP_FORMAT0_FULL)
        {
            const uint8_t *o = (const uint8_t *)c->glyph_id_ofs_list;
            return (uint32_t)c->glyph_id_start + o[rcp];
        }
        if (c->type == CMAP_SPARSE_TINY)
        {
            int idx = bsearchU16(c->unicode_list, c->list_length, (uint16_t)rcp);
            if (idx >= 0)
            {
                return (uint32_t)c->glyph_id_start + (uint32_t)idx;
            }
            return 0;
        }
        if (c->type == CMAP_SPARSE_FULL)
        {
            int idx = bsearchU16(c->unicode_list, c->list_length, (uint16_t)rcp);
            if (idx >= 0)
            {
                const uint16_t *o = (const uint16_t *)c->glyph_id_ofs_list;
                return (uint32_t)c->glyph_id_start + o[idx];
            }
            return 0;
        }
    }
    return 0;
}

// 某个字形在文件里的总字节数（表头 + 位图），与 loader 的算法一致
static uint32_t lfGlyphBytes(uint32_t gid)
{
    uint32_t start = s_lf.offsets[gid];
    uint32_t next = (gid + 1u < s_lf.loca_count) ? s_lf.offsets[gid + 1u] : s_lf.glyf_len;
    if (next <= start)
    {
        return 0;
    }
    return next - start;
}

/* 从 buf 里解出字形表头 */
static void lfDecodeHeader(const uint8_t *buf, uint16_t *adv_w, int16_t *ofs_x, int16_t *ofs_y,
                           uint16_t *box_w, uint16_t *box_h)
{
    BitRd b;
    b.p = buf;
    b.bitpos = 0;
    uint32_t aw = (s_lf.adv_w_bits == 0) ? s_lf.default_adv_w : brRead(&b, s_lf.adv_w_bits);
    if (s_lf.adv_w_format == 0)
    {
        aw *= 16u; // 与 loader 一致：存的是整数像素时要 *16
    }
    *adv_w = (uint16_t)aw;
    *ofs_x = (int16_t)brReadSigned(&b, s_lf.xy_bits);
    *ofs_y = (int16_t)brReadSigned(&b, s_lf.xy_bits);
    *box_w = (uint16_t)brRead(&b, s_lf.wh_bits);
    *box_h = (uint16_t)brRead(&b, s_lf.wh_bits);
}

/* 读一个字形，把「描述符」和「位图」两个缓存一起填好（未命中时调用）。

   为什么合并成一次读卡：
   LVGL 画一个字会先 get_glyph_dsc、紧接着 get_bitmap。如果两者各自按需读卡，
   一个「新字」就要两次 SD 访问（一次读 4 字节表头、一次读整字形）。
   这里改成读一次完整字形、同时填两个缓存 —— 每个新字只花一次 SD 访问，
   首帧开销直接减半。

   de/be 允许传 NULL（表示那一侧不需要）。位图是「尽力而为」：
   空字形 / 超大字形填不进去也不影响描述符（advance 仍然正确）。 */
static bool lfLoadGlyph(uint32_t gid, DscCacheEnt *de, BmpCacheEnt *be)
{
    if (gid == 0 || gid >= s_lf.loca_count)
    {
        return false;
    }
    uint32_t need = lfGlyphBytes(gid);
    if (need < s_lf.header_bytes || need > 65535)
    {
        return false;
    }

    /* 整个字形读进复用缓冲（不够大才重新分配） */
    if (s_glyphBufCap < need)
    {
        if (s_glyphBuf != nullptr)
        {
            heap_caps_free(s_glyphBuf);
        }
        s_glyphBuf = (uint8_t *)psramAlloc(need);
        s_glyphBufCap = (s_glyphBuf != nullptr) ? need : 0;
        s_lf.memBytes += need;
    }
    if (s_glyphBuf == nullptr)
    {
        return false;
    }
    if (!lfRead(s_lf.glyf_start + s_lf.offsets[gid], s_glyphBuf, need))
    {
        return false;
    }

    uint16_t adv_w, box_w, box_h;
    int16_t ofs_x, ofs_y;
    lfDecodeHeader(s_glyphBuf, &adv_w, &ofs_x, &ofs_y, &box_w, &box_h);
    if (de != nullptr)
    {
        de->adv_w = adv_w;
        de->ofs_x = ofs_x;
        de->ofs_y = ofs_y;
        de->box_w = box_w;
        de->box_h = box_h;
    }

    if (be == nullptr)
    {
        return true;
    }

    uint32_t nbits = (uint32_t)box_w * box_h * s_lf.bpp;
    uint32_t len = (nbits + 7u) / 8u;
    if (len == 0)
    {
        len = 1; // 空字形也给 1 字节，保证不返回 NULL（否则 LVGL 会跳过推进宽度）
    }
    if (len > LAZY_MAX_BMP)
    {
        return true; // 描述符已填好，只是位图太大不缓存
    }
    if (be->cap < len)
    {
        if (be->bmp != nullptr)
        {
            heap_caps_free(be->bmp);
            be->bmp = nullptr;
            be->cap = 0;
        }
        be->bmp = (uint8_t *)psramAlloc(len);
        be->cap = (be->bmp != nullptr) ? (uint16_t)len : 0;
        s_lf.memBytes += len;
    }
    if (be->bmp == nullptr)
    {
        return true;
    }

    /* 位图位从表头之后开始，是同一条 MSB-first 位流；
       这里重新按字节对齐打包（LVGL 期望的就是这种连续位流）。 */
    memset(be->bmp, 0, len);
    BitRd b;
    b.p = s_glyphBuf;
    b.bitpos = s_lf.nbits;
    for (uint32_t i = 0; i < nbits; i++)
    {
        if (brRead(&b, 1))
        {
            be->bmp[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
        }
    }
    be->len = (uint16_t)len;
    be->gid = gid;
    return true;
}

/* ==================== 6) LVGL 回调 ==================== */

static bool lazyGetGlyphDsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                            uint32_t letter, uint32_t letter_next)
{
    (void)font;
    (void)letter_next; // 本字体无 kerning
    if (!s_ready || s_dscCache == nullptr)
    {
        return false;
    }
    if (letter == '\t')
    {
        letter = ' ';
    }
    uint32_t gid = lfLookupGid(letter);
    if (gid == 0)
    {
        return false; // 交回给 LVGL（占位/回退字体）
    }

    DscCacheEnt *e = &s_dscCache[gid & DSC_CACHE_MASK];
    if (e->gid != gid)
    {
        s_dscMiss++;
        e->gid = 0;
        /* 一次读卡就把位图也填好：LVGL 紧接着会来问位图，
           分开读的话每个「新字」要两次 SD 访问。 */
        BmpCacheEnt *b = &s_bmpCache[gid & BMP_CACHE_MASK];
        b->gid = 0;
        if (!lfLoadGlyph(gid, e, b))
        {
            return false;
        }
        e->gid = gid;
    }

    /* 与 lv_font_get_glyph_dsc_fmt_txt 完全一致的换算：
       adv_w 存的是 1/16 px，输出前 +8 再 >>4（四舍五入到整像素） */
    dsc_out->adv_w = (uint16_t)((e->adv_w + (1u << 3)) >> 4);
    dsc_out->box_w = e->box_w;
    dsc_out->box_h = e->box_h;
    dsc_out->ofs_x = e->ofs_x;
    dsc_out->ofs_y = e->ofs_y;
    dsc_out->bpp = s_lf.bpp;
    dsc_out->is_placeholder = false;
    return true;
}

static const uint8_t *lazyGetGlyphBitmap(const lv_font_t *font, uint32_t letter)
{
    (void)font;
    if (!s_ready || s_bmpCache == nullptr)
    {
        return nullptr;
    }
    if (letter == '\t')
    {
        letter = ' ';
    }
    uint32_t gid = lfLookupGid(letter);
    if (gid == 0)
    {
        return nullptr;
    }

    BmpCacheEnt *e = &s_bmpCache[gid & BMP_CACHE_MASK];
    if (e->gid == gid && e->bmp != nullptr)
    {
        s_bmpHit++;
        return e->bmp;
    }

    s_bmpMiss++;
    e->gid = 0;
    /* 顺手把描述符也缓存上 —— 这次读卡本来就把表头解出来了，不用再读一次。 */
    DscCacheEnt *d = &s_dscCache[gid & DSC_CACHE_MASK];
    d->gid = 0;
    if (!lfLoadGlyph(gid, d, e))
    {
        return nullptr;
    }
    d->gid = gid;
    return (e->gid == gid) ? e->bmp : nullptr;
}

/* ==================== 7) 对外接口 ==================== */

void LazyFont_Close(void)
{
    lfCloseFileLocked();
    lfFreeAll();
    memset(&s_font, 0, sizeof(s_font));
}

const lv_font_t *LazyFont_Open(const char *sdRelativePath, const lv_font_t *fallback)
{
    LazyFont_Close();

    if (sdRelativePath == nullptr || sdRelativePath[0] == 0)
    {
        return nullptr;
    }

    /* 归一化成「相对挂载点」的路径（SD.open 需要的形式，见结构体里的说明）。 */
    if (sdRelativePath[0] == '/')
    {
        snprintf(s_lf.sd_path, sizeof(s_lf.sd_path), "%s", sdRelativePath);
    }
    else
    {
        snprintf(s_lf.sd_path, sizeof(s_lf.sd_path), "/%s", sdRelativePath);
    }

    /* 两个一级缓存放 PSRAM（见文件头说明：不能占内部 RAM） */
    s_dscCache = (DscCacheEnt *)psramAlloc(sizeof(DscCacheEnt) * DSC_CACHE_N);
    s_bmpCache = (BmpCacheEnt *)psramAlloc(sizeof(BmpCacheEnt) * BMP_CACHE_N);
    if (s_dscCache == nullptr || s_bmpCache == nullptr)
    {
        Serial.println("[LAZYFONT] 缓存分配失败（PSRAM 不足）");
        LazyFont_Close();
        return nullptr;
    }
    memset(s_dscCache, 0, sizeof(DscCacheEnt) * DSC_CACHE_N);
    memset(s_bmpCache, 0, sizeof(BmpCacheEnt) * BMP_CACHE_N);
    s_lf.memBytes += sizeof(DscCacheEnt) * DSC_CACHE_N + sizeof(BmpCacheEnt) * BMP_CACHE_N;

    if (!lfOpenFile())
    {
        const char *mp = SD.mountpoint();
        Serial.printf("[LAZYFONT] 打不开 %s%s  (SD.open(\"%s\") 失败)\n",
                      (mp != nullptr && mp[0] != 0) ? mp : "/sd",
                      s_lf.sd_path, s_lf.sd_path);
        LazyFont_Close();
        return nullptr;
    }

    uint32_t t0 = millis();
    int16_t ascent = 0, descent = 0, ulPos = 0, ulThick = 0;

    if (!lfParseHead(&ascent, &descent, &ulPos, &ulThick))
    {
        LazyFont_Close();
        return nullptr;
    }
    uint32_t cmaps_start = 48; // head 段长度（[u32 len]["head"][40B]）
    if (!lfParseCmaps(cmaps_start))
    {
        LazyFont_Close();
        return nullptr;
    }
    // cmap 段长度 → loca 起点
    uint8_t ch[4];
    if (!lfRead(cmaps_start, ch, 4))
    {
        LazyFont_Close();
        return nullptr;
    }
    uint32_t loca_start = cmaps_start + rdU32(ch);
    uint32_t glyf_start = 0;
    if (!lfParseLoca(loca_start, &glyf_start))
    {
        LazyFont_Close();
        return nullptr;
    }

    // 组装 lv_font_t
    memset(&s_font, 0, sizeof(s_font));
    s_font.get_glyph_dsc = lazyGetGlyphDsc;
    s_font.get_glyph_bitmap = lazyGetGlyphBitmap;
    s_font.line_height = (lv_coord_t)((int32_t)ascent - (int32_t)descent);
    s_font.base_line = (lv_coord_t)(-(int32_t)descent);
    s_font.subpx = LV_FONT_SUBPX_NONE;
    s_font.underline_position = (int8_t)ulPos;
    s_font.underline_thickness = (int8_t)ulThick;
    s_font.dsc = &s_lf;
    s_font.fallback = fallback;

    s_ready = true;

    Serial.printf("[LAZYFONT] 索引建立完成: 字形 %u, cmap %u, 耗时 %u ms, 索引占用约 %u KB\n",
                  (unsigned)s_lf.loca_count, (unsigned)s_lf.cmap_num,
                  (unsigned)(millis() - t0), (unsigned)(s_lf.memBytes / 1024));
    Serial.printf("[LAZYFONT] line_height=%d base_line=%d bpp=%u (位图按需从 SD 读取)\n",
                  (int)s_font.line_height, (int)s_font.base_line, (unsigned)s_lf.bpp);
    return &s_font;
}

void LazyFont_GetStats(uint32_t *dscMiss, uint32_t *bmpMiss, uint32_t *bmpHit)
{
    if (dscMiss) *dscMiss = s_dscMiss;
    if (bmpMiss) *bmpMiss = s_bmpMiss;
    if (bmpHit) *bmpHit = s_bmpHit;
}

uint32_t LazyFont_GetMemoryUsage(void)
{
    return s_lf.memBytes;
}

#endif /* ENABLE_EXTERNAL_FONT */
