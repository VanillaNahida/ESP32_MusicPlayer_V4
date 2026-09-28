// 专辑封面显示模块（异步实现）

#include "AlbumArt.h"

#include <Arduino.h>
#include <string.h>

#include "lvgl.h"
#include "ui.h"
#include "AudioFileSourceID3.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

extern "C" {
#include "rom/tjpgd.h"

// lodepng 以 C 语言编译进 LVGL，导出的是 C 链接符号（名字未被 mangle）。
// 不能直接 include lodepng.h：它在 C++ 下的声明会带上 C++ 链接，链接期找不到符号。
unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                          const unsigned char *in, size_t insize);
}

// 海报位尺寸（与 ui_Screen1 里的 ui_haibao 一致）// 原93px
#define ALBUM_ART_BOX 155
// 每像素 3 字节（RGB565 低字节、高字节、Alpha）
#define ALBUM_ART_BUF_BYTES (ALBUM_ART_BOX * ALBUM_ART_BOX * 3)
// tjpgd 工作区大小
#define ALBUM_ART_WORK 8192

// ---------- 后台解码任务与状态 ----------
static TaskHandle_t s_artTask = nullptr;
static SemaphoreHandle_t s_artMutex = nullptr; // 保护 revision / buffer 指针的交接

static volatile uint32_t s_reqRevision = 0;   // 待解码的封面版本（0 表示无请求）
static volatile bool s_reqPending = false;    // 是否有未处理的请求
static uint8_t *s_readyBuf = nullptr;         // 后台已解码好、等待 UI 取走的缓冲
static uint32_t s_readyRevision = 0;
static uint8_t *s_displayBuf = nullptr;       // 当前正被 LVGL 使用的缓冲
static lv_img_dsc_t s_artDsc;

static uint8_t *s_work = nullptr;             // tjpgd 工作区（仅后台任务使用）

// JPEG 解码上下文
struct JpgCtx
{
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint8_t *out;
    int srcW;
    int srcH;
    int outW;
    int outH;
    uint32_t lastYieldMs; // 上次主动让出 CPU 的时刻（见 artYieldIfDue）
};

static JpgCtx s_ctx;
static JDEC s_jdec;

