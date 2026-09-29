// SD 卡热插拔检测 + 浮窗提醒实现
//
// 状态机：
//   SDH_READY   —— 卡在槽里且可读。定期探测，连续失败 N 次 → 判定拔出。
//   SDH_ABSENT  —— 没有卡 / 卡不可用。定期尝试重连（SD.end()+begin()）。
//   SDH_SCANNING —— 重连成功但没有播放列表缓存，正在分片重建列表。
//
// 探测策略（详见 Music.cpp 的 Music_ProbeStorage 注释）：
//   用「打开根目录」这种真实 FATFS 读来判断卡在不在——SD.cardType() 只返回
//   缓存值，不能反映运行时的拔插，所以这里不用它。
//
// 为什么「重连」用 SD.end()+begin()：
//   SDFS::begin() 有 `_pdrv != 0xFF` 的守卫，同一实例不会重新 mount；必须
//   先 end() 释放掉旧挂载，begin() 才会真正去初始化新插入的卡。
//
// 浮窗挂 lv_layer_top()，切屏（主界面 ↔ 播放列表页）不会把它连带删掉；
// 和 PlaylistTool 的弹窗各自独立，互不影响。

#include "SdHotplug.h"

#include <Arduino.h>
#include <lvgl.h>

#include "ui.h"        // ui_font_AlibabaPuHuiTi_12 / UI_NotifyScreenRebuilt
#include "Music.h"
#include "PlaylistTool.h"

/* ---------- 可调参数 ---------- */

// 探测间隔：拔插检测的采样周期。
// 太快会频繁读卡占 SPI；太慢会让拔卡/插卡响应迟钝。
#define PROBE_INTERVAL_MS 500

// 判定「卡被拔出」需要的连续失败次数（去抖）。
// 读卡偶尔会因为和刷屏抢总线而瞬时失败，直接失败一次就判定拔出会误报。
#define REMOVE_CONFIRM_COUNT 2

// 卡缺失状态下，尝试「重连（SD.end+begin）」的间隔。
// 空槽重连会真的走一遍 SD 初始化序列（要持 SPI 总线锁），所以放慢些。
#define REINIT_INTERVAL_MS 2000

// 单次分片扫描的时间预算（重连后重建播放列表用），与 PlaylistTool 一致。
#define SCAN_STEP_BUDGET_MS 25

// 浮窗显示时长
#define TOAST_SHOW_MS 2500

/* ---------- 状态 ---------- */

enum SdHotplugState : uint8_t
{
    SDH_READY = 0,   // 卡可读
    SDH_ABSENT = 1,  // 没有可用卡
    SDH_SCANNING = 2 // 重连成功、正在重建播放列表
};

static SdHotplugState s_state = SDH_READY;
static bool s_inited = false;      // SdHotplug_Init 是否已调用
static uint32_t s_lastProbeMs = 0; // 上一次探测/决策时刻
static uint32_t s_failCount = 0;   // 连续探测失败次数

/* LVGL 浮窗控件 */
static lv_obj_t *s_toast = nullptr;
static lv_obj_t *s_toastLabel = nullptr;
static uint32_t s_toastUntilMs = 0;

/* LVGL 8 里 LV_PART_MAIN / LV_STATE_DEFAULT 是不同匿名枚举，C++20 下直接
   '|' 会触发 -Wdeprecated-enum-enum-conversion 告警（和 PlaylistTool 一样）。 */
static const lv_style_selector_t TD_SEL_MAIN =
    (lv_style_selector_t)((uint32_t)LV_PART_MAIN | (uint32_t)LV_STATE_DEFAULT);

/* ---------- 浮窗 ---------- */

void SdHotplug_ShowToast(const char *text)
{
    if (!s_inited)
    {
        return; // LVGL 尚未就绪（SdHotplug_Init 之前）
    }

    /* 覆盖旧浮窗：先异步删掉，再新建。用 lv_obj_del_async 而不是 del，
       避免在事件/绘制途中释放正在被 LVGL 使用的对象。 */
    if (s_toast != nullptr)
    {
        lv_obj_del_async(s_toast);
        s_toast = nullptr;
        s_toastLabel = nullptr;
    }

    /* 顶部一条半透明的圆角小条。挂在 top layer，切屏不丢。 */
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, 196, 34);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x001018), TD_SEL_MAIN);
    lv_obj_set_style_bg_opa(toast, (lv_opa_t)217, TD_SEL_MAIN); /* ≈ 85% */
    lv_obj_set_style_radius(toast, 8, TD_SEL_MAIN);
    lv_obj_set_style_border_width(toast, 0, TD_SEL_MAIN);
    lv_obj_set_style_shadow_width(toast, 4, TD_SEL_MAIN);
    lv_obj_set_style_shadow_color(toast, lv_color_hex(0x000000), TD_SEL_MAIN);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), TD_SEL_MAIN);
    lv_obj_set_style_text_font(label, FontManager_GetFont(), TD_SEL_MAIN);
    lv_obj_center(label);

    s_toast = toast;
    s_toastLabel = label;
    s_toastUntilMs = millis() + TOAST_SHOW_MS;

    Serial.printf("[SD] 浮窗：%s\n", text);
}

