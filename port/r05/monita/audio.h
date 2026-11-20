/*
 * 喇叭 (fw v13, caps bit10): 主机透传 PCM -> 环形缓冲 -> 内核 PCM 播放器。协议见 docs/protocol.md "喇叭", 设计见 docs/audio.md。
 * 纯逻辑, 不碰 SDK: 硬件走 au_io_t (实机 fw/app.c: ql_pcm_* + 功放 GPIO; 模拟器 tools/fwsim/simdev.h; 单测 test_audio.c)。
 *
 * 线程: 全在链路主循环里 (同一个任务), 不开音频任务、不加锁。
 *   on_frame 收到 AUDIO_DATA 只往环形缓冲里拷 (放不下整条丢), 从不等;
 *   主循环每轮 au_poll: 按"估计的内核管道水位"往内核写 (非阻塞, 写不进就下一轮), 每次最多 AU_CHUNK_MAX。
 * 内核管道水位是估的: 交给内核的字节 - 开播以来按 16 kHz 单声道 (32 B/ms) 应已放掉的; 内核实际慢一点也没事,
 *   非阻塞写在它满了时只收一部分 (实机管道 16 KiB)。估计值 < AU_PIPE_LOW 而环形缓冲空了: 补 20 ms 静音 (欠载计数),
 *   免得内核播放器自己欠载 (欠载时它怎么表现没在真机上看过)。
 * 状态: IDLE -START-> PREBUF -(攒够 AU_PREBUF_B)-> PLAY (开 PCM、拉功放) -STOP drain / 主机 AU_IDLE_US 没发数据-> DRAIN
 *   (放完环形缓冲, 不补静音; 估计管道空了再等 AU_TAIL_US) -> 关 PCM、放功放 -> IDLE。
 *   STOP 不带 drain / HELLO / 主机走了 / SLEEP: au_halt 立刻关。打不开 / 写出错: ERR (功放放低), 下一条 START 重来。
 * 只收 16000 Hz / 单声道 / s16le (原厂 app 就用这一组; 别的采样率内核可能也行, 没证实, 主机重采样)。
 * 音量 (0..100) 在拷进环形缓冲时软件乘, 不调内核的音量接口 (它每调一次写一次内部 flash, 见 docs/audio.md)。
 */
#ifndef MONITA_LINK_AUDIO_H
#define MONITA_LINK_AUDIO_H
#include <stdint.h>
#include "proto.h"

#define AU_RATE        16000u
#define AU_BPMS        32u                     /* 16 kHz × 1 ch × 2 B = 32 B/ms */
#define AU_FMT_S16LE   1u
#ifndef AU_RING_BYTES
#define AU_RING_BYTES  16384u                  /* 512 ms; 2 的幂 */
#endif
#define AU_PREBUF_B    (100u * AU_BPMS)        /* 开播前攒 100 ms */
#define AU_PIPE_TARGET (120u * AU_BPMS)        /* 内核管道估计水位到这里就不写了 (≈ 延迟) */
#define AU_PIPE_LOW    (40u * AU_BPMS)         /* 低于这个且没数据: 补静音 */
#define AU_SIL         (20u * AU_BPMS)
#define AU_CHUNK_MAX   (40u * AU_BPMS)         /* 一次 write 最多 1280 B */
#define AU_IDLE_US     2000000u                /* 主机这么久没发 AUDIO_DATA: 当成 STOP drain */
#define AU_TAIL_US     150000u                 /* 排空后再等这么久才关 (估计值偏小时别切掉尾巴) */
#define AU_STAT_LEN    40u
#define AU_STOP_DRAIN  1u

enum { AU_IDLE = 0, AU_PREBUF = 1, AU_PLAY = 2, AU_DRAIN = 3, AU_ERR = 4 };
enum { AU_ERR_NONE = 0, AU_ERR_OPEN = 1, AU_ERR_WRITE = 2 };

typedef struct {
    int     (*open)(void *ctx, uint32_t rate, uint32_t channels);   /* 0 = 成功 */
    int32_t (*write)(void *ctx, const uint8_t *p, uint32_t n);      /* 非阻塞: 收下的字节 (0..n), <0 出错 */
    void    (*close)(void *ctx);
    void    (*pa)(void *ctx, uint32_t on);                          /* 功放 */
    void    *ctx;
} au_io_t;

