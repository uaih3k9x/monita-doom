/*
 * monita-doom 平台层的纯逻辑部分: 调色板 -> RGB565、放大写帧缓冲、按键队列、音效混音。
 * 不依赖 Doom 头文件也不依赖 R05 SDK; 设备、QEMU、Mac 主机三份平台层共用, 主机单测 port/tests/test_core.c。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MONITA_DOOM_DG_CORE_H
#define MONITA_DOOM_DG_CORE_H
#include <stdint.h>

#define DGC_W 320u                 /* Doom 原生画面 */
#define DGC_H 200u

/* ---- 调色板 ---- */
/* RGB565 小端、R 在高位 (实机: 0xF800 显示为红, 见 Monita docs/r05-notes.md §4) */
static inline uint16_t dgc_rgb565(uint32_t r, uint32_t g, uint32_t b){
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}
/* rgb: 256 组 r,g,b */
static inline void dgc_lut_from_rgb(uint16_t lut[256], const uint8_t *rgb){
    for (int i = 0; i < 256; i++, rgb += 3) lut[i] = dgc_rgb565(rgb[0], rgb[1], rgb[2]);
}

/* ---- 放大 ----
 * SCALE2: 2× = 640×400; SCALE25: 2.5× = 800×500 (行 3/2 交替、列 a a a b b 交替, 最近邻)。
 * 都放在屏幕上半部 (0..639 行) 居中。 */
enum { DGC_SCALE2 = 0, DGC_SCALE25 = 1 };
typedef struct { int mode; uint32_t w, h, x0, y0; } dgc_geom_t;

static inline dgc_geom_t dgc_geom(int mode, uint32_t fbw, uint32_t fbh){
    dgc_geom_t g;
    g.mode = mode == DGC_SCALE25 ? DGC_SCALE25 : DGC_SCALE2;
    g.w = g.mode == DGC_SCALE25 ? 800u : 640u;
    g.h = g.mode == DGC_SCALE25 ? 500u : 400u;
    g.x0 = (fbw - g.w) / 2u;
    g.y0 = (fbh / 2u - g.h) / 2u;
    return g;
}

static inline void dgc_row_copy(uint16_t *d, const uint16_t *s, uint32_t npx){
    uint32_t *d32 = (uint32_t *)d; const uint32_t *s32 = (const uint32_t *)s;
    for (uint32_t i = 0; i < npx / 2u; i++) d32[i] = s32[i];
}

/* fb: 帧缓冲首地址 (u16, 行跨 stride 个像素); 目标区左上角 4 字节对齐 (x0 偶数) */
static inline void dgc_blit(uint16_t *fb, uint32_t stride, const dgc_geom_t *g, const uint8_t *src, const uint16_t *lut){
    uint16_t *row = fb + g->y0 * stride + g->x0;
    if (g->mode == DGC_SCALE2){
        for (uint32_t y = 0; y < DGC_H; y++, src += DGC_W, row += 2u * stride){
            uint32_t *d0 = (uint32_t *)row, *d1 = (uint32_t *)(row + stride);
            for (uint32_t x = 0; x < DGC_W; x++){
                uint32_t p = lut[src[x]];
                p |= p << 16;
                d0[x] = p; d1[x] = p;
            }
        }
        return;
    }
    for (uint32_t y = 0; y < DGC_H; y++, src += DGC_W){
        uint32_t *d = (uint32_t *)row;
        for (uint32_t x = 0; x < DGC_W; x += 4u, d += 5){
            uint32_t a = lut[src[x]], b = lut[src[x + 1u]], c = lut[src[x + 2u]], e = lut[src[x + 3u]];
            d[0] = a | a << 16; d[1] = a | b << 16; d[2] = b | c << 16; d[3] = c | c << 16; d[4] = e | e << 16;
        }
        uint32_t rep = (y & 1u) ? 2u : 3u;           /* 源偶数行 -> 3 行, 奇数行 -> 2 行 */
        for (uint32_t k = 1; k < rep; k++) dgc_row_copy(row + k * stride, row, g->w);
        row += rep * stride;
    }
}

/* ---- 按键队列 ----
 * 事件来源: 主机 (USB 转发的键盘) 和前面板。主机按住的键记在位图里, 主机走了 (链路超时) 时 dgc_key_release_host
 * 补发抬起, 免得角色一直往前走。键码就是 Doom 的 (doomkeys.h), 0 不用。 */
#define DGC_KEYQ 64u
enum { DGC_SRC_HOST = 0, DGC_SRC_PANEL = 1 };
typedef struct {
    uint8_t  key[DGC_KEYQ], down[DGC_KEYQ];
    uint32_t rd, wr;
    uint32_t held[8];              /* 主机按住的键 (256 位) */
} dgc_keyq_t;

