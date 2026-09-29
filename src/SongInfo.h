// 「歌曲信息」弹层：点专辑封面打开，显示当前曲目的详细信息
//
// 设计要点：
//  1) 挂在 lv_layer_top() 上，**不切换屏幕**。
//     主界面在进播放列表页时会被整个删除，用 top layer 就不必关心
//     当前在哪一屏，也避开了 lv_disp_load_scr 与「删除活动屏幕」的
//     顺序陷阱（那个坑在 ui_events.cpp 的 plClose 里有详细说明）。
//  2) 内容是只读的，所以不做定时刷新 —— 打开那一刻取一次快照就够了。
//     每帧去读 SD 卡（取文件大小）会平白增加 SPI 总线竞争。
//  3) 字段用「标签 + 值」两列布局，值允许换行（路径可能很长），
//     整体放在可滚动容器里。

#ifndef SONG_INFO_H
#define SONG_INFO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 打开弹层。已经打开时什么都不做（避免叠出好几层）。
void SongInfo_Open(void);

// 关闭弹层（点「关闭」或点背景遮罩时内部会调用）。
void SongInfo_Close(void);

// 弹层当前是否打开
bool SongInfo_IsOpen(void);

#ifdef __cplusplus
}
#endif

#endif // SONG_INFO_H
