// 「歌曲信息」弹层的实现 —— 详见 SongInfo.h

#include "SongInfo.h"

#include <Arduino.h>
#include <lvgl.h>

#include "Music.h"
#include "ui.h"

// 面板尺寸：屏幕 240x320，四周各留 8px
#define SI_PANEL_W 224
#define SI_PANEL_H 300

// 关闭按钮
#define SI_BTN_W 54
#define SI_BTN_H 24

// 标题/按钮那一行占的高度（下面的可滚动区要减掉它）
#define SI_HEADER_H 30

static lv_obj_t *s_backdrop = nullptr; // 半透明遮罩：挡住底层交互，点它也能关
static lv_obj_t *s_panel = nullptr;
static lv_obj_t *s_list = nullptr;     // 可滚动的字段容器

/* 值文本用 LV_LABEL_LONG_WRAP：路径/歌名可能很长，截断成 "..." 会让
   这个弹层失去意义（用户就是来看完整路径的）。 */
static void siAddRow(lv_obj_t *parent, const char *name, const String &value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_bottom(row, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 左列：字段名，固定宽度，保证各行的值左对齐
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, 52);
    lv_obj_set_style_text_font(k, FontManager_GetFont(), LV_PART_MAIN);
    lv_obj_set_style_text_color(k, lv_color_hex(0x7FD4FF), LV_PART_MAIN);
    lv_label_set_text(k, name);

    // 右列：值，占满剩余宽度并允许换行
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_font(v, FontManager_GetFont(), LV_PART_MAIN);
    lv_obj_set_style_text_color(v, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v, value.length() > 0 ? value.c_str() : "—");
}

// 把秒数格式化成 mm:ss
static String siFormatDuration(uint32_t sec)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    return String(buf);
}

// 把字节数格式化成人类可读的形式
static String siFormatSize(uint32_t bytes)
{
    char buf[32];
    if (bytes >= 1024u * 1024u)
    {
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
    else if (bytes >= 1024u)
    {
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%u B", (unsigned)bytes);
    }
    return String(buf);
}

// 填充字段内容
static void siFillContent(void)
{
    struct TrackInfo info;
    if (!Music_GetCurrentTrackInfo(&info))
    {
        siAddRow(s_list, "状态", "当前没有正在播放的曲目");
        return;
    }

    siAddRow(s_list, "标题", String(info.title));
    siAddRow(s_list, "艺术家", String(info.artist));
    siAddRow(s_list, "专辑", String(info.album));
    siAddRow(s_list, "作曲家", String(info.composer));

    /* 时长：还没解析出来时显示「未知」而不是 00:00 ——
       刚切歌的那几秒确实还不知道时长，显示 00:00 会被当成「这首歌是空的」。 */
    siAddRow(s_list, "时长", info.durationSec > 0 ? siFormatDuration(info.durationSec) : String("未知"));

    siAddRow(s_list, "文件大小", info.fileSize > 0 ? siFormatSize(info.fileSize) : String("未知"));
    siAddRow(s_list, "文件格式", String(info.format));

    // 采样率：同时给 Hz 和 kHz，前者是技术口径、后者是日常说法
    if (info.sampleRate > 0)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "%u Hz (%.1f kHz)", (unsigned)info.sampleRate,
                 (double)info.sampleRate / 1000.0);
        siAddRow(s_list, "采样率", String(buf));
    }
    else
    {
        siAddRow(s_list, "采样率", "未知");
    }

    siAddRow(s_list, "比特率", info.bitrateKbps > 0 ? String(info.bitrateKbps) + " kbps" : String("未知"));
    siAddRow(s_list, "位深", info.bitsPerSample > 0 ? String(info.bitsPerSample) + " bit" : String("未知"));

    if (info.channels == 1)
    {
        siAddRow(s_list, "声道", "单声道 (1)");
    }
    else if (info.channels == 2)
    {
        siAddRow(s_list, "声道", "立体声 (2)");
    }
    else if (info.channels > 2)
    {
        siAddRow(s_list, "声道", String(info.channels));
    }
    else
    {
        siAddRow(s_list, "声道", "未知");
    }

    siAddRow(s_list, "路径", String(info.path));
}

static void siClose(lv_event_t *e)
{
    (void)e;
    SongInfo_Close();
}

void SongInfo_Close(void)
{
    if (s_backdrop != nullptr)
    {
        /* 异步删除：本函数可能正在某个子对象的点击回调里被调用
           （点关闭按钮、点背景遮罩），同步删除会把事件所属的对象一起释放掉。 */
        lv_obj_del_async(s_backdrop);
        s_backdrop = nullptr;
        s_panel = nullptr;
        s_list = nullptr;
    }
}

bool SongInfo_IsOpen(void)
{
    return s_backdrop != nullptr;
}

void SongInfo_Open(void)
{
    if (s_backdrop != nullptr)
    {
        return; // 已经打开了
    }

    /* 遮罩：铺满全屏、半透明黑。
       加上 CLICKABLE 之后它会吃掉所有点击，底下的按钮就不会被误触；
       同时点遮罩本身也能关闭弹层。 */
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_backdrop, siClose, LV_EVENT_CLICKED, NULL);

    // 面板：居中
    s_panel = lv_obj_create(s_backdrop);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, SI_PANEL_W, SI_PANEL_H);
    lv_obj_center(s_panel);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x021F33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_panel, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x1F6E93), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel, 8, LV_PART_MAIN);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* 面板本身可点但什么也不做：纯粹是为了**吃掉**冒泡到遮罩的点击，
       否则在面板空白处按一下就会把弹层关掉，手感很糟。 */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    // 标题
    lv_obj_t *title = lv_label_create(s_panel);
    lv_obj_set_style_text_font(title, FontManager_GetFont(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(title, "歌曲信息");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // 关闭按钮（右上角）
    lv_obj_t *btn = lv_btn_create(s_panel);
    lv_obj_set_size(btn, SI_BTN_W, SI_BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F6E93), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, siClose, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btnLabel = lv_label_create(btn);
    lv_obj_set_style_text_font(btnLabel, FontManager_GetFont(), LV_PART_MAIN);
    lv_label_set_text(btnLabel, "关闭");
    lv_obj_center(btnLabel);

    /* 可滚动的字段区。
       高度 = 面板高 - 上下内边距 - 标题行。
       滚动条设成 AUTO：内容不长时不显示，长了才出现。 */
    s_list = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), SI_PANEL_H - 8 * 2 - SI_HEADER_H);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1F6E93), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_list, 6, LV_PART_MAIN); // 给滚动条留位置
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    siFillContent();

    /* 内容填完再滚回顶部：LVGL 会保持上一次的滚动位置，
       不重置的话第二次打开会停在半中间，看着像内容缺了一块。 */
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}
