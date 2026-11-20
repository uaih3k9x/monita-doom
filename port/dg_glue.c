/*
 * monita-doom 平台层里依赖 Doom 头文件、但和具体平台无关的部分 (设备 / QEMU / Mac 主机三份共用):
 *   - DG_BigAlloc: 原先静态的大数组 (visplanes 等, 见 doomgeneric 里的 MONITA-DOOM 注释) 从这里分配;
 *   - 调色板 -> RGB565 查找表 + 放大写帧缓冲 (dg_present);
 *   - 音效模块 DG_sound_module (混音在 dg_core.h, 输出由平台层取: dg_audio_render); 音乐模块是空的 (不放音乐)。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_misc.h"
#include "doomgeneric.h"

#include "dg_core.h"
#include "dg_glue.h"

/* ---- 大数组 ----
 * Z_Init 之前 (只有 states / mobjinfo 两张表) 用 calloc; 之后放进 zone (PU_STATIC, 不会被回收),
 * 这样平台层给 zone 定大小时不用再给这些"后来的"分配留余量。 */
static int g_zone_ready;
void dg_glue_zone_ready(void){ g_zone_ready = 1; }

void *DG_BigAlloc(size_t size, const char *what)
{
    void *p;
    if (g_zone_ready) {
        p = Z_Malloc((int)size, PU_STATIC, NULL);
        memset(p, 0, size);
    } else {
        p = calloc(1, size);
    }
    if (p == NULL)
        I_Error("DG_BigAlloc: %s (%u B) 分配失败", what, (unsigned)size);
    return p;
}

/* ---- 画面 ---- */
static uint16_t g_lut[256];

void dg_present(uint16_t *fb, uint32_t stride, const dgc_geom_t *g)
{
    if (palette_changed) {
        uint8_t rgb[768];
        for (int i = 0; i < 256; i++) {
            rgb[3 * i] = (uint8_t)colors[i].r;
            rgb[3 * i + 1] = (uint8_t)colors[i].g;
            rgb[3 * i + 2] = (uint8_t)colors[i].b;
        }
        dgc_lut_from_rgb(g_lut, rgb);
        palette_changed = false;
    }
    dgc_blit(fb, stride, g, (const uint8_t *)DG_ScreenBuffer, g_lut);
}

/* ---- 音效 ----
 * 每个通道记着它占着的 lump (PU_STATIC 锁住, 播完 / 被停后没有别的通道在用就放回 PU_CACHE, 让 zone 能回收)。 */
static dgc_mixer_t g_mix;
static int g_chan_lump[DGC_MIX_CH];
static boolean g_sfx_prefix;
static int g_sound_on;

static void release_finished(void)
{
    for (int c = 0; c < DGC_MIX_CH; c++) {
        int lump = g_chan_lump[c];
        if (lump < 0 || dgc_mix_playing(&g_mix, c))
            continue;
        g_chan_lump[c] = -1;
        int used = 0;
        for (int k = 0; k < DGC_MIX_CH; k++)
            if (g_chan_lump[k] == lump) used = 1;
        if (!used)
            W_ReleaseLumpNum(lump);
    }
}

static boolean snd_init(boolean use_sfx_prefix)
{
    dgc_mix_init(&g_mix);
    for (int c = 0; c < DGC_MIX_CH; c++) g_chan_lump[c] = -1;
    g_sfx_prefix = use_sfx_prefix;
    g_sound_on = 1;
    return true;
}

static void snd_shutdown(void)
{
    for (int c = 0; c < DGC_MIX_CH; c++) dgc_mix_stop(&g_mix, c);
    release_finished();
    g_sound_on = 0;
}

static int snd_lumpnum(sfxinfo_t *sfx)
{
    char name[16];
    if (sfx->link != NULL)
        sfx = sfx->link;
    if (g_sfx_prefix)
        M_snprintf(name, sizeof name, "ds%s", sfx->name);
    else
        M_StringCopy(name, sfx->name, sizeof name);
    return W_CheckNumForName(name);     /* 没有这个音效 (-1) 时 S_StartSound 不会再调我们 */
}

static void snd_update(void){ release_finished(); }

static void snd_params(int c, int vol, int sep)
{
    (void)sep;                           /* 单声道喇叭: 不做左右 */
    dgc_mix_set_vol(&g_mix, c, vol);
}

static int snd_start(sfxinfo_t *sfx, int c, int vol, int sep)
{
    (void)sep;
    if (c < 0 || c >= DGC_MIX_CH || sfx->lumpnum < 0)
        return -1;
    dgc_mix_stop(&g_mix, c);
    release_finished();
    const uint8_t *data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    int len = W_LumpLength(sfx->lumpnum);
    if (dgc_mix_start(&g_mix, c, data, (uint32_t)len, vol) != 0) {
        W_ReleaseLumpNum(sfx->lumpnum);
        return -1;
    }
    g_chan_lump[c] = sfx->lumpnum;
    return c;
}

static void snd_stop(int c)
{
    dgc_mix_stop(&g_mix, c);
    release_finished();
}

static boolean snd_playing(int c){ return dgc_mix_playing(&g_mix, c) ? true : false; }

static snddevice_t snd_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                     SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

sound_module_t DG_sound_module = {
    snd_devices, sizeof snd_devices / sizeof snd_devices[0],
    snd_init, snd_shutdown, snd_lumpnum, snd_update, snd_params, snd_start, snd_stop, snd_playing, NULL,
};

void dg_audio_render(int16_t *out, uint32_t n)
{
    if (!g_sound_on) {
        for (uint32_t i = 0; i < n; i++) out[i] = 0;
        return;
    }
    dgc_mix(&g_mix, out, n);
}

int dg_audio_active(void)
{
    for (int c = 0; c < DGC_MIX_CH; c++)
        if (dgc_mix_playing(&g_mix, c)) return 1;
    return 0;
}

/* 音乐: 不做 (OPL / MIDI 合成在 500 MHz、没有浮点的单核上不划算, 见 README) */
static boolean mus_init(void){ return true; }
static void mus_void(void){ }
static void mus_vol(int v){ (void)v; }
static void *mus_reg(void *d, int len){ (void)d; (void)len; return (void *)1; }
static void mus_unreg(void *h){ (void)h; }
static void mus_play(void *h, boolean loop){ (void)h; (void)loop; }
static boolean mus_playing(void){ return false; }

music_module_t DG_music_module = {
    NULL, 0, mus_init, mus_void, mus_vol, mus_void, mus_void, mus_reg, mus_unreg, mus_play, mus_void, mus_playing, NULL,
};

/* i_sound.c 的 I_BindSoundVariables 会绑这两个配置项 (原本在 SDL 音效模块里定义); 我们不用 libsamplerate */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
