// 播放列表重建工具（BOOT 键触发 + 询问弹窗 + 扫描进度显示）
//
// 设计要点：
//  1. BOOT 键（GPIO0）在主循环里轮询，带消抖；只在「按下沿」触发一次弹窗。
//  2. 扫描在**主循环里分片推进**（Music_RescanStep），不是阻塞调用 ——
//     否则 lv_timer_handler() 跑不起来，进度条根本画不出来，界面看着像死了。
//  3. 所有耗时动作（开始扫描、放弃扫描、写列表）都不在 LVGL 事件回调里做，
//     而是先置一个「待办」标志，回到 PlaylistTool_Poll() 再执行。
//     原因：事件回调是在 lv_timer_handler() 内部跑的，在里面停播放/写 SD
//     会把整帧刷新拖住，而且容易踩到「事件执行中删除对象」的坑。
//  4. 弹窗挂在 lv_layer_top() 上，因此在「播放列表页 / 主界面」之间切换屏幕
//     时不会被连带删掉（lv_disp_load_scr 不会动 top layer）。
//  5. 扫描期间音频是停掉的（由 Music_RescanBegin 负责）：
//     解码任务和扫描器抢同一张 SD 卡的同一条 SPI 总线，
//     不停播放的话扫描会慢到没法用，声音也会碎成一片。

#include "PlaylistTool.h"

#include <Arduino.h>
#include <lvgl.h>

#include "ui.h" // ui_font_AlibabaPuHuiTi_12 / UI_NotifyScreenRebuilt
#include "Music.h"

/* BOOT 键 = GPIO0。板上一般已有外部上拉，这里再开内部上拉兜底。
   注意 GPIO0 是 strapping 引脚：复位瞬间的电平决定启动模式，
   但运行期间当普通输入读是安全的。 */
#define BOOT_KEY_PIN 0
#define BOOT_DEBOUNCE_MS 40

// 单次扫描分片的时间预算：太小则进度刷新多、扫描慢；太大则界面发滞。
#define SCAN_STEP_BUDGET_MS 25

// 重建结果提示的显示时长
#define RESULT_SHOW_MS 2500

/* LVGL 8 里 LV_PART_* 与 LV_STATE_* 是两个不同的匿名枚举，
   在 C++20 下直接 '|' 会触发 -Wdeprecated-enum-enum-conversion 告警。
   先把两边各自转成整数再或，最后转回 lv_style_selector_t，编译就干净了。
   后面所有样式调用统一用这两个常量。
   （SquareLine 生成的那些文件里同样的写法仍会告警，那是既有代码，不动它。） */
static const lv_style_selector_t PT_SEL_MAIN =
    (lv_style_selector_t)((uint32_t)LV_PART_MAIN | (uint32_t)LV_STATE_DEFAULT);
static const lv_style_selector_t PT_SEL_IND =
    (lv_style_selector_t)((uint32_t)LV_PART_INDICATOR | (uint32_t)LV_STATE_DEFAULT);
static const lv_style_selector_t PT_SEL_KNOB =
    (lv_style_selector_t)((uint32_t)LV_PART_KNOB | (uint32_t)LV_STATE_DEFAULT);

enum PtState : uint8_t
{
    PT_IDLE = 0,
    PT_ASK,      // 正在询问是否重建
    PT_SCANNING, // 正在重建
    PT_RESULT    // 重建结束，显示结果
};

static PtState s_state = PT_IDLE;

static lv_obj_t *s_backdrop = nullptr; // 全屏遮罩：吞掉弹窗外的点击
static lv_obj_t *s_dlg = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_body = nullptr;
static lv_obj_t *s_spin = nullptr; // 环形无限旋转的进度指示器
static lv_obj_t *s_pct = nullptr;
static lv_obj_t *s_path = nullptr;
static lv_obj_t *s_btnCancel = nullptr;

static bool s_keyDown = false;
static uint32_t s_keyChangeMs = 0;
static uint32_t s_resultMs = 0;

// 「待办」标志：由事件回调置位，由 Poll 执行真正的重活
static bool s_pendingStart = false;
static bool s_pendingAbort = false;

