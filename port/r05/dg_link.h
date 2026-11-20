/*
 * 设备端 Doom 的 USB 链路和前面板键位 (纯逻辑, 主机单测 port/tests/test_r05_link.c)。
 *
 * 帧格式同 Monita (monita/proto.h): A5 5A | type | seq | u16 len | payload | crc32。只用这几种:
 *   主机 -> 设备  HELLO 0x01 (空)          设备回 HELLO_ACK, 然后把攒着的日志补发
 *                PING  0x7F (空)          转发器每 250 ms 一条, 只用来判断主机还在
 *                KEY   0xA0 u8 key, u8 down   key 是 Doom 键码 (doomkeys.h), 0 不用
 *   设备 -> 主机  HELLO_ACK 0x81  "DOOM" u16 版本 u16 flags (bit0 = 有声版)
 *                LOG   0xA8  printf 输出的原文 (UTF-8), 每条 ≤ DL_LOG_CHUNK 字节
 * 主机没来 (没收到过任何有效帧) 或走了 (DL_HOST_TIMEOUT_MS 没帧) 时设备一个字节都不发: 没人读的时候
 * ql_uart_write 会不会卡住没证实, 日志先进环形缓冲 (满了丢最旧的)。主机走时补发它按住的键的抬起。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MONITA_DOOM_DG_LINK_H
#define MONITA_DOOM_DG_LINK_H
#include <stdint.h>
#include <stddef.h>
#include "../dg_core.h"
#include "monita/proto.h"
#include "monita/keys.h"

#define DL_HELLO      0x01u
#define DL_PING       0x7Fu
#define DL_KEY        0xA0u
#define DL_HELLO_ACK  0x81u
#define DL_LOG        0xA8u

#define DL_VERSION         1u
#define DL_FLAG_SOUND      1u
#define DL_HOST_TIMEOUT_MS 1000u
#define DL_LOG_RING        8192u          /* 2 的幂 */
#define DL_LOG_CHUNK       256u
#define DL_TX_MAX          (LK_HDR + DL_LOG_CHUNK + LK_TRAIL)
#define DL_RX_PL           16u            /* 主机发来的帧都很短; 更长的按长度超限丢 */

typedef struct {
    lk_parser_t p;
    uint8_t  pl[DL_RX_PL];
    int      host_up, ack_due;
    uint32_t last_rx_ms, flags;
    char     log[DL_LOG_RING];
    uint32_t log_wr, log_rd, log_dropped;  /* 一直往上数, 下标 & (DL_LOG_RING - 1) */
    uint32_t keys_rx;
} dl_t;

static inline void dl_init(dl_t *l, uint32_t flags){
    uint8_t *z = (uint8_t *)l;
    for (size_t i = 0; i < sizeof *l; i++) z[i] = 0;
    lk_init(&l->p, l->pl, DL_RX_PL);
    l->flags = flags;
}

static inline void dl_log(dl_t *l, const char *s, size_t n){
    for (size_t i = 0; i < n; i++){
        if (l->log_wr - l->log_rd >= DL_LOG_RING){ l->log_rd++; l->log_dropped++; }
        l->log[l->log_wr & (DL_LOG_RING - 1u)] = s[i];
        l->log_wr++;
    }
}

static inline void dl_frame(dl_t *l, uint32_t now, dgc_keyq_t *q){
    l->host_up = 1;
    l->last_rx_ms = now;
    if (l->p.type == DL_HELLO) l->ack_due = 1;
    else if (l->p.type == DL_KEY && l->p.len == 2 && l->pl[0]){
        if (dgc_key_push(q, l->pl[0], l->pl[1] != 0, DGC_SRC_HOST)) l->keys_rx++;
    }
}

/* 收到的字节全部吃掉; 拆出的帧马上处理 */
static inline void dl_rx(dl_t *l, const uint8_t *d, uint32_t n, uint32_t now, dgc_keyq_t *q){
    while (n){
        int ev;
        uint32_t k = lk_feed(&l->p, d, n, &ev);
        d += k; n -= k;
        if (ev == LK_EV_FRAME) dl_frame(l, now, q);
    }
}

/* 主循环每轮: 主机超时 -> 走了, 补发它按住的键的抬起 */
static inline void dl_tick(dl_t *l, uint32_t now, dgc_keyq_t *q){
    if (l->host_up && now - l->last_rx_ms > DL_HOST_TIMEOUT_MS){
        l->host_up = 0;
        l->ack_due = 0;
        dgc_key_release_host(q);
        lk_reset(&l->p);                  /* 半截帧作废 */
    }
}

