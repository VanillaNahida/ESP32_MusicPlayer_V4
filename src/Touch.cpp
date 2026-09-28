#include "Touch.h"

#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

#include "AudioBusLock.h"

/* 本机 SPI 总线引脚（必须与 lib/TFT_eSPI/User_Setup.h 保持一致）
   TFT 与 SD 与触摸三者共用这一条 SPI2 总线，各自用不同 CS。 */
#ifndef TOUCH_SPI_SCLK
#define TOUCH_SPI_SCLK 12
#endif
#ifndef TOUCH_SPI_MISO
#define TOUCH_SPI_MISO 13
#endif
#ifndef TOUCH_SPI_MOSI
#define TOUCH_SPI_MOSI 11
#endif

/* 触摸片选引脚。TFT_eSPI 的 User_Setup.h 里定义的是 TOUCH_CS，但那个头文件
   只被 TFT_eSPI 自己的源文件包含，这里拿不到，所以本地兜一份默认值
   （与 lib/TFT_eSPI/User_Setup.h 保持一致，改引脚时两边都要改）。 */
#ifndef TOUCH_CS
#define TOUCH_CS 15
#endif

/* 与 XPT2046 库内部的 Z_THRESHOLD 保持一致 */
#define TOUCH_Z_THRESHOLD 400

/* ==================================================================
   编译期引脚冲突检查

   触摸「完全没反应、自检 z 恒为 0」曾经的真凶就是：TOUCH_CS = GPIO15
   而音频 I2S_BCLK 也 = GPIO15 —— 同一个物理引脚被两个外设占用。
   这种错误在运行时极难从现象反推（屏幕和 SD 都正常），
   所以在编译期直接拦住。

   ⚠ 改 lib/Music/Music.cpp 或 User_Setup.h 的引脚时，**必须同步改这里**，
   否则这道检查会因为比对的是旧值而形同虚设（已经漏过两次了：
   一次是 I2S 从 18 改到 5 却还写着 18，一次是后来改成 1/2/8 却还写着 5/6/7）。
   ================================================================== */
#define TOUCH_I2S_BCLK_PIN 1  /* 与 lib/Music/Music.cpp 的 I2S_BCLK 保持一致 */
#define TOUCH_I2S_LRC_PIN 2   /* 与 lib/Music/Music.cpp 的 I2S_LRC  保持一致 */
#define TOUCH_I2S_DOUT_PIN 8  /* 与 lib/Music/Music.cpp 的 I2S_DOUT 保持一致 */
#define TOUCH_SD_CS_PIN 38    /* 与 lib/Music/Music.cpp 的 SD_Pin   保持一致 */
#define TOUCH_BOOT_PIN 0      /* GPIO0 是 strapping 引脚，务必避开 */

#define TOUCH__CONFLICT(a, b) ((a) == (b))

#if TOUCH__CONFLICT(TOUCH_CS, TOUCH_I2S_BCLK_PIN) || \
    TOUCH__CONFLICT(TOUCH_CS, TOUCH_I2S_LRC_PIN) ||  \
    TOUCH__CONFLICT(TOUCH_CS, TOUCH_I2S_DOUT_PIN) || \
    TOUCH__CONFLICT(TOUCH_CS, TOUCH_SD_CS_PIN) ||    \
    TOUCH__CONFLICT(TOUCH_CS, TOUCH_BOOT_PIN)
#error "TOUCH_CS 与音频/SD/BOOT 的某个引脚冲突了！请检查 User_Setup.h 与 lib/Music/Music.cpp 的引脚定义。"
#endif

