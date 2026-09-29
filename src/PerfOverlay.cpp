// 性能浮窗（帧率 / CPU / 内存）实现
//
// 详见 PerfOverlay.h 的说明。这里只讲两个实现上的选择：
//
// 1) 为什么用 lv_timer 而不是在 PerfOverlay_Loop() 里直接刷新文字？
//    刷新文字要用 lv_label_set_text_fmt()，它会分配内存、还会触发重绘。
//    浮窗本身又在测量帧率 —— 如果每帧都刷新，就成了「观测行为影响被观测
//    对象」。所以固定每 500ms 更新一次，采样窗口足够长，数字才有意义。
//
// 2) 为什么帧率要自己数，而不是读 LVGL 的 mon.fps？
//    LVGL 8.3 的 lv_mem_monitor_t 里**没有** fps 字段（那是 9.x 才有的），
//    8.3 只提供 lv_refr_get_fps_avg()。这里两个都打：refr 的是 LVGL 自己
//    统计的「刷新帧率」，而我们另外数 lv_timer_handler() 的调用次数，
//    后者能反映主循环是否被别的东西拖慢。

#include "PerfOverlay.h"

// ── 编译期开关：platformio.ini 里 -DENABLE_PERF_OVERLAY=1 打开 ──
// 未定义时默认 0（关闭），这样不加任何编译选项也不会引入开销。
#ifndef ENABLE_PERF_OVERLAY
#define ENABLE_PERF_OVERLAY 0
#endif

#if ENABLE_PERF_OVERLAY

#include <Arduino.h>
#include <lvgl.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 字体来自 ui 模块（ui_font_AlibabaPuHuiTi12.c），必须包含 ui.h 才能拿到
   ui_font_AlibabaPuHuiTi_12 的声明 —— 否则报
   'ui_font_AlibabaPuHuiTi_12' was not declared in this scope。 */
#include "ui.h"

// 刷新间隔：太短会干扰测量，太长又看不出瞬时变化
#define PERF_REFRESH_MS 500

static lv_obj_t *s_panel = nullptr;
static lv_obj_t *s_label = nullptr;

// 采样状态
static uint32_t s_lastRefreshMs = 0;
static uint32_t s_loopSeq = 0;        // 主循环圈数（PerfOverlay_Loop 被调用次数）
static uint32_t s_lastLoopSeq = 0;    // 上次刷新时的圈数
static uint32_t s_loopHz = 0;         // 主循环频率（次/秒）
static uint32_t s_frameCount = 0;     // 累计刷新帧数（由 flush 包装器递增）
static uint32_t s_lastFrameCount = 0; // 上次刷新时的帧数
static uint32_t s_fps = 0;            // 实测帧率

/* ==================================================================
   【帧率到底怎么测】

   LVGL 8.3 里现成的 lv_refr_get_fps_avg() 被 #if LV_USE_PERF_MONITOR
   包着，而本项目 lv_conf.h 里 LV_USE_PERF_MONITOR = 0（LVGL 自带那块
   性能面板没开），所以那个符号压根不存在，直接调用会编译失败。
   为了一个调试浮窗去改 lv_conf.h 并不划算（还会多一个常驻面板）。

   自己数反而更准、也更贴合本项目：
   LVGL 每完成一块区域的渲染就会调用显示驱动的 flush_cb，
   「flush 被调用的次数」就是真正的刷新次数。
   这里把注册好的 flush_cb 包一层，先计数再转调原函数 ——
   对原有刷屏逻辑零侵入，面板关掉时这段代码根本不存在。

   ⚠ 必须转调原回调：漏掉就会出现「浮窗一开、屏幕全黑」。
   ================================================================== */
typedef void (*PerfFlushCb)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
static PerfFlushCb s_origFlush = nullptr;

static void perfFlushWrapper(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    s_frameCount++;
    if (s_origFlush != nullptr)
    {
        s_origFlush(drv, area, px);
    }
}

