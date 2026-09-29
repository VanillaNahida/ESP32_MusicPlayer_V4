/*
  TouchCal —— 串口触发的触摸校准界面

  为什么触发方式是「串口命令」而不是屏幕上的一个按钮：
  校准的动机就是「当前标定不准、点哪都不对」。如果入口做成屏幕按钮，
  标定不准时就点不中它 —— 也就是这套代码当初被删掉的那个死锁。
  改成串口 'c' 进入之后，入口不再依赖触摸。

  采样为什么一定准：本模块全程用 Touch_ReadRaw() 读**原始值**，
  完全不经过当前标定换算，所以哪怕现在的标定错得离谱，
  采到的原始值也仍然正确 —— 「用错的标定去校准标定」这个问题不存在了。

  流程：
     串口 'c' → 屏幕依次显示 4 个十字标记（左上/右上/左下/右下），
     用户逐个触摸其中心 → 采到的原始值按屏幕坐标**外推**到四边 →
     Touch_SetCalibration() 立即生效 + Touch_SaveCalibration() 存入 NVS。
     串口 'x' 可随时取消。

  见 Touch.h / Touch.cpp 中的标定说明。
*/

#ifndef TOUCHCAL_H
#define TOUCHCAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 开始校准（由主循环的串口命令 'c' 调用）。已在校准中则忽略。 */
void TouchCal_Start(void);

/* 取消正在进行的校准（串口命令 'x'）。 */
void TouchCal_Cancel(void);

/* 主循环轮询：推进采样状态机。未校准时立即返回，开销可忽略。 */
void TouchCal_Poll(void);

/* 是否正在校准（供主循环判断是否需要跳过其它界面刷新）。 */
bool TouchCal_IsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCHCAL_H */