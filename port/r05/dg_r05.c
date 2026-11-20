/*
 * monita-doom 设备平台层 (R05 app, 写进 app 槽 0x60270000)。
 *
 * 线程模型 (照 Monita): appimg_enter 只配看门狗 + 建任务就返回; 所有事都在 "doom" 任务 (prio 12, 栈 16 KiB = 原厂 ql_init)
 *   里跑。USB 回调 (内核线程) 只置一个 volatile 标志; 读 USB、采按键、喂喇叭、喂看门狗都在 Doom 的
 *   DG_DrawFrame / DG_SleepMs / DG_GetKey 里顺手做 (poll_io), 不另开任务, 不需要锁。
 *
 * 画面: 单缓冲, 直接画在原厂帧缓冲 FB_ADDR (0x80de0000, 不在内核堆里); 320×200 8 位 -> 查表 RGB565 -> 2× 放大
 *   640×400, 放在竖屏上半部居中; 写完 clean D-cache。不等帧头 (会有撕裂, 见 README "还没做")。
 * 内存: zone 一整块从内核堆拿, 先查 ql_dev_memory_size_query, 不够就不启动 (dl_zone_check)。
 *   引擎的 .data / .bss 在 app 的 128 KiB RAM 窗口里 (app.ld), 其余零碎 malloc 走内核堆。
 * 文件: 外挂 flash EFS: 只读 (dg_efs.h); WAD 依次找 EFS:/monita/doom1.wad、freedoom1.wad。存档 / 配置写不了 (libc 只读)。
 * 输入: USB 上主机转发的键 (dg_link.h) + 前面板 4 键 (monita/keys_hw.h, dg_link.h 的 dl_panel)。
 * 声音: 只有 DG_SOUND_HW (有声版 doom_snd.bin) 才编进来; 音效混成 16 kHz 单声道喂给 monita/audio.h 的环形缓冲。
 * 测速: 开机时按住 ≡ -> -timedemo demo3; 结束时日志里打 "CRC 链" (应等于 QEMU 的 2e1acd90) 和帧率, 屏上显示帧率。
 * 安全模式: 按住 − 上电 -> 黄色 "--", 不挂 EFS、不启动 Doom, 只喂狗和响应 USB (万一 Doom 版开机就崩, 用它停住设备再刷)。
 * 让出 CPU: 一帧里 Doom 自己没睡过 (测速、或跑不满 35 fps 时) 就在帧末睡 1 ms, 别让 prio 12 的任务一直占着。
 * 状态显示 (屏幕下方的大字): "-" 等 3 s (给刷机工具留进下载模式的窗口) / "E2" 内存不够 / "E3" 找不到 WAD /
 *   "E4" Doom 出错退出 / 数字 = 测速帧率。详细原因看 USB 日志 (host/doomkeys)。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "doomgeneric.h"
#include "doomtype.h"
#include "i_system.h"
#include "doomstat.h"
#include "dg_glue.h"
#include "dgl_backend.h"

/* 内核的 malloc / free 导入成 r05_kmalloc / r05_kfree (doom.imports 里按哈希点名), 名字让给 libc 的 malloc */
#define malloc r05_kmalloc
#define free   r05_kfree
#include "monita/r05_api.h"
#include "monita/lcd.h"
#include "monita/keys_hw.h"
#ifdef DG_SOUND_HW
#include "monita/audio_hw.h"
#endif
#undef malloc
#undef free

#include "dg_link.h"
#include "dg_efs.h"

/* ---- EFS: 只读接口 (用法同 extdump v2, 真机跑过) ---- */
extern int ql_spi6_ext_nor_flash_init(void);
extern int ql_spi6_ext_nor_flash_sffs_mount(void);        /* 内部 mount(0): 挂不上只报错, 不格式化 */
extern int ql_fopen(const char *path, const char *mode);
extern int ql_fread(void *buf, uint32_t size, uint32_t count, int fd);
extern int ql_fseek(int fd, int32_t offset, int32_t whence);
extern int ql_fclose(int fd);
extern int ql_fsize(int fd);
static const efs_ops_t g_efs_ops = { ql_fopen, ql_fread, ql_fseek, ql_fclose, ql_fsize };

