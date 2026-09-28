// 播放列表重建工具（BOOT 键触发 + 询问弹窗 + 扫描进度显示）
//
// 设计要点：
//  1. BOOT 键（GPIO0）在主循环里轮询，带消抖；只在「按下沿」触发一次弹窗。
//  2. 扫描在**主循环里分片推进**（Music_RescanStep），不是阻塞调用 ——
//     否则 lv_timer_handler() 跑不起来，进度条根本画不出来，界面看着像死了。
//  3. 弹窗挂在 lv_layer_top() 上，因此在「播放列表页 / 主界面」之间切换屏幕
//     时不会被连带删掉（lv_scr_load 不会动 top layer）。
//  4. 扫描期间音频是停掉的：解码任务和扫描器抢同一张 SD 卡的同一条 SPI 总线，
//     不停播放的话扫描会慢到没法用，声音也会碎成一片。

#ifndef PLAYLIST_TOOL_H
#define PLAYLIST_TOOL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 BOOT 按键（在 setup() 里、UI 初始化之后调用）
void PlaylistTool_Init(void);

// 主循环里每次调用：处理按键、推进扫描、刷新进度界面
void PlaylistTool_Poll(void);

// 首次使用（卡里没有播放列表缓存）：扫描整张卡并建立播放列表。
// 用的是和按 BOOT 重建**完全相同**的进度界面
// （进度条 + 「正在扫描 N%」+ 当前正在处理的文件路径）。
// 本函数内部会自己驱动 lv_timer_handler()，因此在 setup() 里调用也安全。
void PlaylistTool_RunFirstScan(void);

// 是否正在重建播放列表（重建期间主循环应跳过播放状态相关的界面刷新）
bool PlaylistTool_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif // PLAYLIST_TOOL_H
