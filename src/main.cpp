#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <Music.h>
#include "AlbumArt.h"
#include "AudioBusLock.h"
#include "Touch.h"

/*函数声明*/
void UI_update();

/* 串口调试命令（在 loop() 里处理）：
     t = 打印当前标定与一次实测换算（诊断触摸「错位」）

   触摸校准界面已移除：标定值直接沿用 Touch.cpp 顶部那组项目实测值。
   原因见 Touch.cpp 中「校准界面相关接口已全部移除」的说明。 */

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

  // 音频初始化完成（命令队列已就绪）后再启动解码任务，内?
  xTaskCreatePinnedToCore(audioTask, "audioTask", 16384, NULL, configMAX_PRIORITIES - 1, &audioTaskHandle, 0);

  delay(1000);

  String LVGL_Arduino = "Hello Arduino! ";
  LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

  Serial.println(LVGL_Arduino);
  Serial.println("I am LVGL_Arduino");

  lv_init();

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

  ui_init();

  // 上电直接进主界面，不再强制进入校准界面。
  // 触屏标定值写死在 Touch.cpp 顶部（沿用项目原本实测可用的那组），
  // 因此不存在「标定页依赖触摸、触摸不准就进不去」的死锁。

  // 启动专辑封面后台解码任务（低优先级，解码完成后由 AlbumArt_Poll 换上?
  AlbumArt_Init();

  // 恢复上次播放状态（UI初始化后?
  Music_RestorePlayState();

  Serial.println("Setup done");

  /*初始化后更新页面显示信息*/
  lv_label_set_text(ui_MusicTitleLabel, currentTitle.c_str());   //  歌曲?
  lv_label_set_text(ui_MusicArtistLabel, currentArtist.c_str()); //  歌手?
  if (lrc_flag)
  {
    lv_label_set_text(ui_MusicLrcLabel, "歌词加载?.."); //  加载歌词
  }
  else
  {
    lv_label_set_text(ui_MusicLrcLabel, "暂无歌词"); //  加载歌词
  }
  //  显示播放时间+进度?
  if (duration != 0)
  {

    lv_img_set_angle(ui_citou, map(Music_GetCurrentPlayTime(), 0, duration, 60, -60)); // 设置磁头转动角度

    uint8_t minutes = Music_GetCurrentPlayTime() / 60;
    uint8_t seconds = Music_GetCurrentPlayTime() % 60;

    lv_label_set_text_fmt(ui_MusicTimeLabel1, "%02d:%02d", minutes, seconds);
    lv_label_set_text_fmt(ui_MusicTimeLabel2, "%02d:%02d", duration / 60, duration % 60);

    lv_bar_set_value(ui_Bar1, map(Music_GetCurrentPlayTime(), 0, duration, 0, 100), LV_ANIM_OFF);
  }
}