// true = 正在做「首次使用扫描」而不是 BOOT 重建（只影响标题/结果文案）
static bool s_firstScan = false;

// 只在内容真的变化时才 set_text，避免每 5ms 重新分配字符串 + invalidate
static int s_lastPct = -1;
static String s_lastPath = "";

/* ---------------- 弹窗骨架 ---------------- */

static void ptDestroyDialog(void)
{
    if (s_dlg != nullptr)
    {
        /* 必须用异步删除：本函数经常是在「弹窗内按钮的 CLICKED 回调」里
           被间接调用的，同步 lv_obj_del() 会把正在派发事件的对象直接释放掉。 */
        lv_obj_del_async(s_dlg);
    }
    s_dlg = nullptr;
    if (s_backdrop != nullptr)
    {
        lv_obj_del_async(s_backdrop);
    }
    s_backdrop = nullptr;
    s_title = nullptr;
    s_body = nullptr;
    s_spin = nullptr;
    s_pct = nullptr;
    s_path = nullptr;
    s_btnCancel = nullptr;
    s_lastPct = -1;
    s_lastPath = "";
}

static void ptMakeButton(lv_obj_t *parent, const char *text, int x, int y,
                         lv_event_cb_t cb, lv_obj_t **out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 88, 34);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 6, PT_SEL_MAIN);
    lv_obj_set_style_border_width(btn, 0, PT_SEL_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F5C7A), PT_SEL_MAIN);
    lv_obj_set_style_bg_opa(btn, 255, PT_SEL_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &ui_font_AlibabaPuHuiTi_12, PT_SEL_MAIN);
    lv_obj_center(label);

    if (out != nullptr)
    {
        *out = btn;
    }
}

// 建立弹窗外框（各模式共用）
static void ptBuildDialogFrame(const char *title)
{
    ptDestroyDialog();

    /* 全屏遮罩：必须建在弹窗**之前**（后建的在上层）。
       它是 CLICKABLE 但没有任何事件回调，因此会吃掉落在它身上的点击，
       底下的播放控制按钮在弹窗期间就点不到了 —— 否则用户在扫描时
       乱点「下一曲」，会和正在被重建的 musicFiles 数组打架。 */
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_backdrop, 0, 0);
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_backdrop, 0, PT_SEL_MAIN);
    lv_obj_set_style_border_width(s_backdrop, 0, PT_SEL_MAIN);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(0x000000), PT_SEL_MAIN);
    lv_obj_set_style_bg_opa(s_backdrop, 120, PT_SEL_MAIN);

    s_dlg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dlg, 230, 168);
    lv_obj_center(s_dlg);
    lv_obj_clear_flag(s_dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_dlg, 10, PT_SEL_MAIN);
    lv_obj_set_style_border_width(s_dlg, 0, PT_SEL_MAIN);
    lv_obj_set_style_bg_color(s_dlg, lv_color_hex(0x0B283D), PT_SEL_MAIN);
    lv_obj_set_style_bg_opa(s_dlg, 245, PT_SEL_MAIN);
    lv_obj_set_style_pad_all(s_dlg, 0, PT_SEL_MAIN);

    s_title = lv_label_create(s_dlg);
    lv_obj_set_width(s_title, 210);
    lv_obj_set_pos(s_title, 10, 8);
    lv_label_set_text(s_title, title);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, PT_SEL_MAIN);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), PT_SEL_MAIN);
    lv_obj_set_style_text_font(s_title, &ui_font_AlibabaPuHuiTi_12, PT_SEL_MAIN);
}

static void ptAddBody(const char *text, lv_text_align_t align, int y)
{
    s_body = lv_label_create(s_dlg);
    lv_obj_set_width(s_body, 206);
    lv_obj_set_pos(s_body, 12, y);
    lv_label_set_text(s_body, text);
    lv_obj_set_style_text_align(s_body, align, PT_SEL_MAIN);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xCFE3EF), PT_SEL_MAIN);
    lv_obj_set_style_text_font(s_body, &ui_font_AlibabaPuHuiTi_12, PT_SEL_MAIN);
}