static void toastAutoDismiss(uint32_t now)
{
    if (s_toast != nullptr && now >= s_toastUntilMs)
    {
        lv_obj_del_async(s_toast);
        s_toast = nullptr;
        s_toastLabel = nullptr;
    }
}

/* ---------- 重连后的列表装载 ---------- */

// 有缓存：直接载入，返回 true。
// 无缓存：进入 SDH_SCANNING，返回 false（之后由 Poll 分片扫描）。
static bool tryLoadPlaylistOrScan(void)
{
    if (Music_LoadPlaylistCache())
    {
        return true;
    }

    Serial.println("[SD] 重连成功但没有播放列表缓存，开始重建");
    Music_RescanBegin(); // 内部会停播放、清空列表、准备扫描队列
    s_state = SDH_SCANNING;
    return false;
}

// 列表就绪后：把当前曲目指向第一首，并让主界面整体刷新。
static void playlistReady(void)
{
    music_i = 0;
    music_prev_i = -1;
    UI_NotifyScreenRebuilt();

    // 有歌就自动从第一首开始播，给用户明确的插卡成功反馈。
    if (fileCount > 0)
    {
        Music_PlayPath(musicFiles[0].c_str());
    }
    Serial.printf("[SD] 插卡完成：播放列表 %d 首\n", fileCount);
}

/* ---------- 对外接口 ---------- */

void SdHotplug_Init(void)
{
    s_inited = true;
    s_lastProbeMs = millis();
    s_failCount = 0;

    if (!Music_IsStorageReady())
    {
        s_state = SDH_ABSENT;
        // 开机就没有可用卡：先弹「未插入」，之后每 REINIT_INTERVAL_MS 尝试重连。
        SdHotplug_ShowToast("SD卡未插入");
        Serial.println("[SD] 开机未检测到 SD 卡，进入插卡等待");
    }
    else
    {
        s_state = SDH_READY;
    }
}

void SdHotplug_Poll(void)
{
    if (!s_inited)
    {
        return;
    }

    uint32_t now = millis();
    toastAutoDismiss(now);

    // 重建播放列表（BOOT 键触发的那个）正在进行时，别在这探测/清列表，
    // 两边会抢 musicFiles 数组和 SD 卡。等它扫完再说。
    if (PlaylistTool_IsBusy())
    {
        return;
    }

    /* ---- SDH_SCANNING：分片推进重连后的列表重建 ---- */
    if (s_state == SDH_SCANNING)
    {
        if (Music_RescanStep(SCAN_STEP_BUDGET_MS))
        {
            Music_RescanEnd();
            s_state = SDH_READY;
            playlistReady();
        }
        return; // 扫描期间不并行探测，等扫完
    }

    /* 每个状态用各自的采样间隔：
       READY   → PROBE_INTERVAL_MS（读卡探测拔卡）
       ABSENT  → REINIT_INTERVAL_MS（间隔较长，避免空槽重连刷屏卡顿） */
    uint32_t interval = (s_state == SDH_READY) ? PROBE_INTERVAL_MS : REINIT_INTERVAL_MS;
    if ((uint32_t)(now - s_lastProbeMs) < interval)
    {
        return; // 节流：还没到下一个采样点
    }
    s_lastProbeMs = now;

    if (s_state == SDH_READY)
    {
        /* ---- 卡可读状态：探测是否被拔出 ---- */
        bool ok = Music_ProbeStorage();
        if (ok)
        {
            s_failCount = 0;
        }
        else
        {
            s_failCount++;
            if (s_failCount >= REMOVE_CONFIRM_COUNT)
            {
                s_failCount = 0;
                s_state = SDH_ABSENT;
                Music_HandleCardRemoved();
                UI_NotifyScreenRebuilt();
                SdHotplug_ShowToast("SD卡已拔出");
            }
        }
    }
    else // SDH_ABSENT
    {
        /* ---- 没有卡：定期尝试重连 ---- */
        if (Music_ReinitStorage())
        {
            // 重连成功：装列表。没有缓存则进入 SDH_SCANNING。
            if (tryLoadPlaylistOrScan())
            {
                s_state = SDH_READY;
                playlistReady();
            }
            SdHotplug_ShowToast("SD卡已插入");
        }
        // 失败则保持 SDH_ABSENT，等下一个周期再试。
    }
}