/* SPI 三根总线线也不能和音频撞车 */
#if TOUCH__CONFLICT(TOUCH_SPI_SCLK, TOUCH_I2S_BCLK_PIN) || \
    TOUCH__CONFLICT(TOUCH_SPI_SCLK, TOUCH_I2S_LRC_PIN) ||  \
    TOUCH__CONFLICT(TOUCH_SPI_SCLK, TOUCH_I2S_DOUT_PIN) || \
    TOUCH__CONFLICT(TOUCH_SPI_MISO, TOUCH_I2S_BCLK_PIN) || \
    TOUCH__CONFLICT(TOUCH_SPI_MISO, TOUCH_I2S_LRC_PIN) ||  \
    TOUCH__CONFLICT(TOUCH_SPI_MISO, TOUCH_I2S_DOUT_PIN) || \
    TOUCH__CONFLICT(TOUCH_SPI_MOSI, TOUCH_I2S_BCLK_PIN) || \
    TOUCH__CONFLICT(TOUCH_SPI_MOSI, TOUCH_I2S_LRC_PIN) ||  \
    TOUCH__CONFLICT(TOUCH_SPI_MOSI, TOUCH_I2S_DOUT_PIN)
#error "SPI 总线引脚(SCLK/MISO/MOSI)与 I2S 引脚冲突了！"
#endif

/* 触摸/SPI 引脚同样不能落在 N16R8 的 Flash/PSRAM 保留区(GPIO26~37)，
   否则会和音频犯同一个错：驱动 PSRAM 数据线导致内存错乱、不断复位。 */
#if (TOUCH_CS >= 26 && TOUCH_CS <= 37) || (TOUCH_SPI_SCLK >= 26 && TOUCH_SPI_SCLK <= 37) || \
    (TOUCH_SPI_MISO >= 26 && TOUCH_SPI_MISO <= 37) || (TOUCH_SPI_MOSI >= 26 && TOUCH_SPI_MOSI <= 37)
#error "触摸/SPI 引脚落在 Flash/PSRAM 保留区(GPIO26~37)！N16R8 模组会因内存错乱不断复位。"
#endif

/* 两次真实 SPI 采样之间的最小间隔。
   必须大于库内部的 MSEC_THRESHOLD（3ms），否则库的节流会生效并返回陈旧值。 */
#define TOUCH_READ_INTERVAL_MS 4

/* ==================================================================
   关于 SPI 模式：不要把 SPI_USER 改成「接收模式」

   这里曾经写过一段「手工把 SPI_USER 切成 SPI_USR_MISO|SPI_DOUTDIN」
   的代码，那是**错的**，已删除，别再改回来：

   Arduino 的 SPI 驱动在 spiInitBus() 里本来就把总线配成了收发兼具
   （esp32-hal-spi.c: usr_mosi=1, usr_miso=1, doutdin=1），
   XPT2046 的读事务走的是 IDF 驱动这套配置，本来就是对的。
   我们若在事务外手工改 SPI_USER，反而会被 beginTransaction()/
   spiTransaction() 覆盖，属于无效代码，只会误导后来的人。
   ================================================================== */

/* ------------------------------------------------------------------
   触摸控制器实例
   刻意不传 TOUCH_IRQ，原因见 Touch.h 顶部第 (1) 条
   ------------------------------------------------------------------ */
static XPT2046_Touchscreen s_ts(TOUCH_CS);

/* 触摸坐标系与屏幕的对应关系。
   XPT2046 库内部按 rotation 对原始轴做交换/取反：
     rotation=0 → x=4095-y, y=x
     rotation=1 → x=x,      y=y
     rotation=3 → x=4095-x, y=4095-y
   必须与 TFT 的 setRotation() 配套，否则 X/Y 会互换、方向也会反，
   表现为「点哪都不对」甚至「完全点不动」。

   必须与 TFT 的 setRotation() 配套，否则 X/Y 会互换、方向也会反。

   这里取 0，与 git 首个提交里那份「能正常用」的实现一致：
       git show 43515e4:src/main.cpp
       xpt_touch.setRotation(0);   // 与屏幕一致
       tft.setRotation(0);
   之前误设为 1（原始轴直通），与那套标定值不匹配，会表现为「错位」。 */
#define TOUCH_ROTATION 0

static bool s_inited = false;

