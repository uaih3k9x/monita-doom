/*
 * 无界面平台层: Mac 主机版 (port/host) 和 QEMU 测速版 (qemu/) 共用。
 *   - 画面写进一块和设备一样的 800×1280 RGB565 "帧缓冲" (同一个 dg_present), 每帧打印一行 CRC (8 位画面 + 调色板),
 *     主机版和 QEMU 版逐帧对拍; 主机版还能把指定帧存成 PPM、把混音存成 WAV。
 *   - 时钟是虚拟的 (每帧 +28 ms, DG_SleepMs 照加), 所以同样的参数每次跑出来的帧序列一模一样 (含屏幕溶解效果)。
 *
 * 参数 (其余原样交给 Doom):
 *   -zonekb N      zone 大小 (KiB), 默认 2560 (= 设备上的取值, 见 README "内存预算")
 *   -scale25       2.5× 放大 (默认 2×)
 *   -maxframes N   跑到第 N 帧就退出
 *   -crc           每帧打印 "CRC <帧号> <crc32>", 退出时打印 "CRC 链 <crc32> <帧数>" (设备测速模式打印同一行, 见 dg_glue.h)
 *   -rowcrc N      第 N 帧打印调色板 CRC 和每行画面 CRC ("ROW <行> <crc32>"), 对拍时定位差异
 *   -dump a,b,..   (主机版) 这些帧存成 frame_<n>.ppm (屏幕上半部 800×640)
 *   -wav FILE      (主机版) 混音输出存成 16 kHz 单声道 WAV
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "doomgeneric.h"
#include "doomtype.h"
#include "i_video.h"
#include "dg_glue.h"

#define FBW 800u
#define FBH 1280u

static uint16_t *g_fb;
static dgc_geom_t g_geom;
static uint32_t g_vclock_ms;
static uint32_t g_frame;
static int g_zonekb = 2560, g_maxframes, g_crc, g_rowcrc = -1;
static uint32_t g_chain;
#ifdef DG_HOST
static int g_dump[64], g_ndump;
static FILE *g_wav;
static uint32_t g_wav_n;
#endif

/* QEMU 测速版: 插件按这些函数的地址分段计数 (qemu/run.sh), noinline 保证地址存在 */
__attribute__((noinline)) void m_frame(void){ __asm__ volatile(""); }
__attribute__((noinline)) void m_pause(void){ __asm__ volatile(""); }
__attribute__((noinline)) void m_resume(void){ __asm__ volatile(""); }