#define TASK_STACK   16384u
#define BOOT_WAIT_MS 3000u
#define PORT         R05_PORT_USB_MODEM
#define KEY_GPIO_MENU  21u                /* ≡ (monita/keys_hw.h) */
#define KEY_GPIO_MINUS 23u                /* − */

static void   *g_task;
static dl_t    g_link;
static dgc_keyq_t g_keyq;
static keys_t  g_keys;
static pwr_t   g_pwr;
static int     g_keys_ok, g_bench;
static uint32_t g_key_at, g_wdt_at;
static volatile uint32_t g_rx_flag;
static uint8_t g_rx[512];
static dgc_geom_t g_geom;

static uint32_t now_us(void){ return (uint32_t)osiUpTimeUS(); }        /* 只做差 */
static uint32_t now_ms(void){ return (uint32_t)(osiUpTimeUS() / 1000); }

/* ---- 屏幕中间的状态大字 (3×5 点阵, 自绘) ---- */
static const uint16_t g_font[] = {          /* 0-9 . E - : 每字 15 位, 行优先、高位在左上 */
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717, 000002, 074747, 000700,
};
static int glyph(char c){
    if (c >= '0' && c <= '9') return c - '0';
    return c == '.' ? 10 : c == 'E' ? 11 : c == '-' ? 12 : -1;
}
static void draw_status(const char *s, uint16_t color){
    const uint32_t px = 24, cw = 4 * px;                    /* 一点 24×24 像素, 字距 1 点 */
    uint32_t n = (uint32_t)strlen(s), w = n * cw, x0 = (LCD_W - w) / 2u, y0 = 1180u - 5u * px;   /* 屏幕下半部, 不挡游戏画面 */
    uint16_t *fb = (uint16_t *)FB_ADDR;
    for (uint32_t y = y0; y < y0 + 5u * px; y++)
        for (uint32_t x = 0; x < LCD_W; x++) fb[y * LCD_W + x] = 0;
    for (uint32_t i = 0; i < n; i++){
        int g = glyph(s[i]);
        if (g < 0) continue;
        for (uint32_t r = 0; r < 5; r++)
            for (uint32_t c = 0; c < 3; c++){
                if (!(g_font[g] & (1u << (14u - (r * 3u + c))))) continue;
                for (uint32_t yy = 0; yy < px; yy++)
                    for (uint32_t xx = 0; xx < px; xx++)
                        fb[(y0 + r * px + yy) * LCD_W + x0 + i * cw + c * px + xx] = color;
            }
    }
    dcache_clean(FB_ADDR + y0 * LCD_W * 2u, 5u * px * LCD_W * 2u);
}

/* ---- USB / 按键 / 喇叭 / 看门狗: 在 Doom 的回调里顺手做 ---- */
static void uart_cb(uint32_t ind, uint32_t port, uint32_t size){
    (void)size;
    if (port == PORT && (ind == R05_UART_IND_RX || ind == R05_UART_IND_RX2)) g_rx_flag = 1;
}

#ifdef DG_SOUND_HW
static uint8_t  g_au_ring[AU_RING_BYTES];
static au_t     g_au;
static int      g_audio_ok;
static uint32_t g_au_t;
static const au_io_t g_auio = { au_hw_open, au_hw_write, au_hw_close, au_hw_pa, 0 };
static void audio_poll(void){
    if (!g_audio_ok) return;
    uint32_t t = now_us(), dt = t - g_au_t;
    if (dt > 100000u) dt = 100000u;
    uint32_t n = (uint32_t)(((uint64_t)dt * AU_RATE) / 1000000u);
    if (dg_audio_active() && n){
        if (g_au.state == AU_IDLE){
            static const uint8_t start[8] = { 0x80, 0x3e, 0, 0, 1, AU_FMT_S16LE, 100, 0 };   /* 16000 Hz, 1 声道, 音量 100 */
            au_start(&g_au, start, sizeof start, t);
        }
        static int16_t buf[1600];
        dg_audio_render(buf, n);
        au_data(&g_au, (const uint8_t *)buf, n * 2u, t);    /* 小端: 内存里就是 s16le */
    }
    if (n) g_au_t += (uint32_t)(((uint64_t)n * 1000000u) / AU_RATE);
    if (t - g_au_t > 100000u) g_au_t = t;
    au_poll(&g_au, &g_auio, t);
}
#define FLAGS DL_FLAG_SOUND
#else
static void audio_poll(void){ }
#define FLAGS 0u
#endif