typedef struct {
    uint8_t *buf; uint32_t cap, wr, rd;        /* wr / rd 一直往上数, 下标 & (cap - 1) */
    uint8_t  state, err, opened;
    uint32_t vol_q15;                          /* 32768 = 原样 */
    uint32_t est, t_us, acc_us, tail_at, last_data_us;
    uint32_t rx, dropped, overruns, underruns, silence, pcm_bytes;
} au_t;

static const uint8_t au_zero[AU_SIL];

static void au_init(au_t *a, uint8_t *buf, uint32_t cap){
    uint8_t *p = (uint8_t *)a;
    for (uint32_t i = 0; i < sizeof *a; i++) p[i] = 0;
    a->buf = buf; a->cap = cap;                /* cap 必须是 2 的幂 (调用方保证, 单测查) */
    a->vol_q15 = 32768u;
}
static uint32_t au_fill(const au_t *a){ return a->wr - a->rd; }

static void au_halt(au_t *a, const au_io_t *io){
    if (a->opened){ io->close(io->ctx); io->pa(io->ctx, 0); a->opened = 0; }
    a->rd = a->wr; a->est = 0;
    a->state = AU_IDLE;
}

/* AUDIO_START: u32 rate, u8 channels, u8 format, u8 volume (0..100), u8 0 */
static uint8_t au_start(au_t *a, const uint8_t *pl, uint32_t n, uint32_t now){
    if (n != 8 || lk_rd32(pl) != AU_RATE || pl[4] != 1 || pl[5] != AU_FMT_S16LE || pl[6] > 100) return LK_EPARAM;
    a->vol_q15 = pl[6] >= 100 ? 32768u : (uint32_t)pl[6] * 328u;      /* ≈ vol / 100 (Q15, 不用除法) */
    a->rd = a->wr;                             /* 播放中重来: 丢掉旧的, 管道不关 */
    a->rx = a->dropped = a->overruns = a->underruns = a->silence = a->pcm_bytes = 0;
    a->err = AU_ERR_NONE;
    a->state = AU_PREBUF;
    a->last_data_us = now;
    return LK_OK;
}


/* AUDIO_STOP: [u8 flags]; bit0 = 放完已收的再停 */
static uint8_t au_stop(au_t *a, const au_io_t *io, const uint8_t *pl, uint32_t n, uint32_t now){
    (void)now;
    if (n > 1 || (n == 1 && pl[0] > AU_STOP_DRAIN)) return LK_EPARAM;
    int drain = n == 1 && (pl[0] & AU_STOP_DRAIN);
    if (drain && (a->state == AU_PREBUF || a->state == AU_PLAY)){ a->state = AU_DRAIN; a->tail_at = 0; }
    else if (!(drain && a->state == AU_DRAIN)) au_halt(a, io);
    return LK_OK;
}

/* AUDIO_DATA: 整条放进环形缓冲 (乘音量), 放不下 / 奇数长度 / 不在流里 = 整条丢 */
static void au_data(au_t *a, const uint8_t *pl, uint32_t n, uint32_t now){
    if (a->state != AU_PREBUF && a->state != AU_PLAY){ a->dropped += n; return; }
    a->last_data_us = now;
    if (n & 1u){ a->dropped += n; return; }
    if (n > a->cap - au_fill(a)){ a->dropped += n; a->overruns++; return; }
    uint32_t m = a->cap - 1u, w = a->wr;
    if (a->vol_q15 == 32768u){
        for (uint32_t i = 0; i < n; i++) a->buf[(w + i) & m] = pl[i];
    } else {
        for (uint32_t i = 0; i < n; i += 2){
            int32_t s = (int16_t)(uint16_t)(pl[i] | (uint32_t)pl[i + 1] << 8);
            s = (s * (int32_t)a->vol_q15) >> 15;             /* |s * q| < 2^31; 算术右移 (向负无穷取整) */
            a->buf[(w + i) & m] = (uint8_t)s;
            a->buf[(w + i + 1u) & m] = (uint8_t)((uint32_t)s >> 8);
        }
    }
    a->wr = w + n;
    a->rx += n;
}

