/*
  Touch —— 触摸读取 / 校准 / 坐标标定

  这个模块回答一个问题：**同一块 XPT2046 触摸，与 SD 卡读卡任务、TFT 刷屏任务
  共用同一条 SPI 总线（各自用不同 CS 片选）时，如何保证按下去就有反应。**

  历史踩坑记录（都别再改回去）：

  0) 【真正的根因】触摸 CS 引脚与其他外设撞车。
     本机 TOUCH_CS = GPIO15，而音频的 I2S_BCLK 一度也定义成 GPIO15。
     同一个物理引脚被两个外设占用 → 触摸永远读到 z=0、完全没反应，
     但屏幕和 SD 卡都正常（它们走的是另外几根线），非常容易误判成
     「SPI 配置问题」而去乱改 SPI 寄存器。
     现在 I2S_BCLK 已移到 GPIO18。
     → 任何时候动 User_Setup.h 的引脚定义，都要和
       lib/Music/Music.cpp 的 SD_Pin / I2S_* 做一次交叉检查，
       确认没有任何一个 GPIO 被两个外设同时使用。

     另外两条曾经被误认为根因、但经核对源码**并不成立**的说法，
     记在这里避免以后又有人绕回去：
       · 「XPT2046.begin() 的无参 SPI.begin() 会覆盖 TFT 的引脚绑定」
         —— 不成立：SPIClass::begin() 开头 `if (_spi) return true;`
         （SPI.cpp:67），总线已启动时它是空操作；而且 S3 variant 的默认
         引脚本来就是 12/13/11，和 TFT_eSPI 用的完全一样。
       · 「必须在读触摸前手工把 SPI_USER 切成接收模式」
         —— 不成立：Arduino 的 spiInitBus() 本来就把总线配成
         usr_mosi=1/usr_miso=1/doutdin=1，收发兼具。

  1) 不要给 XPT2046_Touchscreen 传 TOUCH_IRQ 引脚。
     该库 update() 开头是 `if (!isrWake) return;`，isrWake 只在 IRQ 引脚下
     降沿被 ISR 置位、并在松手时清零。一旦传了 IRQ，空闲期触摸控制器就完全
     不被轮询 —— 表现为「按住屏幕一切正常、松手后界面卡住」。
     不传 IRQ 引脚后 isrWake 恒为 true，触摸被持续轮询。

  2) 不要用 tirqTouched() 做门控。理由同上：门控会让空闲期完全没有触摸事务。

  3) 不要用 readData()。它的 z 形参是 uint8_t*，会把 zraw(0~4095) 截断成 8 位，
     Z 阈值判断永远不成立 —— 触摸完全失灵。用 TS_Point（z 是 int16_t）。

  4) 不能用 Arduino 的 map() 做原始值→屏幕坐标的换算。
     map() 把 [-XPT2046 原始范围 0..4095] 折算到 240/320，也就是假定
     触摸原始值的行程和屏幕像素行程是「同一条轴的两个比例尺」。
     但 XPT2046 的原始值范围本来就和屏幕像素不成比例：map() 会把实际较短的
     那段行程放大到整个屏幕，屏幕边缘一小段区域因此挤出越界坐标，那一片
     就是点不动的死区。
     正确做法是像 Adafruit_TouchScreen 的 map() 一样，按**实际需要的行程**
     （校准得到的边缘原始值 → 0..width-1 / 0..height-1）分别算除数和截断。

  5) 触摸读数必须无条件轮询（见 (1)(2)），并且每次 SPI 事务都要用
     AudioBusLock 包住，否则会和刷屏 / 读卡互相破坏。

  6) 节流要自己做，不能依赖库内部那 3ms。
     库的 update() 在节流窗口内直接 return，**不更新 zraw**，
     getPoint() 于是返回上一次的陈旧值。手指抬起后会被持续判为「仍按住」，
     表现为按钮连点。Touch_ReadRaw() 里用 TOUCH_READ_INTERVAL_MS 自己节流。

  7) 触摸轴方向（s_ts.setRotation）必须与 TFT 的 setRotation() 配套。
     不配套会让 X/Y 互换或方向翻转，表现为「点哪都不对」。
     真跑一次校准可以纠正剩余偏差。
*/

#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化触摸控制器：begin + 应用文件顶部那组实测标定值 */
void Touch_Init(void);

/* LVGL 输入设备读取回调 */
void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data);

/* 读一次原始值；false 表示当前没有触摸 */
bool Touch_ReadRaw(int16_t *x, int16_t *y, int16_t *z);

/* 调试：打印当前标定与一次实测的换算结果（串口 't' 触发） */
void Touch_DumpDebug(void);

/* ------------------------------------------------------------------
   校准界面与 NVS 存储已移除，直接用 Touch.cpp 顶部的实测标定值。
   原因见 Touch.cpp 中「校准界面相关接口已全部移除」那段说明。
   ------------------------------------------------------------------ */

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_H */
