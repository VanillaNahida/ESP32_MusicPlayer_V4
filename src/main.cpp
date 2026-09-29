#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <Music.h>
#include "AlbumArt.h"
#include "AudioBusLock.h"
#include "Touch.h"
#include "TouchCal.h"
#include "PlaylistTool.h"
#include "PerfOverlay.h"
#include "SdHotplug.h"
#include "FontManager.h"
#include "AudioProfile.h"
#include "esp_heap_caps.h"

/*函数声明*/
void UI_update();
void UI_CheckAudioTaskAlive();

/* 串口调试/校准命令（在 loop() 里处理）：
     t = 打印当前标定与一次实测换算（诊断触摸「错位」）
     c = 开始触摸校准：屏幕依次显示 4 个十字标记，逐个触摸其中心，
         采到的原始值会外推成四边标定值，立即生效并保存到 NVS
     x = 取消正在进行的校准

   为什么校准入口放在串口而不是屏幕上的按钮：标定不准时按屏幕按钮会
   点不中它（「用错的标定去校准标定」的死锁）。详见 src/TouchCal.h。

   音量记忆：调音量时由 setVolume() 实时写入 NVS，开机由
   Music_LoadVolume() 恢复，与 SD 卡无关（见 lib/Music/Music.cpp）。 */

/*Don't forget to set Sketchbook location in File/Preferences to the path of your UI project (the parent foder of this INO file)*/

/*Change to your screen resolution*/
static const uint16_t screenWidth = 240;
static const uint16_t screenHeight = 320;

// LVGL 绘制缓冲?/4 屏。缓冲越大，单次能渲染的区域越大、刷屏次数越少，
// SPI 事务开销（startWrite/setAddrWindow/endWrite）随之下降，界面更跟手?
// 注意必须放在内部 RAM：TFT_eSPI ?DMA 不能?PSRAM 取数据?
#define LV_DRAW_BUF_PIXELS (screenWidth * screenHeight / 4)

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[LV_DRAW_BUF_PIXELS];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); /* TFT instance */

#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char *buf)
{
  Serial.printf(buf);
  Serial.flush();
}
#endif

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  // TFT 是直接写 SPI 寄存器（绕过 IDF 驱动、也不持?HAL 总线锁）?
  // ?SD 卡读卡与触摸读数都在这条总线上，必须互斥，否则并发会?SPI 事务错乱
  AudioBusLock();
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  AudioBusUnlock();

  lv_disp_flush_ready(disp);
}

// 触摸读取 / 校准 / 坐标标定都在 Touch.cpp 里实现，
// 那里同时记录了这?SPI 总线踩过的坑（不要传 TOUCH_IRQ、不要用 readData?
// 不要?map() 做坐标换算等），改之前务必先读一遍?
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  Touch_Read(indev_driver, data);
}

// 多任务处理部?，内?只处理音频播放，内核1处理歌词显示
TaskHandle_t audioTaskHandle = NULL;

// 音频播放任务
void audioTask(void *parameter)
{
  while (true)
  {
    Music_Loop(); // 音频循环处理

    vTaskDelay(2 / portTICK_PERIOD_MS); // 释放 CPU 时间?
  }
}

