/*
 * QEMU TCG 插件: 数指令, 按"标记函数"切帧 (qemu/run.sh 用)。
 *   -plugin libinsncount.so,frame=0xADDR,pause=0xADDR,resume=0xADDR,out=FILE
 *   每个翻译块进入时内联地把块里的指令数加进计数 (快); 只在三个标记地址上挂回调:
 *     frame  = 一帧结束: 记下这一帧的指令数 (上一个 frame 以来, 扣掉 pause..resume 之间的)
 *     pause / resume = 中间这段 (CRC、打印) 不算
 *   退出时把每帧的指令数逐行写进 out。
 * 只数指令 (TCG 不模拟时序), 换算成时间见 qemu/report.py。单 vCPU。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static struct qemu_plugin_scoreboard *sb;
static qemu_plugin_u64 cnt;
static uint64_t a_frame, a_pause, a_resume;
static uint64_t last, paused_at, excluded;
static uint64_t *frames; static size_t nframes, cap;
static char out_path[512] = "frames.txt";

static uint64_t now(void){ return qemu_plugin_u64_get(cnt, 0); }

static void on_mark(unsigned int vcpu, void *ud){
    (void)vcpu;
    uintptr_t k = (uintptr_t)ud;
    uint64_t t = now();
    if (k == 1){
        if (nframes == cap){ cap = cap ? cap * 2 : 4096; frames = realloc(frames, cap * sizeof *frames); }
        frames[nframes++] = t - last - excluded;
        last = t; excluded = 0;
    } else if (k == 2) paused_at = t;
    else if (k == 3) excluded += t - paused_at;
}

static void on_tb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb){
    (void)id;
    size_t n = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, cnt, n);
    for (size_t i = 0; i < n; i++){
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        uint64_t a = qemu_plugin_insn_vaddr(in);
        uintptr_t k = a == a_frame ? 1 : a == a_pause ? 2 : a == a_resume ? 3 : 0;
        if (k) qemu_plugin_register_vcpu_insn_exec_cb(in, on_mark, QEMU_PLUGIN_CB_NO_REGS, (void *)k);
    }
}

static void on_exit_cb(qemu_plugin_id_t id, void *ud){
    (void)id; (void)ud;
    FILE *f = fopen(out_path, "w");
    if (!f) return;
    for (size_t i = 0; i < nframes; i++) fprintf(f, "%" PRIu64 "\n", frames[i]);
    fprintf(f, "# total %" PRIu64 "\n", now());
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv){
    (void)info;
    for (int i = 0; i < argc; i++){
        if (!strncmp(argv[i], "frame=", 6)) a_frame = strtoull(argv[i] + 6, 0, 0) & ~1ull;
        else if (!strncmp(argv[i], "pause=", 6)) a_pause = strtoull(argv[i] + 6, 0, 0) & ~1ull;
        else if (!strncmp(argv[i], "resume=", 7)) a_resume = strtoull(argv[i] + 7, 0, 0) & ~1ull;
        else if (!strncmp(argv[i], "out=", 4)) snprintf(out_path, sizeof out_path, "%s", argv[i] + 4);
        else return -1;
    }
    sb = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    cnt = qemu_plugin_scoreboard_u64(sb);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb);
    qemu_plugin_register_atexit_cb(id, on_exit_cb, 0);
    return 0;
}
