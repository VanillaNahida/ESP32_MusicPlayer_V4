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

/* ==================================================================
   【已解码封面的缓存】

   为什么需要它：封面解码是几百毫秒到数秒的纯 CPU 重活。原来的实现里，
   只要 UI 重新走一次「请求封面」的流程（典型场景：进列表页再返回播放页，
   主界面被 ui_Screen1_screen_init() 整个重建），就会重新解码一遍同一张图 ——
   用户看到的就是「切个页面回来，封面又空白一下再慢慢出来」。

   现在按 revision（封面版本号，换歌才变）缓存解码结果：
     · revision 没变 → 直接复用 s_cacheBuf，零解码、瞬间出图；
     · revision 变了   → 才真正重新解码。

   缓存的是**解码后的最终位图**（155x155 RGB565+Alpha ≈ 72KB），
   放在 PSRAM 里。只缓存当前这一首：换歌时旧图没有任何复用价值，
   多缓存只会白占内存。

   线程约定：s_cacheBuf / s_cacheRevision 只在下述两处被改动，
     · DecodeTask 线程：解码成功后写入；
     · LVGL 线程（AlbumArt_Request / AlbumArt_Release）：换歌时清掉。
   两者都用 s_artMutex 保护。
   ================================================================== */
static uint8_t *s_cacheBuf = nullptr;      // 缓存：已解码好的位图
static uint32_t s_cacheRevision = 0;       // 上面这张图对应的封面版本（0 = 无缓存）

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

        /* 交接给 UI，并把结果登记为「当前这一版的缓存」。

           三个指针可能是同一块内存，所以判重一律基于「改动前的快照」：
             s_readyBuf   —— 解码好了、等 UI 来取的新图（本函数刚产出）
             s_cacheBuf   —— 同一首歌切页面时复用用的图
             s_displayBuf —— LVGL 此刻正在画的那张图（core1 在引用！）

           **绝不能释放 s_displayBuf 指向的块** —— 它正被 LVGL 使用，
           释放了就是画到已释放内存。它会在 AlbumArt_Poll() 取走新图之后，
           由图源切换完成时才安全释放。 */
        xSemaphoreTake(s_artMutex, portMAX_DELAY);

        uint8_t *const oldReady = s_readyBuf;
        uint8_t *const oldCache = s_cacheBuf;
        uint8_t *const displayed = s_displayBuf;

        // 旧值先作废，避免下面释放后仍被当成有效指针使用
        s_cacheBuf = buf;
        s_cacheRevision = rev;
        s_readyBuf = buf;
        s_readyRevision = rev;
        xSemaphoreGive(s_artMutex);

        /* 收尾释放（放在锁外，避免长时间持锁）：
           1) 上一张「解好但没被取走」的图：没人引用 → 放
           2) 上一版缓存：只要不是在显示的 → 放
           两块都跳过「正在显示的」和「就是新图本身」的情况。 */
        if (oldReady != nullptr && oldReady != buf && oldReady != displayed)
        {
            heap_caps_free(oldReady);
        }
        if (oldCache != nullptr && oldCache != buf && oldCache != displayed)
        {
            heap_caps_free(oldCache);
        }
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
    /* 【缓存命中：直接复用，不重新解码】
       revision 是「封面版本号」，只有换歌才会变。所以同一首歌里
       无论 UI 重建多少次（进列表页再返回、切页面回来……），
       都走这一条分支：把缓存的位图直接挂到 ui_haibao 上，
       既不闪默认封面，也不用等几百毫秒。

       这里刻意**不**先调用 AlbumArt_ShowDefault()：
       缓存命中时用户应该立刻看到原图，先闪一下默认封面反而更难看。 */
    if (s_artMutex != nullptr)
    {
        uint8_t *buf = nullptr;
        uint8_t *oldDisplay = nullptr;
        xSemaphoreTake(s_artMutex, portMAX_DELAY);
        const bool hit = (s_cacheBuf != nullptr && revision != 0 &&
                          revision == s_cacheRevision);
        if (hit)
        {
            buf = s_cacheBuf;
            /* 旧的显示缓冲只摘指针、不在这里释放 —— 它此刻仍被 LVGL
               通过 s_artDsc 引用着，要等下面 lv_img_set_src() 换完图源
               才能真正回收。放在这里 free 就是「释放正在绘制的内存」。
               同时排除别名：缓存命中后 s_displayBuf 往往就等于 s_cacheBuf，
               那种情况根本没有旧图要回收。 */
            if (s_displayBuf != nullptr && s_displayBuf != buf)
            {
                oldDisplay = s_displayBuf;
            }
            s_displayBuf = buf;
        }
        xSemaphoreGive(s_artMutex);

        if (buf != nullptr)
        {
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
            /* 图源已换走，旧图现在没人引用了，可以安全释放 */
            if (oldDisplay != nullptr)
            {
                heap_caps_free(oldDisplay);
            }
            return;
        }
    }

    // 立刻先显示默认封面，解码完成后再替换 —— 保证界面永远不等待封面
    /* ⚠ 这一句同时也是「释放旧显示图」的安全前提：
       ui_haibao 的图源在这里被换成内置默认封面，LVGL 从此不再引用
       旧的那块位图；下面才敢把它 free 掉。
       顺序反过来（先 free 再 ShowDefault）就会让 LVGL 画到已释放内存。 */
    AlbumArt_ShowDefault();

    xSemaphoreTake(s_artMutex, portMAX_DELAY);
    /* ══════════════════════════════════════════════════════════════
       【换歌：把上一首的图全部作废】

       这里曾经因为「三个指针互相别名、边判断边置空」而 free 了两次，
       直接踩中 TLSF 的断言：
           assert failed: tlsf_free tlsf.c:630
           (!block_is_free(block) && "block already marked as free")

       根因是所有权不清晰：s_readyBuf / s_cacheBuf / s_displayBuf 都可能是
       同一块内存（解码后 ready==cache，缓存命中后 display==cache），
       于是任何「先置空 A、再用 A 去判断 B」的写法都会失效。

       现在改成**单向所有权**，规则只有两条：
         1) 每一块内存同一时刻只被一个变量「拥有」，释放权跟着拥有者走；
         2) 需要作废时，先把三个槽位整体搬进局部变量并清空全局，
            再对局部变量按地址去重后释放 —— 绝不基于已改动的全局变量判重。
       ══════════════════════════════════════════════════════════════ */
    uint8_t *handles[3] = {s_readyBuf, s_cacheBuf, s_displayBuf};
    s_readyBuf = nullptr;
    s_displayBuf = nullptr;
    /* 缓存只有在「换了一首歌」时才作废（revision 变了）。
       UI 页面重建会把 ui_coverAttemptedRev 清 0，同一个 revision 会再次
       调用本函数 —— 那种情况必须保住缓存，否则缓存就白做了。 */
    const bool keepCache = (revision == s_cacheRevision) && (s_cacheRevision != 0);
    if (!keepCache)
    {
        s_cacheBuf = nullptr;
        s_cacheRevision = 0;
    }
    else
    {
        /* 保住缓存：把它的句柄从待释放名单里摘掉（去重逻辑不必再特判） */
        for (int i = 0; i < 3; i++)
        {
            if (handles[i] == s_cacheBuf)
            {
                handles[i] = nullptr;
            }
        }
    }

    for (int i = 0; i < 3; i++)
    {
        uint8_t *p = handles[i];
        if (p == nullptr)
        {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++)
        {
            if (handles[j] == p)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
        {
            heap_caps_free(p);
        }
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
    uint8_t *oldDisplay = nullptr;
    xSemaphoreTake(s_artMutex, portMAX_DELAY);
    if (s_readyBuf != nullptr)
    {
        buf = s_readyBuf;
        s_readyBuf = nullptr;
        /* ⚠ 这里**只摘指针、不释放**：旧的显示缓冲此刻可能仍被 LVGL
           引用着，要等下面 lv_img_set_src() 把图源换成新图之后才能 free。
           以前在这里就 free 掉了，属于「释放了 LVGL 正在画的内存」。
           另外还要排除「旧显示缓冲 == 新图」的情况（解码完成后
           s_readyBuf 与 s_cacheBuf 是同一块，缓存命中时 s_displayBuf
           又等于它），否则会把马上要显示的新图直接释放掉。 */
        if (s_displayBuf != nullptr && s_displayBuf != buf &&
            s_displayBuf != s_cacheBuf)
        {
            oldDisplay = s_displayBuf;
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

    /* 图源已经换成新图，LVGL 不再引用旧的那块了，现在释放才安全。 */
    if (oldDisplay != nullptr)
    {
        heap_caps_free(oldDisplay);
    }
    return true;
}

void AlbumArt_Release(void)
{
    if (s_artMutex == nullptr)
    {
        return;
    }

    /* 【先把 ui_haibao 切回内置默认封面，再回收内存】
       s_displayBuf 此刻正被 LVGL 通过 s_artDsc 引用着（ui_haibao 的图源）。
       直接 free 就是「释放正在绘制的内存」—— 表现为花屏或 LoadProhibited。
       换成内置的 ui_img_haibao_png 之后，那块内存才真正没人引用，可以安全回收。
       这也是本函数必须由 LVGL 线程（UI_HandleTrackChange）调用的原因：
       换图源和回收必须在同一个线程里成对完成。 */
    if (ui_haibao != nullptr)
    {
        lv_img_set_src(ui_haibao, &ui_img_haibao_png);
        lv_obj_set_size(ui_haibao, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_invalidate(ui_haibao);
    }

    xSemaphoreTake(s_artMutex, portMAX_DELAY);
    /* 换歌时三块内存全部作废。它们可能是同一块（别名），
       所以先整体搬到局部数组、清空全局，再按地址去重后各释放一次 ——
       绝不基于「已经被改动的全局变量」去判重（那样会 double free）。 */
    uint8_t *toBeFreed[3] = {s_readyBuf, s_cacheBuf, s_displayBuf};
    s_readyBuf = nullptr;
    s_cacheBuf = nullptr;
    s_displayBuf = nullptr;
    s_cacheRevision = 0;

    for (int i = 0; i < 3; i++)
    {
        uint8_t *p = toBeFreed[i];
        if (p == nullptr)
        {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++)
        {
            if (toBeFreed[j] == p)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
        {
            heap_caps_free(p);
        }
    }
    xSemaphoreGive(s_artMutex);
    s_reqPending = false;
}