/* 屏幕分辨率（与 LVGL / TFT 保持一致） */
static const uint16_t s_width = 240;
static const uint16_t s_height = 320;

/* ==================================================================
   标定值（原始值 → 屏幕像素）—— 直接沿用项目原本就在用的实测值

   来源：git 首个提交里那段能正常工作的换算
       git show 43515e4:src/main.cpp
       touchX = map(p.x, 3700, 320,  0, 240);
       touchY = map(p.y,  300, 3820, 0, 320);

   即：
       x 轴：原始 3700 → 屏幕最左(0)，原始  320 → 屏幕最右(239)  ← 反向
       y 轴：原始  300 → 屏幕最顶(0)，原始 3820 → 屏幕最底(319)

   注意 x 是**反向**的（左端原始值比右端大）。这不是笔误：
   scaleToScreen() 用 (raw-lo)*(range-1)/span 计算，span 为负时
   分子分母的负号相消，反向区间能正确换算，无需特殊处理。
   若两者相等会除零，所以 hi==lo 时直接返回 0。

   改这里就是唯一需要改标定的地方（校准界面已移除）。
   ================================================================== */
static int32_t s_xLeft = 3700;  /* 屏幕 x=0   处的原始值 */
static int32_t s_xRight = 320;  /* 屏幕 x=239 处的原始值 */
static int32_t s_yTop = 300;    /* 屏幕 y=0   处的原始值 */
static int32_t s_yBottom = 3820;/* 屏幕 y=319 处的原始值 */

/* 原始值 → 屏幕坐标的安全换算（不用 Arduino map()，见 Touch.h 第 (4) 条） */
static int32_t scaleToScreen(int32_t raw, int32_t lo, int32_t hi, int32_t range)
{
    int32_t span = hi - lo;
    if (range <= 0 || span == 0)
    {
        return 0;
    }
    int32_t v = (int32_t)((int64_t)(raw - lo) * (range - 1) / span);
    if (v < 0)
    {
        v = 0;
    }
    if (v >= range)
    {
        v = range - 1;
    }
    return v;
}


bool Touch_ReadRaw(int16_t *x, int16_t *y, int16_t *z)
{
    if (!s_inited)
    {
        return false;
    }

    /* 时间节流不能靠库内部那 3ms 判断（见下）。
       我们自己在这里挡住「同一次触摸的重复读取」，保证每个采样周期
       只做一次真正的 SPI 事务。 */
    static uint32_t s_lastReadMs = 0;
    static bool s_lastTouched = false;
    static int16_t s_lastX = 0, s_lastY = 0, s_lastZ = 0;

    uint32_t now = millis();

    /* XPT2046 库的 update() 内部有 `if (now - msraw < MSEC_THRESHOLD) return;`
       （MSEC_THRESHOLD = 3ms）。注意它在 return 前**不会更新 zraw**，
       也就是说节流期间 getPoint() 返回的是上一次的**陈旧值**：
       手指抬起后，只要还没满 3ms，读到的 z 依然是抬起前的大于阈值的值。
       在 5ms 的 loop 里这会让「已松手」被持续判为「仍按住」，
       表现为按钮连点、点击落点乱跳。

       所以这里必须自己在节流窗口内直接返回上一次的结果，
       而不是去调 getPoint() 拿陈旧数据。 */
    if (now - s_lastReadMs < TOUCH_READ_INTERVAL_MS)
    {
        if (!s_lastTouched)
        {
            return false;
        }
        if (x) *x = s_lastX;
        if (y) *y = s_lastY;
        if (z) *z = s_lastZ;
        return true;
    }
    s_lastReadMs = now;

    /* XPT2046 与 TFT / SD 共用同一条 SPI 总线，库内部不做任何仲裁，
       必须由我们把这一次 SPI 事务整体包进总线锁里。
       临界区只有一次 6 字节 SPI 事务（约 20us），不会饿死刷屏。 */
    AudioBusLock();
    TS_Point p = s_ts.getPoint();
    AudioBusUnlock();

    if (p.z < TOUCH_Z_THRESHOLD)
    {
        s_lastTouched = false;
        s_lastZ = 0;
        return false;
    }

    s_lastTouched = true;
    s_lastX = p.x;
    s_lastY = p.y;
    s_lastZ = p.z;

    if (x)
    {
        *x = p.x;
    }
    if (y)
    {
        *y = p.y;
    }
    if (z)
    {
        *z = p.z;
    }
    return true;
}

