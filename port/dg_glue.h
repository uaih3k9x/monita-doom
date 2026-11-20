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

/* 对拍用的逐帧 CRC (CRC-32/IEEE): 8 位画面 64000 B + 调色板 768 B (r,g,b), 不看放大后的帧缓冲。
 * "CRC 链" = 每帧 CRC 按小端 4 字节依次再算 CRC-32, 一行就能对整段 timedemo (初值 *chain = 0)。 */
uint32_t dg_frame_crc(void);
void     dg_crc_chain(uint32_t *chain, uint32_t frame_crc);
/* 通用 CRC-32: with_pal = 1 时在 p 后面接着算调色板 */
uint32_t dg_crc32(uint32_t crc, const uint8_t *p, uint32_t n, int with_pal);

#endif