void setup()
{
  Serial.begin(115200); /* prepare for possible serial debug */

  // ==================================================================
  // 【重要】把 CPU 主频锁死在当前值，禁止之后发生任何频率切换?
  //
  // 原因（这是触摸时好时?按下去没反应的真正根因）?
  //   本工程里 TFT_eSPI ?*直接读写 SPI 寄存?*刷屏的，不经?IDF ?SPI
  //   驱动，因此它也不持有 HAL 的总线锁，SPI 时钟分频寄存器完全靠库自己维护?
  //   ?Arduino core 里任何一次主频切换（setCpuFrequencyMhz，典型触发者是
  //   调制解调器睡?/ 省电管理会把 CPU 降到 80MHz）都会走 APB 变更回调?
  //   回调里做的事情是“重?SPI 分频寄存器并**直接写回**”：
  //       spi->dev->clock.val = spiFrequencyToClockDiv(spi, ...);
  //   这个写回既不是原子的，也完全不经?SPI 总线锁?
  //   于是：core1 正在刷屏（TFT 正在?SPI 寄存器）时，core0 上的频率切换
  //   把分频值改?—?刷屏那一次事务的位长?时钟随之错乱?
  //   轻则这一帧输出错位，重则 SPI 外设状态机卡住，TFT 停摆、触摸读数全废?
  //   频率切回来时又会再写一次，所以症状表现为「随机、时好时坏」?
  //
  //   把主频钉死之后就不会再有 APB 变更回调，SPI 分频寄存器只?TFT_eSPI
  //   自己管理，配?AudioBusLock 的总线互斥，三者（TFT/SD/触摸）才能真正稳定共存?
  // ==================================================================
  setCpuFrequencyMhz(getCpuFrequencyMhz());

  // SPI 总线互斥锁必须先于任何刷?读卡建立
  AudioBusInit();

  // 背光：先?TFT 初始化把 GPIO 设为输出并点?
  // （Arduino core 3.x ?TFT_eSPI 库内部的 pinMode/digitalWrite 可能因引脚被抢占而失效）
#if defined(TFT_BL)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  Serial.printf("背光: GPIO%d 已点亮\n", TFT_BL);
#endif

  // 关键顺序：先由 TFT_eSPI 初始化 SPI2 总线，SD 再复用同一条总线。
  // 反之（SD 先初始化）会导致 TFT 重新初始化 SPI2 主机、SD 读取全部失效 —— 表现为能加载播放列表却无法解码 MP3。
  tft.begin();        /* TFT init */
  tft.setRotation(0); /* Landscape orientation, flipped */

  // 触摸屏：初始化并应用 Touch.cpp 里的实测标定值
  Touch_Init();

  /* 音频/存储初始化（复用上面已初始化的 SPI 总线） */
  Music_Init();

  // 音频初始化完成（请求通道已就绪）后再启动解码任务，内?
  xTaskCreatePinnedToCore(audioTask, "audioTask", 16384, NULL, configMAX_PRIORITIES - 1, &audioTaskHandle, 0);

  delay(1000);

  String LVGL_Arduino = "Hello Arduino! ";
  LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

  Serial.println(LVGL_Arduino);
  Serial.println("I am LVGL_Arduino");

  /* LVGL 的内存池必须落在 PSRAM，否则它会占掉内部 RAM 的 110KB，
     而内部堆一旦只剩几 KB，newlib 的 fopen() 分不到 FILE 结构体和递归锁
     就会直接 abort()（点歌 → savePlayState() → SD.open() 崩在
     lock_init_generic，整个芯片复位）。这里把「池子到底从哪个堆拿的」
     打出来，避免以后再靠猜。详见 lv_conf.h 里 LV_MEM_POOL_ALLOC 的说明。 */
  uint32_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  lv_init();

  uint32_t psramAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  Serial.printf("[MEM] LVGL 内存池 %u 字节 -> PSRAM 减少 %u, 内部堆减少 %u | 现在 内部=%u PSRAM=%u\n",
                (unsigned)LV_MEM_SIZE,
                (unsigned)(psramBefore - psramAfter),
                (unsigned)(internalBefore - internalAfter),
                (unsigned)internalAfter, (unsigned)psramAfter);
  if ((internalBefore - internalAfter) >= LV_MEM_SIZE / 2)
  {
    Serial.println("[MEM] !! 内存池落在了内部 RAM（PSRAM 没拿到），内部堆会很紧张！");
  }

#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print); /* register print function for debugging */
#endif

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, LV_DRAW_BUF_PIXELS);

  /*Initialize the display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  /*Change the following line to your display resolution*/
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  /*Initialize the (dummy) input device driver*/
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  /* SD 卡字体（可选，见 include/FontManager.h）：
     注册 LVGL 的文件系统驱动，并尝试把 <sd>/fonts/*.bin 读进 PSRAM。
     成功就用它（全量 CJK 覆盖），失败/没插卡就回退内置字体，不影响启动。
     ⚠ 必须在 ui_init() **之前**：界面里所有 set_text_font 都取这里的指针，
       晚于 ui_init 的话界面已经用内置字体创建好了。 */
  FontManager_Init();

  ui_init();

  /* 性能浮窗：必须在 ui_init() 之后 —— 它要往 lv_layer_top() 上挂控件，
     而 top layer 是 lv_init() 时就建好、ui_init() 之后才用得上。
     编译期开关见 PerfOverlay.h（-DENABLE_PERF_OVERLAY=1）。 */
  PerfOverlay_Init();

  // 上电直接进主界面，不再强制进入校准界面。
  // 触屏标定值写死在 Touch.cpp 顶部（沿用项目原本实测可用的那组），
  // 因此不存在「标定页依赖触摸、触摸不准就进不去」的死锁。

  // 启动专辑封面后台解码任务（低优先级，解码完成后由 AlbumArt_Poll 换上?
  AlbumArt_Init();

  // BOOT 键：按一下询问是否重建播放列表（带扫描进度显示）
  PlaylistTool_Init();

  /* SD 卡热插拔检测 + 浮窗提醒。
     必须在 UI（lv_layer_top）就绪之后初始化：它可能要立刻弹「SD卡未插入」的浮窗，
     也要能随运行中拔卡/插卡给出「已拔出/已插入」提醒。 */
  SdHotplug_Init();

  /* ==================================================================
     播放列表：先试缓存，没有再带进度界面扫描

     为什么扫描放在这里、而不是 Music_Init() 里：
     首次使用（卡里没有 music_playlist.bin）要全卡扫描，而扫描必须让用户
     看到进度条。进度界面依赖 LVGL，可 Music_Init() 是在 lv_init()/ui_init()
     **之前**调用的 —— 那时候还没有任何显示能力，只能干等一大片黑屏。
     挪到这里之后，「首次使用扫描」和「按 BOOT 重建」共用同一套扫描代码
     和同一个进度界面（PlaylistTool_RunFirstScan 内部就是那套）。
     ================================================================== */
  if (!Music_IsStorageReady())
  {
    Serial.println("[SETUP] SD 卡不可用，跳过播放列表加载与扫描");
  }
  else if (!Music_LoadPlaylistCache())
  {
    // 没有缓存 / 缓存无效 → 首次使用，带进度界面扫描整张卡
    PlaylistTool_RunFirstScan();
  }

  // 恢复上次播放状态（UI初始化后?
  Music_RestorePlayState();

  Serial.println("Setup done");

  /*初始化后更新页面显示信息*/
  lv_label_set_text(ui_MusicTitleLabel, currentTitle.c_str());   //  歌曲?
  lv_label_set_text(ui_MusicArtistLabel, currentArtist.c_str()); //  歌手?
  if (lrc_flag)
  {
    lv_label_set_text(ui_MusicLrcLabel, "暂无歌词"); //  加载歌词
  }
  else
  {
    lv_label_set_text(ui_MusicLrcLabel, "暂无歌词"); //  加载歌词
  }
  //  显示播放时间+进度条
  //  【必须用 Music_GetDuration()】duration 由解码任务(core0)写、UI(core1)读，
  //  直接读那个裸全局变量会因为缺 volatile 而读到永不更新的陈旧值，
  //  表现为「进度条不动、时长一直是 00:00」。
  long dur = Music_GetDuration();
  /* 拖动进度条期间**不要**刷新进度和时间：
     否则解码进度会每 5ms 把用户拖到的位置覆盖掉，
     表现为"手指滑到 2:30，进度条却顽固地弹回 1:05"。
     拖动时的显示由 ui_event_ProgressBar 自己负责。 */
  if (dur != 0 && !ProgressBar_IsDragging())
  {
    uint32_t cur = Music_GetCurrentPlayTime();

    lv_label_set_text_fmt(ui_MusicTimeLabel1, "%02d:%02d", (int)(cur / 60), (int)(cur % 60));
    lv_label_set_text_fmt(ui_MusicTimeLabel2, "%02d:%02d", (int)(dur / 60), (int)(dur % 60));

    lv_bar_set_value(ui_Bar1, map(cur, 0, dur, 0, 100), LV_ANIM_OFF);
  }
}