void loop()
{
  // 串口命令（只剩诊断用；校准界面已移除）
  while (Serial.available() > 0)
  {
    int ch = Serial.read();
    if (ch == 't' || ch == 'T')
    {
      // 诊断：打印当前标定 + 一次实测换算，用来判断错位原因
      Touch_DumpDebug();
    }
    else if (ch == '\r' || ch == '\n')
    {
      // 忽略行尾
    }
  }

  lv_timer_handler(); /* let the GUI do its work */
  UI_update();        // 更新UI
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

// 主界面被销毁后重新创建时调用（?lib/ui/ui.h 声明?
void UI_NotifyScreenRebuilt(void)
{
    ui_needsRefresh = true;
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

    lv_label_set_text(ui_MusicLrcLabel, lrc_flag ? "歌词加载?.." : "暂无歌词");

    if (duration > 0)
    {
        long cur = (long)Music_GetCurrentPlayTime();
        lv_label_set_text_fmt(ui_MusicTimeLabel1, "%02d:%02d", (int)(cur / 60), (int)(cur % 60));
        lv_label_set_text_fmt(ui_MusicTimeLabel2, "%02d:%02d", (int)(duration / 60), (int)(duration % 60));
        lv_bar_set_value(ui_Bar1, map(cur, 0, duration, 0, 100), LV_ANIM_OFF);
        lv_img_set_angle(ui_citou, map(cur, 0, duration, 60, -60));
    }
    else
    {
        lv_img_set_angle(ui_citou, -200);
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
        lv_label_set_text(ui_Label2, "暂停播放");
    }

    UI_UpdateAlbumArt();
}

void UI_update()
{
    // 播放列表页打开期间主界面已被销毁，跳过所有主界面控件的更?
    if (ui_Screen1 == nullptr)
    {
        return;
    }

    // 主界面被重建（从播放列表页返回）?整体重新应用一次状?
    if (ui_needsRefresh)
    {
        ui_needsRefresh = false;
        play_executed = false;
        pause_executed = false;
        shownTitle = "";
        shownArtist = "";
        ui_coverAttemptedRev = 0;
        ui_coverShownDefault = false;
        ui_lastShownSec = 0xFFFFFFFF;
        ui_lrcPlaceholderShown = false;
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
            lv_img_set_angle(ui_citou, 0);                                               // 磁头转到播放位置
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

            play_executed = true; // 设置执行标志
            pause_executed = false;
            Serial.println("播放执行标志位");
        }

    if (music_prev_i != music_i) // 播放歌曲索引与上一个歌曲索引不一?,重新解析歌曲信息和歌?
    {
      // 重置时长；曲目信息（标题/歌手/时长）由解码器通过回调异步提供
      duration = 0;
      durationPrinted = false;
      parseLrcFile(musicFiles[music_i]); //  歌词解析

      // 更新索引追踪
      music_prev_i = music_i;
      ui_lastShownSec = 0xFFFFFFFF; // 强制下一帧刷新时?进度
      ui_lrcPlaceholderShown = false;
      if (lrc_flag)
      {
        lv_label_set_text(ui_MusicLrcLabel, "歌词加载?.."); //  加载歌词
      }
      else
      {
        lv_label_set_text(ui_MusicLrcLabel, "暂无歌词"); //  加载歌词
        ui_lrcPlaceholderShown = true;
      }
      Serial.println("更新索引和歌词");
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
    //  说明：lv_label_set_text_fmt 每次都会重新分配字符串并 invalidate?
    //  ?5ms 的主循环频率调用会产生大量刷新与内存抖动，必须按秒更新?
    if (duration != 0)
    {
      uint32_t cur = Music_GetCurrentPlayTime();
      if (cur != ui_lastShownSec)
      {
        ui_lastShownSec = cur;
        lv_img_set_angle(ui_citou, map(cur, 0, duration, 60, -60)); // 设置磁头转动角度
        lv_label_set_text_fmt(ui_MusicTimeLabel1, "%02d:%02d", (int)(cur / 60), (int)(cur % 60));
        lv_label_set_text_fmt(ui_MusicTimeLabel2, "%02d:%02d", (int)(duration / 60), (int)(duration % 60));
        lv_bar_set_value(ui_Bar1, map(cur, 0, duration, 0, 100), LV_ANIM_OFF);
      }
    }
  }
  else
  {
    /* 没在播放时，先只负责把按钮/磁头切到「暂停」外观。
       注意这里**不能**顺手判断「该切下一首了」：
       !Music_IsPlaying() 在「正在装载」和「用户暂停」时同样成立。 */
    if (!pause_executed)
    {
      _ui_flag_modify(ui_PauseButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_ADD);   // 隐藏暂停按钮
      _ui_flag_modify(ui_PlayButton, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE); // 显示播放按钮
      lv_img_set_angle(ui_citou, -200); // 磁头转到停止位置
      pause_executed = true;
    }

    play_executed = false; // 重置标志

    /* 只有「曲目自然播放结束」才自动切歌。
       必须问音频侧专门的事件标志，而不是从「是否正在播放」反推 ——
       否则正在装载下一首、或用户刚按下暂停时，都会被误判成放完了，
       于是莫名跳歌、UI 与实际播放状态对不上。 */
    if (Music_ConsumeEnded())
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
