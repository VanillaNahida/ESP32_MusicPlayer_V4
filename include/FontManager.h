// 运行时字体选择：优先用 SD 卡上的 .bin 字体，失败回退内置字体
//
// 背景：
//   内置字体 ui_font_AlibabaPuHuiTi_12 是编译进固件的（只覆盖一个精选字集），
//   全量 CJK 字体会让 Flash 体积暴涨。所以改成：字体用 lv_font_conv 预先生成成
//   LVGL 的 **bin** 格式，放到 SD 卡的 /fonts/ 下，开机时用 lv_font_load() 读进
//   PSRAM。卡里没有 / 读失败 / 内存不足，就回退到内置字体，绝不影响启动。
//
// 界面怎么用：
//   所有 set_text_font 都不要直接写 &ui_font_AlibabaPuHuiTi_12，而是调
//   FontManager_GetFont() —— 它在启动时被赋成「SD 字体 或 内置字体」，
//   于是整块界面（含播放列表页、弹窗）自然统一。
//
// ⚠ FontManager_Init() 必须在 ui_init() **之前**调用，
//   否则界面创建时拿到的还是内置字体指针。

#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <stdbool.h>

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化：注册 LVGL 的 POSIX 文件系统驱动，并尝试加载 SD 卡字体。
// 内部自行处理失败回退，不会抛错、不会阻塞启动流程（只是读卡期间会占住
// SPI 总线，界面此时尚未开始绘制）。
void FontManager_Init(void);

// 取当前生效的字体指针（SD 字体加载成功则指向它，否则指向内置字体）。
// 供所有 lv_obj_set_style_text_font() 使用。
const lv_font_t *FontManager_GetFont(void);

// 当前用的是不是 SD 卡上的字体（调试/日志用）
bool FontManager_IsExternalFontLoaded(void);

// SD 卡字体的完整 LVGL 路径（'S' 盘符 → /sd），日志用
const char *FontManager_GetFontPath(void);

#ifdef __cplusplus
}
#endif

#endif // FONT_MANAGER_H