void loop()
{
  // 串口命令：诊断 + 触摸校准
  while (Serial.available() > 0)
  {
    int ch = Serial.read();
    if (ch == 't' || ch == 'T')
    {
      // 诊断：打印当前标定 + 一次实测换算，用来判断错位原因
      Touch_DumpDebug();
    }
    else if (ch == 'c' || ch == 'C')
    {
      // 开始触摸校准（屏幕显示十字标，逐个触摸）
      TouchCal_Start();
    }
    else if (ch == 'x' || ch == 'X')
    {
      // 取消正在进行的校准
      TouchCal_Cancel();
    }
    else if (ch == '\r' || ch == '\n')
    {
      // 忽略行尾
    }
  }

  lv_timer_handler(); /* let the GUI do its work */
  PerfOverlay_Loop(); // 性能浮窗（-DENABLE_PERF_OVERLAY=1 时才真正干活）
  PlaylistTool_Poll(); // BOOT 键 + 重建进度（内部按小片推进扫描，不阻塞）
  SdHotplug_Poll();    // SD 卡热插拔：拔卡/插卡自动识别 + 浮窗提醒
  TouchCal_Poll();     // 触摸校准采样状态机（未校准时立即返回）

  /* 重建期间跳过主界面刷新：
     此刻 musicFiles 数组正在被重新分配、fileCount 会在 0 和实际值之间跳，
     UI_update() 里的「换歌检测」和歌词解析都不该在这时候跑。
     进度弹窗自己挂在 top layer 上，不受影响。 */
  if (!PlaylistTool_IsBusy())
  {
    UI_update(); // 更新UI
  }
  UI_CheckAudioTaskAlive(); // 解码任务卡死检测（见下）
  /* 音频链路性能埋点打印（-DENABLE_AUDIO_PROFILE=1 时才真正干活）。
     刻意放在主循环里、而不是音频任务里：Serial.printf 一行约 13ms，
     在音频任务里打印会自己制造欠载，测量结果失真。内部按 1 秒节流。 */
  AudioProfile_Print();
  delay(5);
}
// ===== 界面刷新状?=====
static bool play_executed = false;  // 播放执行标志?
static bool pause_executed = false; // 暂停执行标志?
static String shownTitle = "";      // 当前已显示的标题
static String shownArtist = "";     // 当前已显示的歌手
static uint32_t ui_coverAttemptedRev = 0; // 已尝试过解码的封面版本（含失败，避免每帧重试?
static bool ui_coverShownDefault = false; // 当前显示的是否为默认封面
static bool ui_needsRefresh = false;      // 主界面被重建后置位，需要整体重?
static uint32_t ui_lastShownSec = 0xFFFFFFFF; // 上次已显示到界面上的秒数
static bool ui_lrcPlaceholderShown = false;   // 是否已显示过“暂无歌词”占位文?
static bool ui_lrcEmbeddedChecked = false;    // 本曲是否已补查过内嵌歌词