static void link_poll(void){
    uint32_t t = now_ms();
    if (g_rx_flag){
        g_rx_flag = 0;                                       /* 先清再读: 读的时候又来的数据会再置位 */
        for (;;){
            int r = ql_uart_read(PORT, g_rx, sizeof g_rx);
            if (r <= 0) break;
            dl_rx(&g_link, g_rx, (uint32_t)r, t, &g_keyq);
        }
    }
    dl_tick(&g_link, t, &g_keyq);
    uint8_t out[DL_TX_MAX];
    for (int i = 0; i < 4; i++){                             /* 每轮最多 4 条 (≈ 1 KiB), 不让日志拖慢一帧 */
        uint32_t n = dl_tx(&g_link, out, sizeof out, t);
        if (!n) break;
        ql_uart_write(PORT, out, n);
    }
}

static void keys_poll(void){
    if (!g_keys_ok) return;
    uint32_t t = (uint32_t)(osiUpTimeUS() >> 10);           /* monita/keys.h 用 1.024 ms 单位 */
    if (t - g_key_at < 8u) return;
    g_key_at = t;
    uint8_t ev[8];
    int n = keys_feed(&g_keys, t, keys_hw_raw(&g_pwr, t), ev, 8);
    if (g_bench) return;                                     /* 测速: 开机按住的 ≡ 会长按出 Esc, 回放中弹菜单就对不上 CRC 了 */
    for (int i = 0; i < n; i++) dl_panel(&g_keyq, ev[i], menuactive);
}

/* 每秒喂一次 (同 Monita)。除了每帧, 读文件时也调: 启动和换关时 Doom 连着读一大串 lump, 中间不出帧 */
static void wdt_poll(void){
    uint32_t t = now_ms();
    if (t - g_wdt_at >= 1000u){ ql_dev_feed_wdt(); g_wdt_at = t; }
}

static void poll_io(void){
    wdt_poll();
    link_poll();
    keys_poll();
    audio_poll();
}

/* 停在这里: 屏上留着状态, 照样喂狗、照样回 USB (主机能把日志读走), 等人来重刷 / 断电 */
static void halt(const char *status, uint16_t color) __attribute__((noreturn));
static void halt(const char *status, uint16_t color){
    draw_status(status, color);
    for (;;){ poll_io(); ql_rtos_task_sleep_ms(20); }
}

/* ---- port/libc 的后端 ---- */
void dgl_log_write(const char *s, size_t n){ dl_log(&g_link, s, n); }
int  dgl_fs_open(const char *path){ return efs_open(path); }
long dgl_fs_size(int h){ return efs_size(h); }
void dgl_fs_close(int h){ efs_close(h); }
int  dgl_fs_read(int h, uint32_t off, void *buf, uint32_t n){ wdt_poll(); return efs_read(h, off, buf, n); }

