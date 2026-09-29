// 运行时字体选择实现（接口说明见 include/FontManager.h）
//
// ── 编译期开关：ENABLE_EXTERNAL_FONT（见 platformio.ini）──
//   0（默认）：**完全不碰 SD 卡**，直接用固件内置的全量字库。
//             内置字库已是 DreamHanSansSC-W17 的 44,812 字形，够用且最快。
//   1        ：允许把 SD 卡 <卡根>/fonts/*.bin 当字体，按需读入 PSRAM。
//             换字体不用重新烧录固件，代价是渲染期会零星读卡。
//
// 为什么外部字体要「按需」而不是整个读进来：见 include/LazyFont.h ——
// LVGL 自带的 lv_font_load() 要为每个字形做 seek + 逐字节读、还走两遍，
// 4.5 万字形就是几十万次 VFS/FATFS/SPI 调用（几分钟，表现为开机卡死）。

#include "FontManager.h"

#include <Arduino.h>

#include "ui.h" // ui_font_AlibabaPuHuiTi_12（内置全量字库）

#ifndef ENABLE_EXTERNAL_FONT
#define ENABLE_EXTERNAL_FONT 0
#endif

static const lv_font_t *s_font = &ui_font_AlibabaPuHuiTi_12;
static bool s_externalLoaded = false;

const lv_font_t *FontManager_GetFont(void)
{
    return s_font;
}

bool FontManager_IsExternalFontLoaded(void)
{
    return s_externalLoaded;
}

#if ENABLE_EXTERNAL_FONT

/* ==================== 外部字库（SD 卡）路径 ==================== */

#include <SD.h>

#include "Music.h" // Music_IsStorageReady() / Music_OpenDir 等
#include "LazyFont.h"

/* SD 卡上的字体候选文件（相对 **SD 卡根目录**）。
   按顺序尝试，第一个能打开的生效 —— 生成好的 .bin 用下面任何一个名字都行：
        <SD 卡根>/fonts/default.bin
        <SD 卡根>/fonts/font.bin
        <SD 卡根>/fonts/DreamHanSansSC-W17-12.bin
   注意是 **卡根目录** 下的 fonts 文件夹，不是 /Music/fonts 之类的子目录。
   固件实际传给 SD.open() 的是相对路径 /fonts/xxx.bin
   （Arduino 的 VFSImpl::open 内部会自己补上挂载点 /sd，这里不能重复拼）。 */
static const char *kFontCandidates[] = {
    "/fonts/default.bin",
    "/fonts/font.bin",
    "/fonts/DreamHanSansSC-W17-12.bin",
};
static const int kFontCandidateCount =
    (int)(sizeof(kFontCandidates) / sizeof(kFontCandidates[0]));

static const char *s_activePath = kFontCandidates[0];

const char *FontManager_GetFontPath(void)
{
    return s_activePath;
}

/* ---- 字体没找到时的目录诊断 ----
   把固件「眼里」的目录内容打出来，省得反复猜文件该放哪。
   用的是 Music_OpenDir/ReadDir（自带 SPI 总线锁，且是 O(n) 顺序枚举）。 */
static void fontListDir(const char *dir, bool withSize)
{
    void *d = Music_OpenDir(dir);
    if (d == nullptr)
    {
        Serial.printf("[FONT]   %s : <打不开 —— 目录不存在？>\n", dir);
        return;
    }
    String name;
    bool isDir = false;
    int n = 0;
    while (n < 40 && Music_ReadDir(d, name, isDir))
    {
        String full = String(dir);
        if (!full.endsWith("/"))
        {
            full += "/";
        }
        full += name;
        if (isDir)
        {
            Serial.printf("[FONT]   %s/\n", full.c_str());
        }
        else if (withSize)
        {
            Serial.printf("[FONT]   %-42s %u 字节\n", full.c_str(),
                          (unsigned)Music_GetFileSize(full.c_str()));
        }
        else
        {
            Serial.printf("[FONT]   %s\n", full.c_str());
        }
        n++;
    }
    Music_CloseDir(d);
    if (n == 0)
    {
        Serial.printf("[FONT]   %s : <空目录>\n", dir);
    }
    else if (n >= 40)
    {
        Serial.println("[FONT]   ... (已截断)");
    }
}

