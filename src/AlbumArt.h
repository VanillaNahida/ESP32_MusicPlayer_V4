// 专辑封面显示模块（异步）
//
// 设计要点：
//  1. 解码是耗时操作（355KB 的 PNG 解码可达数百毫秒甚至数秒，
//     且解码中间缓冲高达数 MB），绝不能放在 LVGL 的刷新/事件流程里，
//     否则会把 UI 线程和触摸读取一起阻塞住。
//  2. 调用方（主循环）只负责「请求」：立刻切到默认封面，然后由低优先级
//     后台任务去解码；解码完成后把成品位图交回，主循环在合适的时机换上。
//  3. 支持 JPEG（ROM tjpgd）与 PNG（LVGL lodepng）两种内嵌封面格式。
//  4. 输出统一为显示框尺寸（93x93）的 RGB565+Alpha，
//     因此 LVGL 端不需要任何 zoom / 旋转变换，封面只是普通静态贴图。

#ifndef ALBUM_ART_H
#define ALBUM_ART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化后台解码任务（在 setup() 中、UI 初始化之后调用）
void AlbumArt_Init(void);

// 请求显示当前曲目的内嵌封面。
// 本函数立即返回，不会阻塞：内部先切到默认封面，再由后台任务解码。
// revision 为当前封面版本号，用于判断是否需要重新解码。
void AlbumArt_Request(uint32_t revision);

// 在 LVGL 线程中调用：若后台已解码出新封面则换上（无则返回 false）。
// 本函数只做一次指针替换，开销极小。
bool AlbumArt_Poll(void);

// 显示默认封面
void AlbumArt_ShowDefault(void);

// 丢弃已解码的封面（换歌时调用，让旧图立刻失效）
void AlbumArt_Release(void);

#ifdef __cplusplus
}
#endif

#endif // ALBUM_ART_H