// 主界面被销毁后重新创建时调用（?lib/ui/ui.h 声明?
void UI_NotifyScreenRebuilt(void)
{
    ui_needsRefresh = true;
}

/* ==================================================================
   解码任务「卡死」检测

   为什么需要它：
   界面上所有音频操作（播放/暂停、上一首/下一首、音量、列表选曲）
   最终都只是给解码任务发表请求，真正把请求变成声音的是 core0 上的
   audioTask → Music_Loop()。它是这些请求**唯一**的消费点。
   一旦这个任务卡在某处不返回，用户看到的现象就是「点了完全没反应」，
   而 UI 侧（触摸、按钮、标签）看起来一切正常，非常容易误判成
   「事件没绑定」或「触摸坏了」—— 之前就是这样白查了很久。

   Music_GetLoopSeq() 每跑一轮 Music_Loop() 就自增一次，
   正常情况下 5ms 内必然变化。这里只要发现它长时间不动，
   就直接把结论打到串口，省掉下一次的重复排查。
   ================================================================== */
void UI_CheckAudioTaskAlive()
{
    static uint32_t lastSeq = 0;
    static uint32_t lastChangeMs = 0;
    static bool warned = false;

    uint32_t seq = Music_GetLoopSeq();
    uint32_t now = millis();

    if (seq != lastSeq)
    {
        lastSeq = seq;
        lastChangeMs = now;
        warned = false;
        return;
    }

    if (!warned && (now - lastChangeMs) > 3000)
    {
        warned = true;
        Serial.println("[UI] 警告：解码任务已 3 秒无响应（Music_Loop 没有推进）。");
        Serial.println("[UI]       此时界面上所有播放相关操作都不会生效，问题在解码任务侧，");
        Serial.println("[UI]       请查看上面是否有 [AUDIO] 警告：单次解码阻塞 ... 的打印。");
    }
}

