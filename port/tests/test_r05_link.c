/* port/r05/dg_link.h 的主机单测: 拆帧 -> 按键、主机在不在、日志环、前面板键位、zone 预算。
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "../r05/dg_link.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static dl_t L;
static dgc_keyq_t Q;

static uint32_t frame(uint8_t *out, uint8_t type, const uint8_t *pl, uint32_t n){ return lk_encode(out, type, 7, pl, n); }

/* 设备要发的全部帧拆出来: 返回帧数, types/payload 拼进 txt */
static int drain_tx(uint32_t now, uint8_t *types, int max, char *txt, size_t txtcap){
    static uint8_t buf[DL_TX_MAX], plb[DL_TX_MAX];
    lk_parser_t p; lk_init(&p, plb, sizeof plb);
    int k = 0; size_t tl = 0;
    if (txt) txt[0] = 0;
    for (;;){
        uint32_t n = dl_tx(&L, buf, sizeof buf, now);
        if (!n) break;
        CHECK(n <= DL_TX_MAX, "帧太长 %u", n);
        uint32_t off = 0;
        while (off < n){
            int ev; off += lk_feed(&p, buf + off, n - off, &ev);
            if (ev != LK_EV_FRAME) continue;
            if (k < max) types[k] = p.type;
            k++;
            if (p.type == DL_LOG && txt && tl + p.len < txtcap){ memcpy(txt + tl, p.buf, p.len); tl += p.len; txt[tl] = 0; }
        }
        if (k > 1000) break;
    }
    return k;
}

static void test_hello_key_ping(void){
    uint8_t b[64], t[8];
    dl_init(&L, 0); dgc_keyq_init(&Q);
    CHECK(!L.host_up, "开机时主机不在");
    CHECK(drain_tx(0, t, 8, NULL, 0) == 0, "主机没来不发任何东西");

    uint8_t kd[2] = { 0xad, 1 };
    uint32_t n = frame(b, DL_KEY, kd, 2);
    dl_rx(&L, b, n, 10, &Q);
    int down; uint8_t key;
    CHECK(dgc_key_pop(&Q, &down, &key) == 1 && key == 0xad && down == 1, "KEY 帧进队列 (没 HELLO 也收)");
    CHECK(L.host_up, "任何有效帧都算主机在");

    n = frame(b, DL_HELLO, NULL, 0);
    dl_rx(&L, b, n, 20, &Q);
    int k = drain_tx(20, t, 8, NULL, 0);
    CHECK(k >= 1 && t[0] == DL_HELLO_ACK, "HELLO -> HELLO_ACK 先发 (k=%d t0=%02x)", k, t[0]);

    /* 半截帧 + 剩下的分两次喂 */
    kd[0] = 0xa3; kd[1] = 1;
    n = frame(b, DL_KEY, kd, 2);
    dl_rx(&L, b, 3, 30, &Q); dl_rx(&L, b + 3, n - 3, 31, &Q);
    CHECK(dgc_key_pop(&Q, &down, &key) == 1 && key == 0xa3 && down == 1, "分段喂也能拆出来");

    /* 坏 CRC 丢掉 */
    n = frame(b, DL_KEY, kd, 2); b[n - 1] ^= 1;
    dl_rx(&L, b, n, 40, &Q);
    CHECK(dgc_key_pop(&Q, &down, &key) == 0, "CRC 错的帧不进队列");

    /* 键码 0 / 长度不对 丢掉 */
    uint8_t bad[3] = { 0, 1, 0 };
    n = frame(b, DL_KEY, bad, 2); dl_rx(&L, b, n, 41, &Q);
    n = frame(b, DL_KEY, bad, 3); dl_rx(&L, b, n, 42, &Q);
    CHECK(dgc_key_pop(&Q, &down, &key) == 0, "键码 0 / 长度不对 不进队列");

    /* 主机按着前进键走了: 超时后补发抬起, host_up 清掉 */
    dl_tick(&L, 41 + DL_HOST_TIMEOUT_MS - 1, &Q);
    CHECK(L.host_up, "没到超时还在");
    dl_tick(&L, 42 + DL_HOST_TIMEOUT_MS + 1, &Q);
    CHECK(!L.host_up, "超时 = 主机走了");
    int seen_up = 0;
    while (dgc_key_pop(&Q, &down, &key)) if (key == 0xa3 && !down) seen_up++;
    CHECK(seen_up == 1, "超时补发了按住的开火键的抬起 (%d)", seen_up);
    CHECK(drain_tx(5000, t, 8, NULL, 0) == 0, "主机走了不再发");

    /* PING 叫回来 */
    n = frame(b, DL_PING, NULL, 0); dl_rx(&L, b, n, 6000, &Q);
    CHECK(L.host_up, "PING 也算主机在");
}