/* ---------------- 询问模式 ---------------- */

static void ptCbRebuild(lv_event_t *e);
static void ptCbCancel(lv_event_t *e);

static void ptShowAsk(void)
{
    ptBuildDialogFrame("重建播放列表？");
    s_state = PT_ASK;

    ptAddBody("将重新扫描存储卡上的全部音乐文件，\n"
              "并把曲目内嵌歌词导出到 /lrc/，\n"
              "完成后覆盖现有播放列表。",
              LV_TEXT_ALIGN_LEFT, 32);

    ptMakeButton(s_dlg, "取消", 16, 122, ptCbCancel, nullptr);
    ptMakeButton(s_dlg, "重建", 126, 122, ptCbRebuild, nullptr);

    Serial.println("[PLTOOL] BOOT 键按下：询问是否重建播放列表");
}

/* ---------------- 扫描进度模式 ---------------- */

/* 只搭界面，真正开工留给 Poll（见文件头第 3 条）。
   showCancel=false 时不画「取消」按钮 —— 首次使用扫描是必须走完的，
   给一个点了没用的按钮只会让人以为程序坏了。 */
static void ptBuildScanUI(const char *title, bool showCancel)
{
    ptBuildDialogFrame(title);

    /* 环形无限旋转的进度指示器（lv_spinner = 一段持续旋转的圆弧）。
       为什么用「无限旋转」而不是线性进度条：
       扫描的百分比本身是**估算值** —— 目录枚举没有固定的总数分母，
       进度只能按「已访问目录 / (已访问+待访问)」推。画成一根有终点的
       进度条会让人盯着数字较真（为什么卡在 60% 不动？）；
       转圈表达的是「正在干活」，配上百分比文字既诚实又直观。

       旋转动画由 lv_spinner 内部创建（LV_ANIM_REPEAT_INFINITE），
       由 lv_timer_handler() 驱动，这里不需要自己推进。 */
    s_spin = lv_spinner_create(s_dlg, 1000, 60); // 1 秒转一圈，指示弧 60°
    lv_obj_set_size(s_spin, 44, 44);
    lv_obj_set_pos(s_spin, (230 - 44) / 2, 28);

    // 去掉圆弧默认附带的「旋钮」圆点，只留一圈干净的环。
    // 注意：knob 的样式是**主题**加上去的（lv_theme_default 会给 lv_arc 挂
    // styles->knob：实心圆 + 主色填充），所以光调 lv_obj_remove_style(obj,NULL,
    // LV_PART_KNOB) 不够 —— 那只删局部样式。最稳的是直接把它的填充设成透明。
    lv_obj_remove_style(s_spin, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_spin, LV_OPA_TRANSP, PT_SEL_KNOB);
    lv_obj_set_style_border_width(s_spin, 0, PT_SEL_KNOB);
    lv_obj_set_style_shadow_width(s_spin, 0, PT_SEL_KNOB);

    // 底环（暗）与旋转弧（亮）
    lv_obj_set_style_arc_width(s_spin, 5, PT_SEL_MAIN);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x123A52), PT_SEL_MAIN);
    lv_obj_set_style_arc_width(s_spin, 5, PT_SEL_IND);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x35C1F1), PT_SEL_IND);
    lv_obj_set_style_arc_rounded(s_spin, true, PT_SEL_IND);

    // 「正在扫描 36%」
    s_pct = lv_label_create(s_dlg);
    lv_obj_set_width(s_pct, 210);
    lv_obj_set_pos(s_pct, 10, 78);
    lv_label_set_text(s_pct, "正在扫描...");
    lv_obj_set_style_text_align(s_pct, LV_TEXT_ALIGN_CENTER, PT_SEL_MAIN);
    lv_obj_set_style_text_color(s_pct, lv_color_hex(0xFFFFFF), PT_SEL_MAIN);
    lv_obj_set_style_text_font(s_pct, &ui_font_AlibabaPuHuiTi_12, PT_SEL_MAIN);

    // 当前正在处理的文件路径（过长时循环滚动）
    s_path = lv_label_create(s_dlg);
    lv_obj_set_width(s_path, 206);
    lv_obj_set_pos(s_path, 12, 100);
    lv_label_set_long_mode(s_path, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_path, "/");
    lv_obj_set_style_text_align(s_path, LV_TEXT_ALIGN_LEFT, PT_SEL_MAIN);
    lv_obj_set_style_text_color(s_path, lv_color_hex(0x8FB8CE), PT_SEL_MAIN);
    lv_obj_set_style_text_font(s_path, &ui_font_AlibabaPuHuiTi_12, PT_SEL_MAIN);

    if (showCancel)
    {
        ptMakeButton(s_dlg, "取消", 71, 122, ptCbCancel, &s_btnCancel);
    }

    s_lastPct = -1;
    s_lastPath = "";
}

