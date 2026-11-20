#ifndef DGL_MATH_H
#define DGL_MATH_H
/* Doom 引擎只在 V_DrawMouseSpeedBox (测鼠标用, 默认不走) 用到 fabs */
static inline double fabs(double x){ return x < 0 ? -x : x; }
#endif
