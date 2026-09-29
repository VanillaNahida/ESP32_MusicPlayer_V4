// 触摸校准界面 —— 见 TouchCal.h 顶部的设计说明。

#include "TouchCal.h"

#include <Arduino.h>
#include <lvgl.h>

#include "Touch.h"
#include "ui.h" // ui_font_AlibabaPuHuiTi_12

// 屏幕分辨率（与 main.cpp / Touch.cpp 保持一致）
#define TC_SCR_W 240
#define TC_SCR_H 320

/* 采样目标点相对屏幕边缘的内缩量。
   不要贴着 0/239 采：那一圈常常是触摸屏的「死区」，而且手指也压不到像素边界。
   内缩后按线性外推回四边，结论一样准确。 */
#define TC_MARGIN 20

// 十字标尺寸
#define TC_CROSS 28

// 采样点数：左上、右上、左下、右下
#define TC_POINTS 4

/* 一点的采样要求：
   MIN_SAMPLES  —— 至少采到这么多次原始值才认（约 0.2 秒，主循环 ~5ms 一次）
   RELEASE_CONFIRM —— 连续这么多次读不到触摸才当「松手」，避免抖动被当成松手 */
#define TC_MIN_SAMPLES 5
#define TC_RELEASE_CONFIRM 2

// 结果提示显示时长
#define TC_RESULT_MS 2500

/* LVGL 8 里 LV_PART_* 与 LV_STATE_* 是两个不同的匿名枚举，C++20 下直接 '|'
   会触发 -Wdeprecated-enum-enum-conversion 告警；先转整数再或，编译干净。 */
static const lv_style_selector_t TC_SEL =
    (lv_style_selector_t)((uint32_t)LV_PART_MAIN | (uint32_t)LV_STATE_DEFAULT);

// 四个目标点的屏幕坐标（见 TC_MARGIN 说明）
static const int16_t TC_TGT_X[TC_POINTS] = {
    TC_MARGIN, TC_SCR_W - 1 - TC_MARGIN, TC_MARGIN, TC_SCR_W - 1 - TC_MARGIN};
static const int16_t TC_TGT_Y[TC_POINTS] = {
    TC_MARGIN, TC_MARGIN, TC_SCR_H - 1 - TC_MARGIN, TC_SCR_H - 1 - TC_MARGIN};

enum TcState : uint8_t
{
    TC_IDLE = 0, // 未校准
    TC_WAIT,     // 正在引导用户逐点触摸
    TC_DONE      // 已出结果，等待自动关闭
};

static TcState s_state = TC_IDLE;

// 界面对象
static lv_obj_t *s_backdrop = nullptr;
static lv_obj_t *s_panel = nullptr;
static lv_obj_t *s_hint = nullptr;
static lv_obj_t *s_sub = nullptr;
static lv_obj_t *s_target = nullptr; // 承载十字标的容器，靠移动它来指向下一点

// 采样状态
static int s_idx = 0;                  // 当前是第几点（0~3）
static int32_t s_rawX[TC_POINTS];      // 已采到的原始 x
static int32_t s_rawY[TC_POINTS];      // 已采到的原始 y
static bool s_touching = false;        // 当前是否处于「按住」状态
static int32_t s_accX = 0, s_accY = 0; // 本次按住的累加值（用于取平均抗噪）
static int s_accN = 0;                 // 本次按住的采样次数
static uint8_t s_missCnt = 0;          // 连续「读不到触摸」的次数
static uint32_t s_doneMs = 0;          // 进入 TC_DONE 的时刻

static void tcShowTarget(int idx);
static void tcShowResult(const char *text, uint32_t color);
static void tcClose(void);

/* ---------------- 界面 ---------------- */