static void ptUpdateProgress(void)
{
    /* 环形指示器是无限旋转的，不需要（也无法）按百分比设值，
       这里只更新数字与路径。 */
    int pct = Music_RescanPercent();
    if (s_pct != nullptr && pct != s_lastPct)
    {
        s_lastPct = pct;
        lv_label_set_text_fmt(s_pct, "正在扫描...", pct);
    }

    const char *path = Music_RescanCurrentPath();
    if (s_path != nullptr && path != nullptr && s_lastPath != path)
    {
        s_lastPath = path;
        lv_label_set_text(s_path, path);
    }
}

/* ---------------- 结果模式 ---------------- */

static void ptEnterResultMode(void)
{
    bool ok = Music_RescanEnd();
    int found = Music_RescanFoundCount();

    /* 三种结果分开写，别把「卡里没有音乐」也报成「保存失败」——
       首次使用遇到空卡是很正常的情况，报错会让人以为程序坏了。 */
    const char *title;
    const char *line1;
    if (found <= 0)
    {
        title = "未找到音乐";
        line1 = "存储卡里没有可播放的音乐文件";
    }
    else if (ok)
    {
        title = s_firstScan ? "扫描完成" : "重建完成";
        line1 = s_firstScan ? "播放列表已建立" : "播放列表已保存";
    }
    else
    {
        title = s_firstScan ? "扫描失败" : "重建失败";
        line1 = "播放列表保存失败";
    }

    ptBuildDialogFrame(title);

    char buf[128];
    lv_snprintf(buf, sizeof(buf), "%s\n共 %d 首歌曲\n\n歌词会在播放时自动读取/生成",
                line1, found);
    ptAddBody(buf, LV_TEXT_ALIGN_CENTER, 40);

    ptMakeButton(s_dlg, "知道了", 71, 122, ptCbCancel, nullptr);

    s_resultMs = millis();
    s_state = PT_RESULT;

    // 主界面需要重新拉取曲目信息 / 歌词 / 封面
    UI_NotifyScreenRebuilt();
}

/* ---------------- 事件回调（只置标志，不干重活） ---------------- */

static void ptCbCancel(lv_event_t *e)
{
    (void)e;
    if (s_state == PT_SCANNING)
    {
        s_pendingAbort = true; // 真正的放弃动作在 Poll 里做
        return;
    }
    ptDestroyDialog();
    s_state = PT_IDLE;
    Serial.println("[PLTOOL] 已取消");
}

static void ptCbRebuild(lv_event_t *e)
{
    (void)e;
    s_firstScan = false;
    ptBuildScanUI("正在重建播放列表", true);
    s_state = PT_SCANNING;
    s_pendingStart = true; // 真正的开扫在 Poll 里做
}

/* ---------------- 对外接口 ---------------- */

void PlaylistTool_Init(void)
{
    pinMode(BOOT_KEY_PIN, INPUT_PULLUP);
    s_keyDown = (digitalRead(BOOT_KEY_PIN) == LOW);
    s_keyChangeMs = millis();
    s_state = PT_IDLE;
    s_pendingStart = false;
    s_pendingAbort = false;
    Serial.printf("[PLTOOL] BOOT 键就绪: GPIO%d（按下为低电平），按一下可重建播放列表\n",
                  BOOT_KEY_PIN);
}

