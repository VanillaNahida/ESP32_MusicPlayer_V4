// SD 卡热插拔检测 + 浮窗提醒
//
// 功能：
//   1) 运行途中拔卡 / 插卡能自动识别；
//   2) 用 LVGL 浮窗（挂在 lv_layer_top() 上）提醒：
//        · 开机没插卡      → 「SD卡未插入」
//        · 运行中拔卡      → 「SD卡已拔出」
//        · 运行中插入卡    → 「SD卡已插入」
//   3) 拔卡后自动停止播放并清空播放列表；插卡后自动重新初始化 SD 卡、
//      重新载入（或扫描）播放列表。
//
// 线程模型与 PlaylistTool 相同：一切逻辑都在 core1 的主循环（loop()）里
// 每圈调用一次 SdHotplug_Poll() 完成，内部按时间节流，不做阻塞式整卡扫描
//（重连后若没有播放列表缓存，用 Music_RescanStep 分片推进）。

#ifndef SDHOTPLUG_H
#define SDHOTPLUG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化（setup() 里、UI 初始化之后调用）。
// 会依据当前存储卡状态决定要不要先弹「SD卡未插入」的浮窗。
void SdHotplug_Init(void);

// 主循环里每次调用：状态机推进拔卡/插卡检测 + 播放列表重建 + 浮窗显示。
void SdHotplug_Poll(void);

// 立即弹一条浮窗提醒（会覆盖当前正在显示的浮窗，重新计时）。
// 文本用中文字体，自动在数秒后消失。可在任意 UI 线程位置调用。
void SdHotplug_ShowToast(const char *text);

#ifdef __cplusplus
}
#endif

#endif // SDHOTPLUG_H
