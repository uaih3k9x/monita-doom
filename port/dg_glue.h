/*
 * dg_glue.c 的接口 (平台层调用)。
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MONITA_DOOM_DG_GLUE_H
#define MONITA_DOOM_DG_GLUE_H
#include <stdint.h>
#include "dg_core.h"

void dg_glue_zone_ready(void);                      /* DG_ZoneAlloc 拿到 zone 后调: 之后 DG_BigAlloc 走 zone */
void dg_present(uint16_t *fb, uint32_t stride, const dgc_geom_t *g);   /* DG_ScreenBuffer (8 位) -> RGB565 放大 */
void dg_audio_render(int16_t *out, uint32_t n);    /* 混 n 个 16 kHz 样本 */
int  dg_audio_active(void);                          /* 有通道在响 */

#endif