// 专辑封面：立即显示默认封面，内嵌封面由后台任务解码完成后再换上?
// 解码（尤?PNG）耗时可达数百毫秒，绝不能放在这里同步做，否则会阻?UI 与触摸?
static void UI_UpdateAlbumArt(void)
{
    if (ui_haibao == nullptr)
    {
        return;
    }

    // 后台是否已解码好新封?
    AlbumArt_Poll();

    const uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t rev = 0;
    bool hasArt = Music_GetAlbumCover(&data, &size, &rev);

    if (!hasArt || rev == 0)
    {
        // 当前曲目没有内嵌封面 ?默认封面（只在状态变化时设置一次，避免每帧重绘?
        if (!ui_coverShownDefault)
        {
            ui_coverShownDefault = true;
            ui_coverAttemptedRev = 0;
            AlbumArt_ShowDefault();
        }
        return;
    }

    // 每个封面版本只请求一次解码（失败也不会反复重试）
    if (rev == ui_coverAttemptedRev)
    {
        return;
    }
    ui_coverAttemptedRev = rev;
    ui_coverShownDefault = false;
    // 立刻切默认封?+ 投递后台解码请求（本函数立即返回）
    AlbumArt_Request(rev);
}

// 主界面重建后按当前播放状态整体刷新一?
static void UI_ApplyCurrentState(void)
{
    if (ui_Screen1 == nullptr)
    {
        return;
    }

    Music_info();

    lv_label_set_text(ui_MusicTitleLabel, currentTitle.c_str());
    lv_label_set_text(ui_MusicArtistLabel, currentArtist.c_str());
    lv_label_set_long_mode(ui_MusicTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_long_mode(ui_MusicLrcLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    shownTitle = currentTitle;
    shownArtist = currentArtist;

    lv_label_set_text(ui_MusicLrcLabel, lrc_flag ? "" : "暂无歌词");

    // 【必须用 Music_GetDuration()】跨核读取，见 UI_ApplyCurrentState() 中的说明
    long dur = Music_GetDuration();
    if (dur > 0)
    {
        long cur = (long)Music_GetCurrentPlayTime();
        lv_label_set_text_fmt(ui_MusicTimeLabel1, "%02d:%02d", (int)(cur / 60), (int)(cur % 60));
        lv_label_set_text_fmt(ui_MusicTimeLabel2, "%02d:%02d", (int)(dur / 60), (int)(dur % 60));
        lv_bar_set_value(ui_Bar1, map(cur, 0, dur, 0, 100), LV_ANIM_OFF);
    }
    else
    {
        /* 时长还没解析出来（刚切歌 / 正在装载）。
           这里必须把时间和进度清零：留着上一首的数字会让人以为界面没更新。 */
        lv_label_set_text(ui_MusicTimeLabel1, "00:00");
        lv_label_set_text(ui_MusicTimeLabel2, "00:00");
        lv_bar_set_value(ui_Bar1, 0, LV_ANIM_OFF);
    }

    if (Music_IsPlaying())
    {
        _ui_flag_modify(ui_PlayButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
        _ui_flag_modify(ui_PauseButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
        lv_label_set_text(ui_Label2, "正在播放");
    }
    else
    {
        _ui_flag_modify(ui_PauseButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);
        _ui_flag_modify(ui_PlayButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
        /* 没在播放时要区分两种情况，别一律写成「暂停播放」：
             · pause_status==1 → 用户自己按的暂停
             · 否则           → 正在装载（或刚播完），对用户来说就是「正在加载」
           写错会让用户以为程序卡住了 —— 明明刚点了歌，却显示暂停。 */
        lv_label_set_text(ui_Label2, pause_status ? "暂停播放" : "正在加载?..");
    }

    UI_UpdateAlbumArt();
}

/* ==================================================================
   换曲目：**立刻**把界面上的所有文字切成新曲目的内容

   为什么要单独抽出来、而且放在播放状态判断之外：
   原来这段逻辑写在 `if (Music_IsPlaying())` 里面，而「用户刚选了歌」
   的那一刻恰恰是**没在播放**的（解码器正在装载）。于是标题、歌手、时间、
   进度、歌词全都要等装载完、真正开始播之后才更新 ——
   用户看到的就是「列表里选了曲，界面上有部分文字还是旧的」。

   现在改成：只要检测到曲目索引变化就立刻刷新，不管后台在干什么。
   这也是「UI 不能被后台阻塞」的一部分 —— 用户点了，界面就必须有反应。
   ================================================================== */
static void UI_HandleTrackChange(void)
{
    if (music_prev_i == music_i)
    {
        return;
    }
    music_prev_i = music_i;

    if (fileCount <= 0 || music_i < 0 || music_i >= fileCount)
    {
        return;
    }

    durationPrinted = false;
    ui_lastShownSec = 0xFFFFFFFF;
    ui_lrcPlaceholderShown = false;
    ui_lrcEmbeddedChecked = false; // 新曲目：还没等过它的内嵌歌词

    /* 1) 标题/歌手立刻换成新曲目。
        Music_ResetTrackInfo() 清掉上一首的元数据，Music_info() 随即用
        文件名兜底 —— 即使新曲目没有标签，界面也不会继续挂着上一首的歌名。 */
    Music_ResetTrackInfo();
    Music_info();
    shownTitle = currentTitle;
    shownArtist = currentArtist;
    lv_label_set_text(ui_MusicTitleLabel, currentTitle.c_str());
    lv_label_set_text(ui_MusicArtistLabel, currentArtist.c_str());
    lv_label_set_long_mode(ui_MusicTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* 2) 时间与进度清零。
        不清的话会一直显示上一首的 01:25 和进度，直到新时长解析出来为止。 */
    lv_label_set_text(ui_MusicTimeLabel1, "00:00");
    lv_label_set_text(ui_MusicTimeLabel2, "00:00");
    lv_bar_set_value(ui_Bar1, 0, LV_ANIM_OFF);

    /* 3) 歌词：此刻能查到的只有「同名 .lrc」和 /lrc/ 下已生成的文件；
        曲目内嵌歌词要等解码任务解析完，由 UI_update() 里的补查兜底。 */
    parseLrcFile(musicFiles[music_i]);
    lv_label_set_text(ui_MusicLrcLabel, lrc_flag ? "" : "暂无歌词");
    ui_lrcPlaceholderShown = !lrc_flag;

    /* 4) 顶栏立刻给出反馈，让用户知道「点到了」 */
    lv_label_set_text(ui_Label2, "正在加载?..");
    Serial.printf("[UI] 切到第 %d 首: %s\n", music_i + 1, musicFiles[music_i].c_str());

    /* 5) 换歌了：作废上一首的封面缓存。
        必须在这里做，而不是在 Music.cpp 的 audioDoLoad() 里 ——
        那里跑在解码任务（core0）上，而 s_displayBuf 正被 core1 的 LVGL
        用来绘制，跨核释放会画出已释放的内存。
        本函数在 LVGL 线程里执行，且「每换一首歌恰好走一次」，
        正是释放旧图、让下一首重新解码的正确时机。
        （封面版本号 revision 本身也会变，所以即使漏了这一步，
          AlbumArt_Request 也会按 revision 判断而不会显示错图；
          这里主动释放只是为了及时回收那 72KB，不让它滞留到下一首。） */
    AlbumArt_Release();
}

void UI_update()
{
    // 播放列表页打开期间主界面已被销毁，跳过所有主界面控件的更?
    if (ui_Screen1 == nullptr)
    {
        return;
    }

    // 换曲目优先处理：它会把标题/歌手/时间/进度/歌词一次性设成新曲目的值
    UI_HandleTrackChange();

    // 主界面被重建（从播放列表页返回）?整体重新应用一次状?
    if (ui_needsRefresh)
    {
        ui_needsRefresh = false;
        play_executed = false;
        pause_executed = false;
        shownTitle = "";
        shownArtist = "";
        /* 注意：这里**不要**把 ui_coverAttemptedRev 清成 0 当作「强制重新解码」。
           页面重建后 ui_haibao 是一个全新的空控件，确实需要重新把封面挂上去，
           但「重新挂」不等于「重新解码」——
           AlbumArt_Request() 内部按 revision 命中缓存，瞬间就能出图。
           清成 0 只是让 UI_UpdateAlbumArt() 愿意再调一次 Request，是必须的；
           真正避免重复解码的是 AlbumArt.cpp 里的缓存。 */
        ui_coverAttemptedRev = 0;
        ui_coverShownDefault = false;
        ui_lastShownSec = 0xFFFFFFFF;
        ui_lrcPlaceholderShown = false;
        ui_lrcEmbeddedChecked = false;
        UI_ApplyCurrentState();
    }

    // 应用解码器异步回调传来的曲目信息（标?歌手），仅在变化时刷新标签，避免打断滚动动画
    Music_info();
    if (currentTitle != shownTitle)
    {
        shownTitle = currentTitle;
        lv_label_set_text(ui_MusicTitleLabel, currentTitle.c_str());
        lv_label_set_long_mode(ui_MusicTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    if (currentArtist != shownArtist)
    {
        shownArtist = currentArtist;
        lv_label_set_text(ui_MusicArtistLabel, currentArtist.c_str());
    }

    // 专辑封面（ID3 内嵌优先，无则默认封面）
    UI_UpdateAlbumArt();

    if (Music_IsPlaying())
    {
        /* UI显示曲目名称，歌手，歌词，显示播放进度，播放时间，总时长，播放状?*/
        if (!play_executed)
        {
            _ui_flag_modify(ui_PlayButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);     // 隐藏播放按钮
            _ui_flag_modify(ui_PauseButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE); // 显示暂停按钮
            // 注意：海报（专辑封面）不再做旋转动画。旋转一张带 Alpha 的图片需要每帧软?
            // 变换 + 混合，会持续占满 SPI 刷屏带宽，与 SD 读卡争用同一?SPI 总线?
            // 表现为上半屏花屏/卡死 + 音频卡顿。封面改为静态显示?
            // 重新启动歌词和标题标签的滚动动画
            const char *current_title = lv_label_get_text(ui_MusicTitleLabel);
            const char *current_lrc = lv_label_get_text(ui_MusicLrcLabel);
            lv_label_set_text(ui_MusicTitleLabel, current_title);                        // 重新设置标题文本
            lv_label_set_text(ui_MusicLrcLabel, current_lrc);                            // 重新设置歌词文本
            lv_label_set_long_mode(ui_MusicTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);   // 重新设置标题标签滚动模式
            lv_label_set_long_mode(ui_MusicLrcLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);     // 重新设置歌词标签滚动模式

            // 装载完成、真正开始出声了：把顶栏从「正在加载…」切回「正在播放」
            lv_label_set_text(ui_Label2, "正在播放");
            play_executed = true; // 设置执行标志
            pause_executed = false;
            Serial.println("播放执行标志位");
        }

    /* 换曲目的处理已经提前到 UI_update() 开头的 UI_HandleTrackChange() 里了
       （见那里的说明：必须在「还没开始播」的时候就刷新界面，
        否则用户从列表里选歌后会看到一堆上一首的旧文字）。 */

    /* 【内嵌歌词补查 + /lrc 懒生成】
       上面那次 parseLrcFile() 发生在「刚刚切歌」的瞬间，此时解码任务
       还没读文件、标签根本没解析，所以内嵌歌词一定是拿不到的。
       等解码任务把这首的内嵌歌词解析出来（Music_EmbeddedLyricsReady），
       再补查一次 —— 这一步同时也是**唯一**会生成 /lrc/<歌名>.lrc 的地方
       （parseLrcFile 命中内嵌歌词后会顺手落盘），完全符合
       「歌词只在要播放歌曲的时候才读取生成」的设计。
       注意判据里带了曲目归属，不会把上一首残留的歌词误当成这一首的。 */
    if (!lrc_flag && !ui_lrcEmbeddedChecked && fileCount > 0 &&
        music_i >= 0 && music_i < fileCount &&
        Music_EmbeddedLyricsReady(musicFiles[music_i].c_str()))
    {
      ui_lrcEmbeddedChecked = true;
      parseLrcFile(musicFiles[music_i]);
      if (lrc_flag)
      {
        ui_lrcPlaceholderShown = false;
        lv_label_set_text(ui_MusicLrcLabel, "暂无歌词"); // 先清掉占位
        Serial.println("已补查到曲目内嵌歌词（并生成 /lrc 文件）");
      }
      else
      {
        lv_label_set_text(ui_MusicLrcLabel, "暂无歌词");
        ui_lrcPlaceholderShown = true;
      }
    }

    // 显示播放歌词
    if (lrc_flag && Music_GetCurrentPlayTime() != temp_AudioCurrentTime) // lrc_flag 为找到歌词标志位，且播放时间值跟之前找到歌词时间值不一样，确保只显示一次歌?
    {

      for (uint8_t i = 0; i < lyricCount; i++)
      {
        if (Music_GetCurrentPlayTime() == lyrics[i].timestamp)
        {

          temp_AudioCurrentTime = Music_GetCurrentPlayTime();
          Serial.println(lyrics[i].lyric);
          lv_label_set_text(ui_MusicLrcLabel, lyrics[i].lyric.c_str());
          break;
        }
      }
    }
    if (!lrc_flag && !ui_lrcPlaceholderShown)
    {
      // 只设置一次：lv_label_set_text 会无条件 invalidate 并重新分配字符串?
      // 每帧重复设置会造成持续重绘?LVGL 堆抖?
      ui_lrcPlaceholderShown = true;
      lv_label_set_text(ui_MusicLrcLabel, "暂无歌词"); //  加载歌词
    }
    //  显示播放时间+进度条（按「秒」节流）
    //  说明：lv_label_set_text_fmt 每次都会重新分配字符串并 invalidate，
    //  以 5ms 的主循环频率调用会产生大量刷新与内存抖动，必须按秒更新。
    //  【必须用 Music_GetDuration()】跨核读取，见 UI_ApplyCurrentState() 中的说明。
    long dur = Music_GetDuration();
    if (dur != 0)
    {
      uint32_t cur = Music_GetCurrentPlayTime();
      if (cur != ui_lastShownSec)
      {
        ui_lastShownSec = cur;
        lv_label_set_text_fmt(ui_MusicTimeLabel1, "%02d:%02d", (int)(cur / 60), (int)(cur % 60));
        lv_label_set_text_fmt(ui_MusicTimeLabel2, "%02d:%02d", (int)(dur / 60), (int)(dur % 60));
        lv_bar_set_value(ui_Bar1, map(cur, 0, dur, 0, 100), LV_ANIM_OFF);
      }
    }
  }
  else
  {
    /* 没在播放时，先只负责把按钮切到「暂停」外观。
       注意这里**不能**顺手判断「该切下一首了」：
       !Music_IsPlaying() 在「正在装载」和「用户暂停」时同样成立。 */
    if (!pause_executed)
    {
      _ui_flag_modify(ui_PauseButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);   // 隐藏暂停按钮
      _ui_flag_modify(ui_PlayButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE); // 显示播放按钮
      pause_executed = true;
    }

    play_executed = false; // 重置标志

    /* 只有「曲目自然播放结束」才自动切歌。
       必须问音频侧专门的事件标志，而不是从「是否正在播放」反推 ——
       否则正在装载下一首、或用户刚按下暂停时，都会被误判成放完了，
       于是莫名跳歌、UI 与实际播放状态对不上。
       另外必须确认存储卡仍在且列表非空：卡被拔出时列表会被清空（musicFiles
       已释放），此时即便解码器因读卡失败报了「结束」，也不能去索引一个空的
       列表 —— 否则会解引用已释放的指针。 */
    if (Music_ConsumeEnded() && Music_IsStorageReady() && fileCount > 0)
    {
      Serial.println("曲目播放结束，按循环方式切换");
      switch (currentPlayMode)
      {
      case LIST_LOOP:
        if (music_i < (fileCount - 1))
        {
          music_i++; // 切换到下一首
        }
        else
        {
          music_i = 0; // 回到列表开头
        }
        Music_PlayPath(musicFiles[music_i].c_str()); // 自动播放下一首
        Serial.println("列表循环播放下一首");
        break;

      case SINGLE_LOOP:
        Music_PlayPath(musicFiles[music_i].c_str()); // 重新播放当前曲目
        Serial.println("单曲循环播放");
        break;

      case RANDOM_PLAY:
        music_i = random(0, fileCount - 1); // 生成随机索引
        Music_PlayPath(musicFiles[music_i].c_str());
        Serial.println("随机播放");
        break;

      default:
        break;
      }
    }
  }
}