static void tcBuildUI(void)
{
    /* 全屏遮罩：可点但没有任何回调，作用是**吃掉**落在它身上的点击，
       这样校准期间的触摸不会误触底下的播放控制按钮。
       （我们的采样走 Touch_ReadRaw()，和这里的点击派发是两条独立路径。） */
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_backdrop, 0, 0);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(0x000000), TC_SEL);
    lv_obj_set_style_bg_opa(s_backdrop, 150, TC_SEL);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);

    /* 提示面板放屏幕正中：四个目标点在四角，中间是空的，
       面板放这里既不会挡住十字标，也不会被手指到处碰。 */
    s_panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 224, 66);
    lv_obj_set_pos(s_panel, 8, 127);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x0B283D), TC_SEL);
    lv_obj_set_style_bg_opa(s_panel, 240, TC_SEL);
    lv_obj_set_style_radius(s_panel, 8, TC_SEL);
    lv_obj_set_style_border_width(s_panel, 1, TC_SEL);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x1F6E93), TC_SEL);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_hint = lv_label_create(s_panel);
    lv_obj_set_width(s_hint, 208);
    lv_obj_set_pos(s_hint, 8, 6);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, TC_SEL);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFFFFF), TC_SEL);
    lv_obj_set_style_text_font(s_hint, FontManager_GetFont(), TC_SEL);

    s_sub = lv_label_create(s_panel);
    lv_obj_set_width(s_sub, 208);
    lv_obj_set_pos(s_sub, 8, 32);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, TC_SEL);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0x8FB8CE), TC_SEL);
    lv_obj_set_style_text_font(s_sub, FontManager_GetFont(), TC_SEL);

    // 十字标：一个容器 + 横线 + 竖线 + 中心点，定位时整体移动容器即可
    s_target = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_target);
    lv_obj_set_size(s_target, TC_CROSS, TC_CROSS);
    lv_obj_clear_flag(s_target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_target, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_obj_create(s_target);
    lv_obj_remove_style_all(h);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(h, TC_CROSS, 3);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(0xFF4040), TC_SEL);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, TC_SEL);

    lv_obj_t *v = lv_obj_create(s_target);
    lv_obj_remove_style_all(v);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(v, 3, TC_CROSS);
    lv_obj_align(v, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(0xFF4040), TC_SEL);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, TC_SEL);

    lv_obj_t *c = lv_obj_create(s_target);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c, 7, 7);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, TC_SEL);
    lv_obj_set_style_bg_color(c, lv_color_hex(0xFFFFFF), TC_SEL);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, TC_SEL);
}

static void tcShowTarget(int idx)
{
    if (s_target != nullptr)
    {
        lv_obj_clear_flag(s_target, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_target, TC_TGT_X[idx] - TC_CROSS / 2, TC_TGT_Y[idx] - TC_CROSS / 2);
    }
    if (s_hint != nullptr)
    {
        lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFFFFF), TC_SEL);
        lv_label_set_text_fmt(s_hint, "请触摸十字中心 (%d/%d)", idx + 1, TC_POINTS);
    }
    if (s_sub != nullptr)
    {
        lv_label_set_text(s_sub, "按住约 0.2 秒后松手");
    }
}