static uint32_t g_frames, g_t0_ms, g_chain, g_yield_us, g_yields;
static int g_slept;
void dgl_exit(int code){
    if (g_bench){                                            /* timedemo 结束 (I_Error "timed ...", code -1) */
        uint32_t ms = now_ms() - g_t0_ms, n = g_frames ? g_frames - 1u : 0, yms = g_yield_us / 1000u;
        uint32_t f10 = ms ? (uint32_t)((uint64_t)n * 10000u / ms) : 0;
        uint32_t g10 = ms > yms ? (uint32_t)((uint64_t)n * 10000u / (ms - yms)) : 0;
        printf("CRC 链 %08x %u\n", (unsigned)g_chain, (unsigned)g_frames);
        printf("让出 CPU %u 次共 %u ms; 扣掉之后 %u.%u fps\n", (unsigned)g_yields, (unsigned)yms, (unsigned)(g10 / 10u), (unsigned)(g10 % 10u));
        printf("测速: 第一帧之后 %u 帧 / %u ms = %u.%u fps (每帧含逻辑、渲染、放大、clean cache、轮询 USB)\n", (unsigned)n, (unsigned)ms,
               (unsigned)(f10 / 10u), (unsigned)(f10 % 10u));
        char s[16];
        snprintf(s, sizeof s, "%u.%u", (unsigned)(f10 / 10u), (unsigned)(f10 % 10u));
        halt(s, 0x07E0);
    }
    printf("exit %d\n", code);
    halt(code ? "E4" : "0", code ? 0xF800 : 0xFFFF);
}

/* 内核 malloc 没有 realloc / 不知道块大小: 每块前面 8 字节记大小 (保持内核给的对齐) */
void *malloc(size_t n){
    uint32_t *p = r05_kmalloc((uint32_t)n + 8u);
    if (!p) return NULL;
    p[0] = (uint32_t)n;
    return p + 2;
}
void free(void *p){ if (p) r05_kfree((uint32_t *)p - 2); }
void *calloc(size_t n, size_t m){
    if (m && n > 0xFFFFFFF0u / m) return NULL;
    void *p = malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}
void *realloc(void *p, size_t n){
    if (!p) return malloc(n);
    uint32_t old = ((uint32_t *)p)[-2];
    if (n <= old) return p;
    void *q = malloc(n);
    if (q){ memcpy(q, p, old); free(p); }
    return q;
}

/* ---- doomgeneric 回调 ---- */
byte *DG_ZoneAlloc(int *size){
    uint32_t q[3] = { 0, 0, 0 };                             /* {total, avail, max_block} (Monita docs/r05-notes.md) */
    if (ql_dev_memory_size_query(q) != 0) q[1] = q[2] = 0;
    uint32_t zone = dl_zone_pick(q[1], q[2]);
    printf("内核堆: 总 %u KiB, 可用 %u KiB, 最大块 %u KiB; zone 取 %u KiB (2560 / 2048 / 1536 里放得下的最大一档), 分完还要剩 %u KiB\n",
           (unsigned)(q[0] >> 10), (unsigned)(q[1] >> 10), (unsigned)(q[2] >> 10), (unsigned)(zone >> 10), DL_RESERVE >> 10);
    if (!zone){ printf("内存不够, 不启动\n"); halt("E2", 0xF800); }
    byte *p = r05_kmalloc(zone);
    if (!p){ printf("zone malloc 失败\n"); halt("E2", 0xF800); }
    dg_glue_zone_ready();
    *size = (int)zone;
    return p;
}

void DG_Init(void){ g_geom = dgc_geom(DGC_SCALE2, LCD_W, LCD_H); }

void DG_DrawFrame(void){
    dg_present((uint16_t *)FB_ADDR, LCD_W, &g_geom);
    dcache_clean(FB_ADDR + g_geom.y0 * LCD_W * 2u, g_geom.h * LCD_W * 2u);
    if (g_bench){
        if (!g_frames++) g_t0_ms = now_ms();                 /* 第一帧 (含启动) 不计时 */
        dg_crc_chain(&g_chain, dg_frame_crc());
    }
    poll_io();
    if (!g_slept){                                           /* 这一帧 Doom 没睡过: 让出 1 ms */
        uint32_t t = now_us();
        ql_rtos_task_sleep_ms(1);
        g_yield_us += now_us() - t; g_yields++;
    }
    g_slept = 0;
}