static inline void dgc_keyq_init(dgc_keyq_t *q){
    uint8_t *p = (uint8_t *)q;
    for (uint32_t i = 0; i < sizeof *q; i++) p[i] = 0;
}
/* 返回 1 = 收下, 0 = 满了 / 键码 0 */
static inline int dgc_key_push(dgc_keyq_t *q, uint8_t key, int down, int src){
    if (!key || q->wr - q->rd >= DGC_KEYQ - 1u) return 0;
    uint32_t i = q->wr % DGC_KEYQ;
    q->key[i] = key; q->down[i] = down ? 1u : 0u;
    q->wr++;
    if (src == DGC_SRC_HOST){
        if (down) q->held[key >> 5] |= 1u << (key & 31u);
        else q->held[key >> 5] &= ~(1u << (key & 31u));
    }
    return 1;
}
static inline int dgc_key_pop(dgc_keyq_t *q, int *down, uint8_t *key){
    if (q->rd == q->wr) return 0;
    uint32_t i = q->rd % DGC_KEYQ;
    *key = q->key[i]; *down = q->down[i];
    q->rd++;
    return 1;
}
/* 主机按住的全部补发抬起; 返回补发了几个 (队列满时剩下的留着下次) */
static inline int dgc_key_release_host(dgc_keyq_t *q){
    int n = 0;
    for (uint32_t k = 1; k < 256u; k++){
        if (!(q->held[k >> 5] & (1u << (k & 31u)))) continue;
        if (!dgc_key_push(q, (uint8_t)k, 0, DGC_SRC_HOST)) break;
        n++;
    }
    return n;
}

/* ---- 音效混音 ----
 * Doom 音效 lump (DMX 格式): u16 3, u16 采样率, u32 n, 然后 n 个 u8 无符号 PCM; DMX 播放时跳过前后各 16 字节 (同 Chocolate Doom)。
 * 输出 16 kHz 单声道 s16 (Monita 喇叭路径只收这一种), 最近邻重采样 (16.16 定点步长), 每路 (u8 - 128) × vol (0..127), 叠加后钳位。
 * 不做立体声 (喇叭只有一个)。 */
#define DGC_MIX_RATE 16000u
#define DGC_MIX_CH   8
typedef struct {
    const uint8_t *data;           /* 0 = 空闲 */
    uint32_t len, pos, step;       /* pos / step: 16.16 */
    int32_t  vol;
} dgc_chan_t;
typedef struct { dgc_chan_t ch[DGC_MIX_CH]; } dgc_mixer_t;

static inline void dgc_mix_init(dgc_mixer_t *m){
    uint8_t *p = (uint8_t *)m;
    for (uint32_t i = 0; i < sizeof *m; i++) p[i] = 0;
}
/* 返回 0 = 开始播放; <0 = 通道号不对 / lump 不是 DMX 音效 */
static inline int dgc_mix_start(dgc_mixer_t *m, int c, const uint8_t *lump, uint32_t lumplen, int vol){
    if (c < 0 || c >= DGC_MIX_CH) return -1;
    if (lumplen < 8u || lump[0] != 3 || lump[1] != 0) return -2;
    uint32_t rate = (uint32_t)lump[2] | (uint32_t)lump[3] << 8;
    uint32_t n = (uint32_t)lump[4] | (uint32_t)lump[5] << 8 | (uint32_t)lump[6] << 16 | (uint32_t)lump[7] << 24;
    if (n <= 32u || n > lumplen - 8u || rate < 4000u || rate > 48000u) return -3;
    dgc_chan_t *h = &m->ch[c];
    h->data = lump + 8u + 16u; h->len = n - 32u; h->pos = 0;
    h->step = (rate << 16) / DGC_MIX_RATE;
    h->vol = vol < 0 ? 0 : vol > 127 ? 127 : vol;
    return 0;
}
static inline void dgc_mix_stop(dgc_mixer_t *m, int c){ if (c >= 0 && c < DGC_MIX_CH) m->ch[c].data = 0; }
static inline int  dgc_mix_playing(const dgc_mixer_t *m, int c){ return c >= 0 && c < DGC_MIX_CH && m->ch[c].data != 0; }
static inline void dgc_mix_set_vol(dgc_mixer_t *m, int c, int vol){
    if (c >= 0 && c < DGC_MIX_CH) m->ch[c].vol = vol < 0 ? 0 : vol > 127 ? 127 : vol;
}
/* 混 count 个样本进 out (覆盖写) */
static inline void dgc_mix(dgc_mixer_t *m, int16_t *out, uint32_t count){
    for (uint32_t i = 0; i < count; i++) out[i] = 0;
    for (int c = 0; c < DGC_MIX_CH; c++){
        dgc_chan_t *h = &m->ch[c];
        if (!h->data) continue;
        for (uint32_t i = 0; i < count; i++){
            uint32_t k = h->pos >> 16;
            if (k >= h->len){ h->data = 0; break; }
            int32_t s = out[i] + ((int32_t)h->data[k] - 128) * h->vol;
            out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            h->pos += h->step;
        }
        if (h->data && (h->pos >> 16) >= h->len) h->data = 0;
    }
}

#endif
