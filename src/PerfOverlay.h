// 性能浮窗（帧率 / CPU / 内存）——编译期开关控制
//
// 用法：
//   在 platformio.ini 的 build_flags 里加 -DENABLE_PERF_OVERLAY=1 打开，
//   改成 0 或整行删掉即可关闭。关闭时本模块的代码会被完全编译掉
//   （函数体为空、宏直接展开成空语句），不占 Flash、不占 CPU。
//
// 为什么挂在 lv_layer_top()：
//   主界面在「进播放列表页」时会被整个删除，浮窗如果挂在 ui_Screen1 上
//   就会跟着消失。lv_layer_top() 是屏幕之上的独立层，切屏不受影响 ——
//   和 PlaylistTool 的进度弹窗用的是同一个套路。

#ifndef PERF_OVERLAY_H
#define PERF_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化浮窗（在 ui_init() 之后调用）。
   未启用开关时是空函数。 */
void PerfOverlay_Init(void);

/* 每轮主循环调用一次：内部自己按固定间隔刷新，不做任何耗时的事。
   未启用开关时是空函数（但仍会编译，方便随时打开开关）。 */
void PerfOverlay_Loop(void);

/* 是否启用浮窗（供 UI 侧判断，比如决定是否给浮窗让出屏幕空间） */
bool PerfOverlay_IsEnabled(void);

#ifdef __cplusplus
}
#endif

#endif // PERF_OVERLAY_H