/* 下一条要发的帧写进 out (cap ≥ DL_TX_MAX), 返回长度; 0 = 没有要发的 */
static inline uint32_t dl_tx(dl_t *l, uint8_t *out, uint32_t cap, uint32_t now){
    (void)now;
    uint8_t pl[DL_LOG_CHUNK];
    if (!l->host_up || cap < DL_TX_MAX) return 0;
    if (l->ack_due){
        l->ack_due = 0;
        pl[0] = 'D'; pl[1] = 'O'; pl[2] = 'O'; pl[3] = 'M';
        lk_wr16(pl + 4, DL_VERSION); lk_wr16(pl + 6, l->flags);
        return lk_encode(out, DL_HELLO_ACK, 0, pl, 8);
    }
    uint32_t n = l->log_wr - l->log_rd;
    if (!n) return 0;
    if (n > DL_LOG_CHUNK) n = DL_LOG_CHUNK;
    /* 帧总长 (n + 10) 正好是 64 的倍数时少发 1 字节: USB 满包结尾没有零长包时, 主机的读请求可能一直等着,
       最后几行日志就卡住了 (Monita 在主机 -> 设备方向为同类问题补过 0x00, 这边没验证过, 直接避开) */
    if ((n + LK_HDR + LK_TRAIL) % 64u == 0) n--;
    for (uint32_t i = 0; i < n; i++) pl[i] = (uint8_t)l->log[(l->log_rd + i) & (DL_LOG_RING - 1u)];
    l->log_rd += n;
    return lk_encode(out, DL_LOG, 0, pl, n);
}

/* ---- 前面板 4 键 (monita/keys.h 的事件) -> Doom 键 ----
 *   1 查询 (PWRKEY) = 开火;  3 + = 前进;  4 − = 后退;
 *   2 ≡ = 菜单外开门、菜单里确认 (只发一个键: 标题画面上任何键都会打开菜单, 两个一起发就开菜单又选中 New Game);
 *         长按 = Esc 点一下。抬起时发按下时发的那个键 (按着的时候菜单开关了也不会漏抬)。
 * in_menu: Doom 的 menuactive。 */
static uint8_t dl_menu_key_down;
static inline void dl_panel(dgc_keyq_t *q, uint8_t ev, int in_menu){
    uint32_t k = KEY_EV_KEY(ev), t = KEY_EV_TYPE(ev);
    if (t == KEY_LONG){
        if (k == 2){ dgc_key_push(q, 27, 1, DGC_SRC_PANEL); dgc_key_push(q, 27, 0, DGC_SRC_PANEL); }
        return;
    }
    int down = t == KEY_DOWN;
    switch (k){
    case 1: dgc_key_push(q, 0xa3, down, DGC_SRC_PANEL); break;
    case 2:
        if (down) dl_menu_key_down = in_menu ? 13 : 0xa2;
        if (dl_menu_key_down) dgc_key_push(q, dl_menu_key_down, down, DGC_SRC_PANEL);
        if (!down) dl_menu_key_down = 0;
        break;
    case 3: dgc_key_push(q, 0xad, down, DGC_SRC_PANEL); break;
    case 4: dgc_key_push(q, 0xaf, down, DGC_SRC_PANEL); break;
    default: break;
    }
}

/* ---- zone 预算 ----
 * avail / maxblk: ql_dev_memory_size_query 的可用总量和最大连续块 (查询失败传 0)。
 * zone 要一整块; 分完之后堆里还要剩 DL_RESERVE: 约 140 KiB 是 Doom 自己后面的零碎 malloc (QEMU 实测),
 * 其余留给基带 / 内核其它任务 (Monita 日用固件三缓冲时只留 128 KiB 也一直稳, 这里多留很多)。 */
#define DL_RESERVE     (1280u << 10)
#define DL_MEM_BLOCK   1
#define DL_MEM_RESERVE 2
static inline int dl_zone_check(uint32_t avail, uint32_t maxblk, uint32_t zone){
    if (maxblk < zone + 4096u) return DL_MEM_BLOCK;
    if (avail < zone || avail - zone < DL_RESERVE) return DL_MEM_RESERVE;
    return 0;
}
/* 从 2560 KiB 起逐档往下试 (ARM 上 1200 KiB 已能跑完 E1 全部地图, 见 README "内存预算"); 返回字节数, 0 = 都不行 */
static inline uint32_t dl_zone_pick(uint32_t avail, uint32_t maxblk){
    static const uint32_t kib[] = { 2560u, 2048u, 1536u };
    for (unsigned i = 0; i < sizeof kib / sizeof kib[0]; i++)
        if (!dl_zone_check(avail, maxblk, kib[i] << 10)) return kib[i] << 10;
    return 0;
}

#endif