void Touch_Init(void)
{
    /* ==============================================================
       说明（修正历史误判，别再照抄旧注释）：

       曾经以为「XPT2046.begin() 里的无参 SPI.begin() 会用默认引脚覆盖
       TFT_eSPI 绑定的 12/13/11」。经逐行核对 Arduino core 源码，
       这个判断是**错的**，两个原因：

         1) SPIClass::begin() 开头就是 `if (_spi) return true;`
            （SPI.cpp:67）—— 总线已由 tft.begin() 启动过，
            再调 begin() 直接返回，什么都不会改。
         2) ESP32-S3 variant 的默认引脚本来就是 SCK=12 / MISO=13 / MOSI=11
            （pins_arduino.h），与 TFT_eSPI 用的完全相同。

       所以这里不需要、也不能靠重复 SPI.begin() 来「纠正引脚」。
       下面保留一次显式 begin 只是为了在「TFT 未初始化」的极端顺序下
       仍能自行起总线，正常路径下它是空操作。
       ============================================================== */
    SPI.begin(TOUCH_SPI_SCLK, TOUCH_SPI_MISO, TOUCH_SPI_MOSI, TOUCH_CS);

    s_ts.begin();

    /* 触摸轴方向必须与 TFT 的 setRotation() 配套 */
    s_ts.setRotation(TOUCH_ROTATION);

    /* XPT2046 的 CS 必须由我们确保处于「未选中」（高电平）。
       TFT_eSPI 的 initBus() 虽然也会拉高 TOUCH_CS，但那是**它自己**的
       配置；这里再确认一次，避免两条初始化路径对同一引脚的状态期望不一致。 */
    pinMode(TOUCH_CS, OUTPUT);
    digitalWrite(TOUCH_CS, HIGH);

    /* 标定值直接使用文件顶部那组项目实测值，不再读 NVS / SD，也不再进校准界面。
       好处：开机即用、不依赖 NVS 是否可用、也不会因为「标定页本身点不动」而卡死。 */
    Serial.printf("[TOUCH] 标定 x=%d..%d y=%d..%d (屏 %ux%u, rotation=%d)\n",
                  (int)s_xLeft, (int)s_xRight, (int)s_yTop, (int)s_yBottom,
                  (unsigned)s_width, (unsigned)s_height, TOUCH_ROTATION);

    s_inited = true;

    /* ---------------- 诊断输出 ----------------
       触摸无声无息地「没反应」时，靠下面这些信息定位，不要凭猜。
       先确认引脚与总线状态，再看自检的 z 值。 */
    Serial.printf("[TOUCH] 引脚 SCLK=%d MISO=%d MOSI=%d CS=%d\n",
                  TOUCH_SPI_SCLK, TOUCH_SPI_MISO, TOUCH_SPI_MOSI, TOUCH_CS);
    Serial.printf("[TOUCH] CS 电平=%d (应为 1/高)，MISO 电平=%d\n",
                  digitalRead(TOUCH_CS), digitalRead(TOUCH_SPI_MISO));

    /* 自检：连续读几次，把原始值打到串口。
       判读方法：
         · z 恒为 0            → 触摸控制器根本没被读到（SPI/供电/CS 问题）
         · z 有值但 raw 不动   → 轴方向或标定问题
         · z 有值且 raw 随手指变化 → 一切正常，剩下的只是标定精度 */
    for (uint8_t i = 0; i < 3; i++)
    {
        int16_t rx = 0, ry = 0, rz = 0;
        bool hit = Touch_ReadRaw(&rx, &ry, &rz);
        Serial.printf("[TOUCH] 自检 %u: %s raw=(%d,%d) z=%d\n",
                      (unsigned)i, hit ? "有触摸" : "无触摸",
                      (int)rx, (int)ry, (int)rz);
        delay(20);
    }
}