/* 往内核写一段; 返回收下的字节, <0 = 出错 (已经停了) */
static int32_t au_put(au_t *a, const au_io_t *io, const uint8_t *p, uint32_t n){
    int32_t k = io->write(io->ctx, p, n);
    if (k < 0){ au_halt(a, io); a->state = AU_ERR; a->err = AU_ERR_WRITE; return -1; }
    a->est += (uint32_t)k; a->pcm_bytes += (uint32_t)k;
    return k;
}

static void au_pump(au_t *a, const au_io_t *io){
    while (a->est < AU_PIPE_TARGET){
        uint32_t want = AU_PIPE_TARGET - a->est, fill = au_fill(a);
        if (want > AU_CHUNK_MAX) want = AU_CHUNK_MAX;
        if (!fill){
            if (a->state != AU_PLAY || a->est >= AU_PIPE_LOW) return;
            uint32_t s = want < AU_SIL ? want : AU_SIL;
            int32_t k = au_put(a, io, au_zero, s);
            if (k <= 0) return;
            a->silence += (uint32_t)k; a->underruns++;
            continue;
        }
        if (a->silence & 1u){                              /* 补过奇数字节静音: 先补齐一个字节, 保持采样对齐 */
            int32_t k = au_put(a, io, au_zero, 1);
            if (k <= 0) return;
            a->silence++;
            continue;
        }
        uint32_t off = a->rd & (a->cap - 1u), len = a->cap - off;
        if (len > fill) len = fill;
        if (len > want) len = want;
        int32_t k = au_put(a, io, a->buf + off, len);
        if (k <= 0) return;
        a->rd += (uint32_t)k;
    }
}

/* 主循环每轮 */
static void au_poll(au_t *a, const au_io_t *io, uint32_t now){
    if (a->state == AU_IDLE || a->state == AU_ERR) return;
    if (a->opened){                                        /* 估计管道里放掉了多少 */
        uint32_t dt = now - a->t_us;
        a->t_us = now;
        if (dt > 1000000u) dt = 1000000u;
        a->acc_us += dt;
        uint32_t ms = (uint32_t)(((uint64_t)a->acc_us * 4294968u) >> 32);   /* = acc_us / 1000 (acc_us ≤ 1001000, 已穷举验证; 固件没有 libgcc 除法) */
        a->acc_us -= ms * 1000u;
        uint32_t c = ms * AU_BPMS;
        a->est = a->est > c ? a->est - c : 0;
    }
    if ((a->state == AU_PREBUF || a->state == AU_PLAY) && now - a->last_data_us > AU_IDLE_US){ a->state = AU_DRAIN; a->tail_at = 0; }
    if (a->state == AU_PREBUF && au_fill(a) < AU_PREBUF_B) return;
    if (!a->opened){
        if (a->state == AU_DRAIN && !au_fill(a)){ a->state = AU_IDLE; return; }
        io->pa(io->ctx, 1);                                /* 同原厂: 先开功放再开 PCM */
        if (io->open(io->ctx, AU_RATE, 1)){
            io->pa(io->ctx, 0);
            a->rd = a->wr; a->state = AU_ERR; a->err = AU_ERR_OPEN;
            return;
        }
        a->opened = 1; a->est = 0; a->t_us = now; a->acc_us = 0;
    }
    if (a->state == AU_PREBUF) a->state = AU_PLAY;
    au_pump(a, io);
    if (a->state == AU_DRAIN && !au_fill(a) && a->est == 0){
        if (!a->tail_at) a->tail_at = now | 1u;
        else if (now - a->tail_at >= AU_TAIL_US) au_halt(a, io);
    }
}

/* AUDIO_STAT_R (40 B): u8 state, u8 err, u16 0, u32 fill, cap, pipe_est, rx, dropped, underruns, pcm_bytes, overruns, silence */
static uint32_t au_stat(const au_t *a, uint8_t *b){
    b[0] = a->state; b[1] = a->err; b[2] = 0; b[3] = 0;
    uint32_t v[9] = { au_fill(a), a->cap, a->est, a->rx, a->dropped, a->underruns, a->pcm_bytes, a->overruns, a->silence };
    for (int i = 0; i < 9; i++) lk_wr32(b + 4 + 4 * i, v[i]);
    return AU_STAT_LEN;
}

#endif