static void test_log_ring(void){
    uint8_t b[64], t[64]; char txt[DL_LOG_RING * 2];
    dl_init(&L, 0); dgc_keyq_init(&Q);
    dl_log(&L, "hello ", 6);
    dl_log(&L, "doom\n", 5);
    CHECK(drain_tx(0, t, 64, txt, sizeof txt) == 0, "主机没来, 日志只进环");
    uint32_t n = frame(b, DL_HELLO, NULL, 0);
    dl_rx(&L, b, n, 1, &Q);
    int k = drain_tx(1, t, 64, txt, sizeof txt);
    CHECK(k >= 2 && strcmp(txt, "hello doom\n") == 0, "HELLO 后把攒的日志补发 (k=%d txt=%s)", k, txt);
    CHECK(drain_tx(2, t, 64, txt, sizeof txt) == 0, "发过的不重发");

    /* 环满: 丢最旧的, 留最新的, 记丢了多少 */
    dl_init(&L, 0);
    char big[DL_LOG_RING + 100];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 26);
    dl_log(&L, big, sizeof big);
    CHECK(L.log_dropped == 100, "丢了 %u", (unsigned)L.log_dropped);
    dl_rx(&L, b, n, 1, &Q);
    drain_tx(1, t, 64, txt, sizeof txt);
    CHECK(strlen(txt) == DL_LOG_RING && memcmp(txt, big + 100, DL_LOG_RING) == 0, "留下的是最新的 %zu 字节", strlen(txt));
}

static void test_panel(void){
    dgc_keyq_init(&Q);
    int down; uint8_t key;
    dl_panel(&Q, KEY_EV(3, KEY_DOWN));
    CHECK(dgc_key_pop(&Q, &down, &key) && key == 0xad && down, "+ 按下 = 前进");
    dl_panel(&Q, KEY_EV(3, KEY_UP));
    CHECK(dgc_key_pop(&Q, &down, &key) && key == 0xad && !down, "+ 抬起");
    dl_panel(&Q, KEY_EV(4, KEY_DOWN));
    CHECK(dgc_key_pop(&Q, &down, &key) && key == 0xaf && down, "- = 后退");
    dl_panel(&Q, KEY_EV(1, KEY_DOWN));
    CHECK(dgc_key_pop(&Q, &down, &key) && key == 0xa3 && down, "查询 = 开火");
    dl_panel(&Q, KEY_EV(2, KEY_DOWN));
    int got_use = 0, got_enter = 0;
    while (dgc_key_pop(&Q, &down, &key)){ if (key == 0xa2 && down) got_use++; if (key == 13 && down) got_enter++; }
    CHECK(got_use == 1 && got_enter == 1, "≡ = 开门 + 确认 (%d %d)", got_use, got_enter);
    dl_panel(&Q, KEY_EV(2, KEY_LONG));
    int esc_d = 0, esc_u = 0;
    while (dgc_key_pop(&Q, &down, &key)){ if (key == 27) { if (down) esc_d++; else esc_u++; } }
    CHECK(esc_d == 1 && esc_u == 1, "≡ 长按 = Esc 点一下 (%d %d)", esc_d, esc_u);
    /* 面板键不记进"主机按住的", 主机走了不会替它补抬起 */
    dl_init(&L, 0);
    dl_panel(&Q, KEY_EV(3, KEY_DOWN));
    while (dgc_key_pop(&Q, &down, &key)) {}
    CHECK(dgc_key_release_host(&Q) == 0, "面板键不算主机键");
}

static void test_zone_budget(void){
    /* 返回 0 = 可以; 否则是不够的原因 */
    CHECK(dl_zone_check(4360u << 10, 4350u << 10, 2560u << 10) == 0, "单缓冲时 (4.36 MB / 最大块 4.35 MB) 够");
    CHECK(dl_zone_check(4360u << 10, 2300u << 10, 2560u << 10) == DL_MEM_BLOCK, "最大块不够 zone");
    CHECK(dl_zone_check(3600u << 10, 3600u << 10, 2560u << 10) == DL_MEM_RESERVE, "分完剩下的不够留给基带");
    CHECK(dl_zone_check(0, 0, 2560u << 10) == DL_MEM_BLOCK, "查询失败按 0 算");
}

int main(void){
    test_hello_key_ping();
    test_log_ring();
    test_panel();
    test_zone_budget();
    printf(fails ? "test_r05_link: %d FAIL\n" : "test_r05_link: OK\n", fails);
    return fails != 0;
}