static void fontDumpCard(void)
{
    const char *mp = SD.mountpoint();
    const char *base = (mp != nullptr && mp[0] != 0) ? mp : "/sd";
    Serial.printf("[FONT] SD 挂载点 = %s\n", base);
    Serial.printf("[FONT] 要放的位置：卡根目录的 fonts 文件夹，即固件路径 %s/fonts/<名字>.bin\n", base);
    Serial.println("[FONT] ---- 卡根目录内容 ----");
    fontListDir("/", false);
    Serial.println("[FONT] ---- /fonts 目录内容 ----");
    fontListDir("/fonts", true);
}

void FontManager_Init(void)
{
    // 先把内置字库设成兜底：后面任何一步失败都不需要再管
    s_font = &ui_font_AlibabaPuHuiTi_12;
    s_externalLoaded = false;

    Serial.println("[FONT] ENABLE_EXTERNAL_FONT=1：尝试启用 SD 卡外部字库");

    if (!Music_IsStorageReady())
    {
        Serial.println("[FONT] SD 卡未就绪，直接使用内置字库");
        return;
    }

    uint32_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[FONT] 加载前空闲内存: 内部=%u KB PSRAM=%u KB\n",
                  (unsigned)(internalBefore / 1024), (unsigned)(psramBefore / 1024));

    /* 依次尝试候选文件名，第一个成功的生效。
       注意：这里只读索引，不读字形位图，所以很快。 */
    const lv_font_t *loaded = nullptr;
    for (int i = 0; i < kFontCandidateCount && loaded == nullptr; i++)
    {
        Serial.printf("[FONT] 尝试加载 SD 字体(按需): %s\n", kFontCandidates[i]);
        loaded = LazyFont_Open(kFontCandidates[i], &ui_font_AlibabaPuHuiTi_12);
        if (loaded != nullptr)
        {
            s_activePath = kFontCandidates[i];
        }
    }

    if (loaded != nullptr)
    {
        s_font = loaded;
        s_externalLoaded = true;
        Serial.printf("[FONT] 已启用 SD 卡字体(按需加载): %s\n", s_activePath);
        Serial.printf("[FONT] line_height=%d base_line=%d | 内置字库 line_height=%d base_line=%d\n",
                      (int)loaded->line_height, (int)loaded->base_line,
                      (int)ui_font_AlibabaPuHuiTi_12.line_height,
                      (int)ui_font_AlibabaPuHuiTi_12.base_line);
        if (loaded->line_height != ui_font_AlibabaPuHuiTi_12.line_height)
        {
            Serial.println("[FONT] 注意：两者行高不同，界面排版可能与原设计不一致");
        }
        Serial.printf("[FONT] 占用: PSRAM %u KB, 内部 %u KB (索引+缓存，不含字形位图)\n",
                      (unsigned)((psramBefore - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / 1024),
                      (unsigned)((internalBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
    }
    else
    {
        Serial.println("[FONT] SD 字体不可用（文件不存在 / 格式不符 / 内存不足），回退内置字库");
        fontDumpCard();
        Serial.printf("[FONT] 回退后空闲内存: 内部=%u KB PSRAM=%u KB\n",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

#else /* !ENABLE_EXTERNAL_FONT */

/* ==================== 只用内置字库（默认） ====================
   一个字都不读卡：不建索引、不分配缓存、不占额外 PSRAM。
   内置字库本身就是全量字符集（44,812 字形），所以这是最快的路径。 */

const char *FontManager_GetFontPath(void)
{
    return "(built-in)";
}

void FontManager_Init(void)
{
    s_font = &ui_font_AlibabaPuHuiTi_12;
    s_externalLoaded = false;
    Serial.println("[FONT] 外部字库未启用（ENABLE_EXTERNAL_FONT=0），使用内置全量字库"
                   "（如需从 SD 卡换字体：在 platformio.ini 里打开该宏）");
}

#endif /* ENABLE_EXTERNAL_FONT */