bool PlaylistTool_IsBusy(void)
{
    return s_state == PT_SCANNING;
}

void PlaylistTool_Poll(void)
{
    uint32_t now = millis();

    /* 1) BOOT 按键：消抖 + 只在「按下沿」动作一次 */
    bool down = (digitalRead(BOOT_KEY_PIN) == LOW);
    if (down != s_keyDown && (now - s_keyChangeMs) >= BOOT_DEBOUNCE_MS)
    {
        s_keyChangeMs = now;
        s_keyDown = down;
        if (down)
        {
            if (s_state == PT_IDLE)
            {
                ptShowAsk();
            }
            else if (s_state == PT_RESULT)
            {
                ptDestroyDialog();
                s_state = PT_IDLE;
            }
            // PT_ASK / PT_SCANNING 期间忽略，避免误触打断
        }
    }

    /* 2) 执行事件回调里排下的重活 */
    if (s_pendingAbort)
    {
        s_pendingAbort = false;
        Music_RescanAbort();
        ptDestroyDialog();
        s_state = PT_IDLE;
        UI_NotifyScreenRebuilt();
        Serial.println("[PLTOOL] 已取消重建");
        return;
    }

    if (s_pendingStart)
    {
        s_pendingStart = false;
        // 内部会先停掉播放，再准备扫描队列（歌词不在这里生成，留到播放时）
        Music_RescanBegin();
        ptUpdateProgress();
        return;
    }

    /* 3) 扫描推进：每次只跑一小段，保证 lv_timer_handler() 有机会跑 */
    if (s_state == PT_SCANNING)
    {
        bool done = Music_RescanStep(SCAN_STEP_BUDGET_MS);
        ptUpdateProgress();
        if (done)
        {
            ptEnterResultMode();
        }
    }

    /* 4) 结果提示自动关闭 */
    if (s_state == PT_RESULT && (now - s_resultMs) >= RESULT_SHOW_MS)
    {
        ptDestroyDialog();
        s_state = PT_IDLE;
    }
}

/* ==================================================================
   首次使用扫描

   和按 BOOT 重建走的是**同一套**代码和**同一个**进度界面
   （同一份 ptBuildScanUI / ptUpdateProgress），区别只有两处文案。

   与 BOOT 那条路的唯一结构差异：这里是开机时在 setup() 里同步跑完的，
   所以循环里要自己调 lv_timer_handler() —— 否则进度条只是改了数值，
   永远画不到屏上。所幸 setup() 里显示驱动和 UI 都已经初始化完毕。
   ================================================================== */
void PlaylistTool_RunFirstScan(void)
{
    Serial.println("[PLTOOL] 首次使用：开始扫描存储卡（带进度界面）");

    s_firstScan = true;
    s_state = PT_SCANNING;
    ptBuildScanUI("首次使用：扫描存储卡", false); // 必须扫完，不给「取消」
    lv_timer_handler();                          // 先把界面画出来

    Music_RescanBegin();
    ptUpdateProgress();

    uint32_t lastUiMs = 0;
    uint32_t scanStart = millis();
    while (!Music_RescanStep(SCAN_STEP_BUDGET_MS))
    {
        /* 刷新进度条。
           按时间节流到 ~30Hz：lv_label_set_text 会重新分配字符串并 invalidate，
           每一小片扫描都刷一遍会造成没必要的 LVGL 堆抖动。 */
        uint32_t now = millis();
        if (now - lastUiMs >= 30)
        {
            lastUiMs = now;
            ptUpdateProgress();
            lv_timer_handler();
        }
    }

    ptUpdateProgress();
    lv_timer_handler();

    Serial.printf("[PLTOOL] 首次扫描完成：%d 首歌曲，耗时 %u ms\n",
                  Music_RescanFoundCount(), (unsigned)(millis() - scanStart));

    // 收尾：写二进制播放列表、复位索引，并显示「扫描完成」
    ptEnterResultMode();
    lv_timer_handler();
    // 结果弹窗留给主循环的 PlaylistTool_Poll() 在 RESULT_SHOW_MS 后自动关闭
}