// 显示结果（成功/失败），隐藏十字标，进入自动关闭倒计时
static void tcShowResult(const char *text, uint32_t color)
{
    if (s_target != nullptr)
    {
        lv_obj_add_flag(s_target, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_hint != nullptr)
    {
        lv_obj_set_style_text_color(s_hint, lv_color_hex(color), TC_SEL);
        lv_label_set_text(s_hint, text);
    }
    if (s_sub != nullptr)
    {
        lv_label_set_text(s_sub, "");
    }
    s_state = TC_DONE;
    s_doneMs = millis();
}

static void tcClose(void)
{
    if (s_backdrop != nullptr)
    {
        /* 异步删除：本函数可能在 LVGL 事件处理路径上被间接调用，
           同步删掉正在派发事件的对象会崩。 */
        lv_obj_del_async(s_backdrop);
        s_backdrop = nullptr;
    }
    s_panel = nullptr;
    s_hint = nullptr;
    s_sub = nullptr;
    s_target = nullptr;
    s_state = TC_IDLE;
}

/* ---------------- 标定换算 ---------------- */

/* 线性外推：已知屏幕坐标 x1/x2 处的原始值 r1/r2，求 target 处的原始值。
   之所以要外推，是因为采样点刻意内缩了（见 TC_MARGIN），
   而标定值要求的是「屏幕 0 和 239/319 处的原始值」。 */
static int32_t tcRawAtScreen(int32_t r1, int32_t r2, int32_t x1, int32_t x2, int32_t targetSec)
{
    const int32_t denom = x2 - x1;
    if (denom == 0)
    {
        return r1;
    }
    const int64_t num = (int64_t)(r2 - r1) * (targetSec - x1);
    return (int32_t)(r1 + num / denom);
}

// 四点采完：换算 → 校验 → 应用 → 存 NVS
static void tcFinish(void)
{
    const int32_t xl = tcRawAtScreen(s_rawX[0], s_rawX[1], TC_TGT_X[0], TC_TGT_X[1], 0);
    const int32_t xr = tcRawAtScreen(s_rawX[0], s_rawX[1], TC_TGT_X[0], TC_TGT_X[1], TC_SCR_W - 1);
    const int32_t yt = tcRawAtScreen(s_rawY[0], s_rawY[2], TC_TGT_Y[0], TC_TGT_Y[2], 0);
    const int32_t yb = tcRawAtScreen(s_rawY[0], s_rawY[2], TC_TGT_Y[0], TC_TGT_Y[2], TC_SCR_H - 1);

    Serial.printf("[TCAL] 采样: (%d,%d)(%d,%d)(%d,%d)(%d,%d)\n",
                  (int)s_rawX[0], (int)s_rawY[0], (int)s_rawX[1], (int)s_rawY[1],
                  (int)s_rawX[2], (int)s_rawY[2], (int)s_rawX[3], (int)s_rawY[3]);
    Serial.printf("[TCAL] 换算: x=%d..%d y=%d..%d\n",
                  (int)xl, (int)xr, (int)yt, (int)yb);

    // Touch_SetCalibration() 内部会再做一次范围/跨度校验
    if (!Touch_SetCalibration(xl, xr, yt, yb))
    {
        Serial.println("[TCAL] 校准失败：采样值异常，未保存");
        tcShowResult("校准失败：采样异常，请重试", 0xFF8080);
        return;
    }

    if (Touch_SaveCalibration())
    {
        Serial.println("[TCAL] 校准完成，已保存到 NVS 并立即生效");
        tcShowResult("校准完成，已保存", 0x7FE08A);
    }
    else
    {
        // 已生效但没落盘：本次能用，重启后失效 —— 如实告知，别报成完全成功
        Serial.println("[TCAL] 校准已生效，但保存到 NVS 失败");
        tcShowResult("已生效，但保存失败", 0xFFD070);
    }
}

/* ---------------- 对外接口 ---------------- */

void TouchCal_Start(void)
{
    if (s_state != TC_IDLE)
    {
        Serial.println("[TCAL] 校准已在进行中（串口 'x' 可取消）");
        return;
    }

    s_idx = 0;
    s_touching = false;
    s_accX = 0;
    s_accY = 0;
    s_accN = 0;
    s_missCnt = 0;

    tcBuildUI();
    tcShowTarget(0);
    s_state = TC_WAIT;

    Serial.println("[TCAL] 触摸校准开始：请依次触摸屏幕上 4 个十字标记的中心");
    Serial.println("[TCAL] 每一点按住约 0.2 秒后松手；串口 'x' 取消");
}

void TouchCal_Cancel(void)
{
    if (s_state == TC_IDLE)
    {
        Serial.println("[TCAL] 当前没有正在进行的校准");
        return;
    }
    tcClose();
    Serial.println("[TCAL] 校准已取消");
}

bool TouchCal_IsActive(void)
{
    return s_state != TC_IDLE;
}

void TouchCal_Poll(void)
{
    if (s_state == TC_IDLE)
    {
        return;
    }

    const uint32_t now = millis();

    if (s_state == TC_DONE)
    {
        if ((now - s_doneMs) >= TC_RESULT_MS)
        {
            tcClose();
        }
        return;
    }

    int16_t rx = 0, ry = 0, rz = 0;
    const bool hit = Touch_ReadRaw(&rx, &ry, &rz);

    if (!s_touching)
    {
        // 等待按下
        if (hit)
        {
            s_touching = true;
            s_missCnt = 0;
            s_accX = rx;
            s_accY = ry;
            s_accN = 1;
            if (s_sub != nullptr)
            {
                lv_label_set_text(s_sub, "按住不动，松手确认这一点");
            }
        }
        return;
    }

    if (hit)
    {
        // 按住中：持续累加，松手时取平均以抵抗单点噪声
        s_missCnt = 0;
        s_accX += rx;
        s_accY += ry;
        s_accN++;
        return;
    }

    /* 可能松手了。必须连续确认若干次才当真的松手 ——
       否则 XPT2046 的偶发空读会被当成松手，导致提前结算一个坏点。 */
    s_missCnt++;
    if (s_missCnt < TC_RELEASE_CONFIRM)
    {
        return;
    }

    s_touching = false;

    if (s_accN < TC_MIN_SAMPLES)
    {
        Serial.printf("[TCAL] 第 %d 点采样太少(%d)，请重按\n", s_idx + 1, s_accN);
        tcShowTarget(s_idx); // 同一点重新采
        return;
    }

    s_rawX[s_idx] = s_accX / s_accN;
    s_rawY[s_idx] = s_accY / s_accN;
    Serial.printf("[TCAL] 第 %d 点 raw=(%d,%d) 样本=%d\n",
                  s_idx + 1, (int)s_rawX[s_idx], (int)s_rawY[s_idx], s_accN);

    s_idx++;
    if (s_idx >= TC_POINTS)
    {
        tcFinish();
        return;
    }
    tcShowTarget(s_idx);
}