// 在 PSRAM 优先分配
static uint8_t *artAlloc(size_t n)
{
    uint8_t *p = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == nullptr)
    {
        p = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

// ---------- JPEG ----------
static UINT jpgReadCb(JDEC *jd, BYTE *buff, UINT nbyte)
{
    JpgCtx *ctx = (JpgCtx *)jd->device;
    size_t remain = (ctx->pos < ctx->size) ? (ctx->size - ctx->pos) : 0;
    UINT n = (nbyte <= remain) ? nbyte : (UINT)remain;
    if (n == 0)
    {
        return 0;
    }
    if (buff)
    {
        memcpy(buff, ctx->data + ctx->pos, n);
    }
    ctx->pos += n;
    return n;
}

/* ==================================================================
   【这张回调里必须定期让出 CPU —— 否则整机会被看门狗复位】

   本函数在 albumArtTask 里被调用，那个任务被固定在 **core0、优先级 1**。
   而 FreeRTOS 的 IDLE 任务优先级是 **0** —— 也就是说：
   只要 albumArtTask 在连着算不让出 CPU，IDLE0 就连一次都拿不到 CPU。

   而 ESP-IDF 的 Task Watchdog 正是靠 IDLE0 定期「喂狗」来判断系统还活着的，
   喂不上就 Aborting + 整机复位。实测现象：

       E task_wdt: Task watchdog got triggered.
       E task_wdt:  - IDLE0 (CPU 0)
       E task_wdt: CPU 0: audioTask          ← 只有当前任务的快照，别被它带偏

   触发条件是「封面足够大」：1500x1500 的图 tjpgd 要哈夫曼解码 225 万像素，
   在 240MHz 上要好几秒，远超看门狗超时。所以只要播放大封面的歌就必然重启。

   注意：光在 Music_Loop() 里加 delay 是没用的 —— 音频任务让出 CPU 之后，
   排在 IDLE0 前面的 albumArtTask（优先级 1 > 0）会立刻顶上，IDLE0 照样饿死。
   必须让**这个任务自己**定期阻塞，IDLE0 才有机会跑。

   做法：每 5ms 主动 vTaskDelay(1)。对解码总耗时的影响可以忽略
   （一次 1500x1500 的解码大约 150 次回调，合计多花 150ms）。
   ================================================================== */
static inline void artYieldIfDue(JpgCtx *ctx)
{
    if ((uint32_t)(millis() - ctx->lastYieldMs) >= 5)
    {
        ctx->lastYieldMs = millis();
        vTaskDelay(1);
    }
}

static UINT jpgWriteCb(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpgCtx *ctx = (JpgCtx *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;

    artYieldIfDue(ctx); // 见上面那段说明：不让出 CPU 会被看门狗复位

    for (int y = (int)rect->top; y <= (int)rect->bottom; y++)
    {
        int dy = (int)((int32_t)y * ctx->outH / ctx->srcH);
        if (dy >= ctx->outH)
        {
            dy = ctx->outH - 1;
        }
        uint8_t *row = ctx->out + (size_t)dy * ctx->outW * 3;

        for (int x = (int)rect->left; x <= (int)rect->right; x++)
        {
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;

            int dx = (int)((int32_t)x * ctx->outW / ctx->srcW);
            if (dx >= ctx->outW)
            {
                dx = ctx->outW - 1;
            }
            if (dx < 0 || dy < 0)
            {
                continue;
            }
            uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            uint8_t *p = row + (size_t)dx * 3;
            p[0] = (uint8_t)(c & 0xFF);
            p[1] = (uint8_t)(c >> 8);
            p[2] = 0xFF;
        }
    }
    return 1;
}

static bool decodeJpeg(const uint8_t *data, size_t len, uint8_t *out)
{
    if (s_work == nullptr)
    {
        s_work = (uint8_t *)heap_caps_malloc(ALBUM_ART_WORK, MALLOC_CAP_8BIT);
        if (s_work == nullptr)
        {
            return false;
        }
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.data = data;
    s_ctx.size = len;

    JRESULT jr = jd_prepare(&s_jdec, jpgReadCb, s_work, ALBUM_ART_WORK, &s_ctx);
    if (jr != JDR_OK)
    {
        Serial.printf("封面：JPEG 头解析失败 (%d)\n", (int)jr);
        return false;
    }

    /* 【失控保护】超大封面直接放弃。
       解码耗时与源图像素数成正比 —— tjpgd 即使降采样，也要把整幅图的
       哈夫曼数据解一遍。1500x1500 就要好几秒，几万像素的图会让
       albumArtTask 长时间霸占 core0（界面刷新、SD 读卡都会跟着变慢）。
       （让出 CPU 的逻辑已经能保证不触发看门狗，这个上限纯粹是为了体验。）
       显示框只有 155x155，4096x4096 已经是 700 倍余量，正常封面够用。 */
    if ((uint64_t)s_jdec.width * (uint64_t)s_jdec.height > (4096ULL * 4096ULL))
    {
        Serial.printf("封面：源图过大 (%ux%u)，跳过解码\n",
                      (unsigned)s_jdec.width, (unsigned)s_jdec.height);
        return false;
    }

    // 取最大的降采样级别，让解码尺寸仍不小于显示框（后续只做缩小）
    int scale = 0;
    for (int k = 3; k >= 0; k--)
    {
        UINT w = (s_jdec.width + (1u << k) - 1) >> k;
        UINT h = (s_jdec.height + (1u << k) - 1) >> k;
        if (w >= ALBUM_ART_BOX && h >= ALBUM_ART_BOX)
        {
            scale = k;
            break;
        }
    }

    s_ctx.srcW = (int)((s_jdec.width + (1u << scale) - 1) >> scale);
    s_ctx.srcH = (int)((s_jdec.height + (1u << scale) - 1) >> scale);
    s_ctx.outW = ALBUM_ART_BOX;
    s_ctx.outH = ALBUM_ART_BOX;
    s_ctx.out = out;
    if (s_ctx.srcW <= 0 || s_ctx.srcH <= 0)
    {
        return false;
    }

    jr = jd_decomp(&s_jdec, jpgWriteCb, (BYTE)scale);
    if (jr != JDR_OK)
    {
        Serial.printf("封面：JPEG 解码失败 (%d)\n", (int)jr);
        return false;
    }
    Serial.printf("封面：JPEG 已解码 %ux%u (1/%d)\n", (unsigned)s_jdec.width, (unsigned)s_jdec.height, 1 << scale);
    return true;
}

// ---------- PNG ----------
static bool decodePng(const uint8_t *data, size_t len, uint8_t *out)
{
    unsigned w = 0;
    unsigned h = 0;
    unsigned char *rgba = nullptr;

    unsigned err = lodepng_decode32(&rgba, &w, &h, data, len);
    if (err != 0 || rgba == nullptr || w == 0 || h == 0)
    {
        Serial.printf("封面：PNG 解码失败 (%u)\n", err);
        if (rgba != nullptr)
        {
            free(rgba);
        }
        return false;
    }

    for (int dy = 0; dy < ALBUM_ART_BOX; dy++)
    {
        /* 同样要定期让出 CPU：lodepng_decode32 本身是长计算，
           这里的缩放循环也要几百毫秒（见 artYieldIfDue 的说明）。 */
        if ((dy & 15) == 0)
        {
            vTaskDelay(1);
        }

        uint32_t sy = (uint32_t)dy * h / ALBUM_ART_BOX;
        uint8_t *row = out + (size_t)dy * ALBUM_ART_BOX * 3;
        for (int dx = 0; dx < ALBUM_ART_BOX; dx++)
        {
            uint32_t sx = (uint32_t)dx * w / ALBUM_ART_BOX;
            const uint8_t *p = rgba + ((size_t)sy * w + sx) * 4;
            uint16_t c = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
            uint8_t *o = row + (size_t)dx * 3;
            o[0] = (uint8_t)(c & 0xFF);
            o[1] = (uint8_t)(c >> 8);
            o[2] = p[3];
        }
    }
    free(rgba);
    Serial.printf("封面：PNG 已解码 %ux%u\n", w, h);
    return true;
}

// 后台任务：串行处理封面解码请求（低优先级，不抢 UI / 音频）
static void albumArtTask(void *param)
{
    (void)param;
    for (;;)
    {
        if (!s_reqPending)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t rev = s_reqRevision;

        // 读取封面数据（持锁，避免解码任务换歌时释放缓冲）
        if (!AudioFileSourceID3::pictureLock(200))
        {
            s_reqPending = false;
            continue;
        }
        const uint8_t *data = AudioFileSourceID3::pictureData();
        size_t len = AudioFileSourceID3::pictureSize();
        uint32_t curRev = AudioFileSourceID3::pictureRevision();

        // 版本已变（换歌了）→ 放弃这次解码
        if (data == nullptr || len < 8 || curRev != rev)
        {
            AudioFileSourceID3::pictureUnlock();
            s_reqPending = false;
            continue;
        }

        uint8_t *buf = artAlloc(ALBUM_ART_BUF_BYTES);
        bool ok = false;
        uint32_t decodeStart = millis();
        if (buf != nullptr)
        {
            if (data[0] == 0xFF && data[1] == 0xD8)
            {
                ok = decodeJpeg(data, len, buf);
            }
            else if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
            {
                ok = decodePng(data, len, buf);
            }
            else
            {
                Serial.printf("封面：未知图片格式（首字节 0x%02X%02X）\n", data[0], data[1]);
            }
        }
        else
        {
            Serial.println("封面：解码缓冲分配失败");
        }
        /* 报一下解码耗时和源图尺寸。
           大封面的解码是纯 CPU 长任务：1500x1500 的 JPEG 要好几秒。
           这段期间 albumArtTask（core0，优先级 1）一直占着 CPU，
           所以它必须定期 vTaskDelay 让 IDLE0 喂狗（见 artYieldIfDue 的说明）。
           这条日志能直接告诉你「封面是不是太大了」。 */
        uint32_t decodeCost = millis() - decodeStart;
        Serial.printf("封面：解码%s，耗时 %u ms，源图 %ux%u\n",
                      ok ? "成功" : "失败", (unsigned)decodeCost,
                      (unsigned)s_jdec.width, (unsigned)s_jdec.height);
        AudioFileSourceID3::pictureUnlock();

        s_reqPending = false;

        if (!ok)
        {
            continue; // 解码失败：保持默认封面
        }

        // 交接给 UI：若已有未被取走的旧结果，直接丢弃（UI 还没换上就走下一首了）
        xSemaphoreTake(s_artMutex, portMAX_DELAY);
        if (s_readyBuf != nullptr)
        {
            heap_caps_free(s_readyBuf);
        }
        s_readyBuf = buf;
        s_readyRevision = rev;
        xSemaphoreGive(s_artMutex);
    }
}

void AlbumArt_Init(void)
{
    if (s_artMutex == nullptr)
    {
        s_artMutex = xSemaphoreCreateMutex();
    }
    if (s_artTask == nullptr)
    {
        // 优先级 1（低于 LVGL/音频任务），栈 8KB
        xTaskCreatePinnedToCore(albumArtTask, "albumArt", 8192, nullptr, 1, &s_artTask, 0);
    }
}

void AlbumArt_ShowDefault(void)
{
    if (ui_haibao == nullptr)
    {
        return;
    }
    lv_img_set_src(ui_haibao, &ui_img_haibao_png);
    lv_obj_set_size(ui_haibao, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_invalidate(ui_haibao);
}

void AlbumArt_Request(uint32_t revision)
{
    // 立刻先显示默认封面，解码完成后再替换 —— 保证界面永远不等待封面
    AlbumArt_ShowDefault();

    xSemaphoreTake(s_artMutex, portMAX_DELAY);
    // 旧的结果已无意义
    if (s_readyBuf != nullptr)
    {
        heap_caps_free(s_readyBuf);
        s_readyBuf = nullptr;
    }
    // 释放上一首正在显示的图（此时 UI 已切回默认封面，不再引用它）
    if (s_displayBuf != nullptr)
    {
        heap_caps_free(s_displayBuf);
        s_displayBuf = nullptr;
    }
    xSemaphoreGive(s_artMutex);

    s_reqRevision = revision;
    s_reqPending = true;
}

bool AlbumArt_Poll(void)
{
    if (ui_haibao == nullptr || s_artMutex == nullptr)
    {
        return false;
    }

    uint8_t *buf = nullptr;
    xSemaphoreTake(s_artMutex, portMAX_DELAY);
    if (s_readyBuf != nullptr)
    {
        buf = s_readyBuf;
        s_readyBuf = nullptr;
        // 上一张正在显示的图可以释放了（LVGL 下一秒就会引用新图）
        if (s_displayBuf != nullptr)
        {
            heap_caps_free(s_displayBuf);
        }
        s_displayBuf = buf;
    }
    xSemaphoreGive(s_artMutex);

    if (buf == nullptr)
    {
        return false;
    }

    s_artDsc.header.always_zero = 0;
    s_artDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_artDsc.header.reserved = 0;
    s_artDsc.header.w = ALBUM_ART_BOX;
    s_artDsc.header.h = ALBUM_ART_BOX;
    s_artDsc.data_size = ALBUM_ART_BUF_BYTES;
    s_artDsc.data = buf;

    lv_img_set_src(ui_haibao, &s_artDsc);
    lv_obj_set_size(ui_haibao, ALBUM_ART_BOX, ALBUM_ART_BOX);
    lv_obj_invalidate(ui_haibao);
    return true;
}

void AlbumArt_Release(void)
{
    if (s_artMutex == nullptr)
    {
        return;
    }
    xSemaphoreTake(s_artMutex, portMAX_DELAY);
    if (s_readyBuf != nullptr)
    {
        heap_caps_free(s_readyBuf);
        s_readyBuf = nullptr;
    }
    xSemaphoreGive(s_artMutex);
    s_reqPending = false;
}