void DG_SleepMs(uint32_t ms){ poll_io(); ql_rtos_task_sleep_ms(ms ? ms : 1); g_slept = 1; }
uint32_t DG_GetTicksMs(void){ return now_ms(); }
int DG_GetKey(int *pressed, unsigned char *key){
    int down; uint8_t k;
    if (!dgc_key_pop(&g_keyq, &down, &k) || g_bench) return 0;   /* 测速时主机键也不给 Doom */
    *pressed = down; *key = k;
    return 1;
}
void DG_SetWindowTitle(const char *title){ (void)title; }

/* ---- 任务 ---- */
static void task_entry(void *arg){
    (void)arg;
    dl_init(&g_link, FLAGS);
    dgc_keyq_init(&g_keyq);
    int lcd_err = lcd_power_on();                            /* 清黑、指好扫描基址, 背光还灭着 */
    if (lcd_err == 0){ draw_status("-", 0xFFFF); ql_rtos_task_sleep_ms(200); lcd_backlight(1); }

    ql_uart_open(PORT);
    ql_uart_register_cb(PORT, uart_cb);
    g_keys_ok = (keys_hw_init() & 7u) == 0;
    keys_init(&g_keys); pwr_init(&g_pwr);
    ql_rtos_task_sleep_ms(10);                               /* 等上拉稳定 */
    uint8_t lv = 1;
    if (ql_gpio_get_level(KEY_GPIO_MINUS, &lv) == 0 && lv == 0){
        printf("安全模式 (上电时按住了 -): 不启动 Doom\n");
        halt("--", 0xFFE0);
    }
#ifdef DG_SOUND_HW
    au_init(&g_au, g_au_ring, sizeof g_au_ring);
    g_audio_ok = audio_hw_init() == 0;
    g_au_t = now_us();
#endif
    printf("monita-doom (%s) 开机; 大屏 %s, 前面板键 %s\n", FLAGS ? "有声版" : "无声版",
           lcd_err ? "失败" : "OK", g_keys_ok ? "OK" : "失败");

    uint32_t t0 = now_ms();                                  /* 先等 3 s: 万一后面崩了反复重启, 刷机工具还有窗口 */
    while (now_ms() - t0 < BOOT_WAIT_MS){ poll_io(); ql_rtos_task_sleep_ms(20); }
    lv = 1;
    g_bench = ql_gpio_get_level(KEY_GPIO_MENU, &lv) == 0 && lv == 0;

    int r1 = ql_spi6_ext_nor_flash_init();
    int r2 = ql_spi6_ext_nor_flash_sffs_mount();
    printf("EFS: init %d, mount %d (同一次上电第二次 init 可能报错, 照样试着打开文件)\n", r1, r2);
    efs_init(&g_efs_ops);
    static const char *const wads[] = { "EFS:/monita/doom1.wad", "EFS:/monita/freedoom1.wad" };
    const char *wad = NULL;
    for (unsigned i = 0; i < sizeof wads / sizeof wads[0] && !wad; i++){
        int h = efs_open(wads[i]);
        if (h >= 0){ printf("WAD: %s (%ld B)\n", wads[i], efs_size(h)); efs_close(h); wad = wads[i]; }
    }
    if (!wad){ printf("找不到 WAD: 要放在 EFS:/monita/doom1.wad 或 freedoom1.wad\n"); halt("E3", 0xF800); }

    char *argv[] = { "doom", "-iwad", (char *)wad, "-timedemo", "demo3", NULL };
    int argc = g_bench ? 5 : 3;
    if (g_bench) printf("测速模式: -timedemo demo3\n");
    draw_status("", 0);
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
}

int appimg_enter(void *arg){
    (void)arg;
    ql_dev_cfg_wdt(1);                                       /* 同原厂 enter; 任务里每秒喂一次 */
    ql_rtos_task_create(&g_task, TASK_STACK, APP_PRIORITY_NORMAL, "doom", task_entry, 0, 1);
    return 0;
}

void appimg_exit(void){ }