/* ==================================================================
   校准界面相关接口已全部移除。

   原因：那套「8 点采样 + 存 NVS」的设计依赖触摸本身先能工作 ——
   而标定不准时，用户根本点不中红点，形成「用错的标定去校准标定」的
   死锁。项目原本就有一组实测可用的标定值（见文件顶部），
   直接沿用即可，既无死锁风险，也不依赖 NVS 是否可用。
   需要改标定时，改文件顶部那 4 个 s_xLeft/s_xRight/s_yTop/s_yBottom。
   ================================================================== */

void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    int16_t rawX = 0, rawY = 0, rawZ = 0;
    if (!Touch_ReadRaw(&rawX, &rawY, &rawZ))
    {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = 0;
        data->point.y = 0;
        return;
    }

    lv_coord_t sx = (lv_coord_t)scaleToScreen(rawX, s_xLeft, s_xRight, s_width);
    lv_coord_t sy = (lv_coord_t)scaleToScreen(rawY, s_yTop, s_yBottom, s_height);

    data->point.x = sx;
    data->point.y = sy;
    data->state = LV_INDEV_STATE_PR;

    /* 排查「UI 完全没反应」用：把真实上报给 LVGL 的坐标打出来。
       判读方法（关键是把「有没有上报」和「上报得对不对」分开看）：
         · 按住屏幕完全没有任何 [TOUCH] 上报行
               → 触摸根本没读到（SPI/CS/供电），与坐标无关
         · 有上报且 state=PR，但坐标明显不对（比如恒为某点）
               → 轴方向/标定问题，改 TOUCH_ROTATION 或标定值
         · 有上报、坐标也对，但界面仍无响应
               → 问题在 LVGL/事件绑定层，不在触摸
       为避免刷屏，只在「按下/抬起/坐标明显变化」时打印。 */
    static bool s_dbgPrevPr = false;
    static lv_coord_t s_dbgPrevX = -1, s_dbgPrevY = -1;
    bool pr = true;
    bool moved = (abs((int)sx - (int)s_dbgPrevX) > 6) || (abs((int)sy - (int)s_dbgPrevY) > 6);
    if (pr != s_dbgPrevPr || moved)
    {
        s_dbgPrevPr = pr;
        s_dbgPrevX = sx;
        s_dbgPrevY = sy;
        Serial.printf("[TOUCH] raw=(%d,%d) z=%d -> 屏幕 (%d,%d) state=PR\n",
                      (int)rawX, (int)rawY, (int)rawZ, (int)sx, (int)sy);
    }
}

/* 自检用：把当前标定和一次实测的换算结果打出来。
   接串口 't' 触发，用来判断「错位」到底是标定值不对还是轴方向不对。 */
void Touch_DumpDebug(void)
{
    Serial.printf("[TOUCH] 标定 x=%d..%d (屏宽 %u)  y=%d..%d (屏高 %u)\n",
                  (int)s_xLeft, (int)s_xRight, (unsigned)s_width,
                  (int)s_yTop, (int)s_yBottom, (unsigned)s_height);

    int16_t rx = 0, ry = 0, rz = 0;
    if (!Touch_ReadRaw(&rx, &ry, &rz))
    {
        Serial.println("[TOUCH] 当前没有触摸，按住屏幕再敲 't'");
        return;
    }

    int32_t sx = scaleToScreen(rx, s_xLeft, s_xRight, s_width);
    int32_t sy = scaleToScreen(ry, s_yTop, s_yBottom, s_height);
    Serial.printf("[TOUCH] 实测 raw=(%d,%d) z=%d → 屏幕 (%d,%d)\n",
                  (int)rx, (int)ry, (int)rz, (int)sx, (int)sy);
}