#ifdef DG_HOST
static void wav_hdr(FILE *f, uint32_t nsamp){
    uint8_t h[44];
    uint32_t data = nsamp * 2u;
    memcpy(h, "RIFF", 4); h[4] = (uint8_t)(data + 36); h[5] = (uint8_t)((data + 36) >> 8); h[6] = (uint8_t)((data + 36) >> 16); h[7] = (uint8_t)((data + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint8_t fmt[20] = { 16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 2, 0, 16, 0 };
    memcpy(h + 16, fmt, 20);
    memcpy(h + 36, "data", 4); h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8); h[42] = (uint8_t)(data >> 16); h[43] = (uint8_t)(data >> 24);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
}
static void wav_close(void){ if (g_wav){ wav_hdr(g_wav, g_wav_n); fclose(g_wav); g_wav = NULL; } }

static void dump_ppm(uint32_t n){
    char name[64];
    snprintf(name, sizeof name, "frame_%05u.ppm", (unsigned)n);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", FBW, FBH / 2u);
    for (uint32_t i = 0; i < FBW * FBH / 2u; i++){
        uint16_t p = g_fb[i];
        uint8_t rgb[3] = { (uint8_t)((p >> 8) & 0xF8u), (uint8_t)((p >> 3) & 0xFCu), (uint8_t)(p << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
#endif

byte *DG_ZoneAlloc(int *size){
    *size = g_zonekb * 1024;
    byte *p = malloc((size_t)*size);
    if (p) dg_glue_zone_ready();
    return p;
}

void DG_Init(void){
    g_fb = calloc(FBW * FBH, sizeof *g_fb);
}

void DG_DrawFrame(void){
    dg_present(g_fb, FBW, &g_geom);
    m_frame();                                     /* 一帧 (游戏逻辑 + 渲染 + 放大) 在这里结束 */
    m_pause();                                     /* 下面是测量用的附加活, 不计入 */
    g_frame++;
    if (g_crc){
        uint32_t c = dg_frame_crc();
        dg_crc_chain(&g_chain, c);
        printf("CRC %u %08x\n", (unsigned)g_frame, (unsigned)c);
    }
    if ((uint32_t)g_rowcrc == g_frame){
        printf("PAL %08x\n", (unsigned)dg_crc32(0, NULL, 0, 1));
        for (uint32_t y = 0; y < DGC_H; y++)
            printf("ROW %u %08x\n", (unsigned)y, (unsigned)dg_crc32(0, (const uint8_t *)DG_ScreenBuffer + y * DGC_W, DGC_W, 0));
    }
#ifdef DG_HOST
    for (int i = 0; i < g_ndump; i++) if ((uint32_t)g_dump[i] == g_frame) dump_ppm(g_frame);
    if (g_wav){
        int16_t buf[460];
        uint32_t n = (g_frame * 16000u) / 35u - (g_wav_n);    /* 按每帧一个游戏 tic 算 (timedemo 就是这样) */
        if (n > 460) n = 460;
        dg_audio_render(buf, n);
        fwrite(buf, 2, n, g_wav);
        g_wav_n += n;
    }
#endif
    g_vclock_ms += 28u;
    m_resume();
    if (g_maxframes && g_frame >= (uint32_t)g_maxframes){
        printf("maxframes %d reached\n", g_maxframes);
        exit(0);
    }
}

void DG_SleepMs(uint32_t ms){ g_vclock_ms += ms; }
uint32_t DG_GetTicksMs(void){ return g_vclock_ms; }
int DG_GetKey(int *pressed, unsigned char *key){ (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char *title){ (void)title; }

static void print_chain(void){ printf("CRC 链 %08x %u\n", (unsigned)g_chain, (unsigned)g_frame); }

static int take_arg(int *argc, char **argv, int i, int n){
    for (int k = i; k + n < *argc; k++) argv[k] = argv[k + n];
    *argc -= n;
    return 0;
}

int main(int argc, char **argv){
    int mode = DGC_SCALE2;
    for (int i = 1; i < argc;){
        if (!strcmp(argv[i], "-zonekb") && i + 1 < argc){ g_zonekb = atoi(argv[i + 1]); take_arg(&argc, argv, i, 2); }
        else if (!strcmp(argv[i], "-scale25")){ mode = DGC_SCALE25; take_arg(&argc, argv, i, 1); }
        else if (!strcmp(argv[i], "-maxframes") && i + 1 < argc){ g_maxframes = atoi(argv[i + 1]); take_arg(&argc, argv, i, 2); }
        else if (!strcmp(argv[i], "-crc")){ g_crc = 1; atexit(print_chain); take_arg(&argc, argv, i, 1); }
        else if (!strcmp(argv[i], "-rowcrc") && i + 1 < argc){ g_rowcrc = atoi(argv[i + 1]); take_arg(&argc, argv, i, 2); }
#ifdef DG_HOST
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc){
            char *s = argv[i + 1];
            while (*s && g_ndump < 64){ g_dump[g_ndump++] = atoi(s); while (*s && *s != ',') s++; if (*s) s++; }
            take_arg(&argc, argv, i, 2);
        }
        else if (!strcmp(argv[i], "-wav") && i + 1 < argc){
            g_wav = fopen(argv[i + 1], "wb");
            if (g_wav){ wav_hdr(g_wav, 0); atexit(wav_close); }
            take_arg(&argc, argv, i, 2);
        }
#endif
        else i++;
    }
    g_geom = dgc_geom(mode, FBW, FBH);
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}