void PerfOverlay_Init(void)
{
    if (s_panel != nullptr)
    {
        return;
    }

    /* 挂在 top layer 上，切屏不丢。

       【位置：必须避开左上角的返回按钮】
       主界面的返回图标在 ui_TopPanel（高 25px）里，SquareLine 把它放在
       x = -100、垂直居中，也就是大约 x 6~22 / y 6~18；
       而 ui_Screen1.c 里又给它加了 9px 的扩大命中区（方便手指点），
       实际可点范围约为 x 0~31 / y 0~27。
       播放列表页自己的「返回」按钮是 x=3 / y=3 / 宽 54 / 高 22，
       同样压在左上角。

       所以浮窗不能待在最上面。这里把它**整体下移到顶栏之下**：
         y = 30 → 位于 25px 顶栏 + 一点间隙的下面，两个页面的返回按钮都碰不到。

       【尺寸：按中文字体的实际度量算，不要凭感觉给】
       字体用的是 ui_font_AlibabaPuHuiTi_12，它的
           line_height = 15   （不是字号 12！）
       6 行文本 = 6*15 + 5*行距(1) = 95
       加 padding 4*2                = 103
       所以面板高必须 >= 103。取 106 留 3px 余量。
       ⚠ 之前按 montserrat_10（line_height = 11）算出来是 79，
         换成中文字体后如果还沿用手算的老值，最后一行会被裁掉。
         改字体或改行数时，务必重新按 line_height 算一遍。

       宽度同理：最宽的标签「内部堆」是 3 个汉字 = 36px，
       值最宽形如 "1234K" 约 40px，再加标签与值之间的间隙和左右 padding，
       取 112 足够，且 2+112=114 远小于屏宽 240。 */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 112, 106);
    lv_obj_align(s_panel, LV_ALIGN_TOP_LEFT, 2, 30);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_panel, 4, 0);
    lv_obj_set_style_pad_all(s_panel, 4, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_IGNORE_LAYOUT);

    s_label = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x00FF88), 0);
    /* 中文字体（ui_font_AlibabaPuHuiTi_12）。
       它只有 12px 高，但 line_height 是 15，行与行之间自带 3px 空隙，
       所以 text_line_space 用 0 就够了，再加行距会显得松散。 */
    lv_obj_set_style_text_font(s_label, FontManager_GetFont(), 0);
    lv_obj_set_style_text_line_space(s_label, 0, 0);
    lv_label_set_text(s_label, "正在采样...");

    /* 给显示驱动的 flush_cb 套一层计数包装器（见上面 perfFlushWrapper 的说明）。
       这里先申请再挂上，避免中途被 lv_timer_handler 观察到半成品状态。 */
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != nullptr && disp->driver != nullptr)
    {
        s_origFlush = (PerfFlushCb)disp->driver->flush_cb;
        disp->driver->flush_cb = perfFlushWrapper;
    }

    s_lastRefreshMs = millis();
    s_lastLoopSeq = 0;
    s_lastFrameCount = s_frameCount;
}

void PerfOverlay_Loop(void)
{
    if (s_panel == nullptr)
    {
        return; // 还没初始化
    }

    // 先累加圈数：即使还没到刷新时刻，计数也必须继续
    s_loopSeq++;

    const uint32_t now = millis();
    const uint32_t dt = now - s_lastRefreshMs;
    if (dt < PERF_REFRESH_MS)
    {
        return;
    }
    s_lastRefreshMs = now;

    const uint32_t fps = s_fps;

    /* ── 主循环频率 ──
       PerfOverlay_Loop() 由主循环每圈调用一次，所以「圈数 / 时间」
       就是主循环频率。正常 ~200Hz（loop 里 delay(5)）；
       掉到几十 Hz 说明主循环里有地方在阻塞。 */
    const uint32_t seq = s_loopSeq;
    s_loopHz = (dt > 0) ? (uint32_t)((uint64_t)(seq - s_lastLoopSeq) * 1000ULL / dt) : 0;
    s_lastLoopSeq = seq;

    /* ── 帧率 ──
       两次采样之间的 flush 次数 / 时间。
       ⚠ 界面完全静止时 LVGL 不重绘，FPS 会显示 0 —— 这是正常的，
       不代表卡顿；要看卡不卡应该同时看 Loop 那一行。 */
    const uint32_t frames = s_frameCount;
    s_fps = (dt > 0) ? (uint32_t)((uint64_t)(frames - s_lastFrameCount) * 1000ULL / dt) : 0;
    s_lastFrameCount = frames;

    // ── 内存 ──
    /* 内部堆单独看：esp_get_free_heap_size() 把 8MB PSRAM 也算进去，
       总数永远很宽裕，真正会卡死的是内部堆（fopen 分不到小块就 abort）。
       这个数字掉到几十 KB 就该警惕了。 */
    const uint32_t heapInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t heapPsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t stackFree = (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    /* lv_mem 的使用率：这是 LVGL 自己的内存池（现在在 PSRAM 里），
       对象/样式都从这里分配。用满会直接分配失败。 */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    /* 用「标签 + 值」两列排版，每行一个指标。
       为什么不用一长串字符串：中文字宽是数字的两倍，混排时如果不用
       等宽标签列，右边的数字会参差不齐，很难一眼对比。
       这里把 6 个指标各放一行，标签列固定宽度自然对齐。 */
    lv_label_set_text_fmt(s_label,
                          "FPS:   %ufps\n"
                          "主循环频率:   %uHz\n"
                          "内部堆栈: %uK\n"
                          "PSRAM:  %uK\n"
                          "LVGL内存: %u%%\n"
                          "栈余:   %u",
                          (unsigned)s_fps,
                          (unsigned)s_loopHz,
                          (unsigned)(heapInternal / 1024),
                          (unsigned)(heapPsram / 1024),
                          (unsigned)mon.used_pct,
                          (unsigned)stackFree);
}

bool PerfOverlay_IsEnabled(void)
{
    return true;
}

#else // ENABLE_PERF_OVERLAY

// 关闭时全部编译成空函数：不占 Flash，也不会有未使用变量的告警
void PerfOverlay_Init(void) {}
void PerfOverlay_Loop(void) {}
bool PerfOverlay_IsEnabled(void) { return false; }

#endif // ENABLE_PERF_OVERLAY
