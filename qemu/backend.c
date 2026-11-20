/*
 * QEMU 测速版的 libc 后端: 半主机 (文件只读、控制台、命令行、退出) + 简单的堆 (只增长)。
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dgl_backend.h"

extern int sh_call(int op, void *arg);
extern char __heap_start[], __heap_end[], __stack_bottom[], __stack_top[];
enum { SYS_OPEN = 1, SYS_CLOSE = 2, SYS_WRITE = 5, SYS_READ = 6, SYS_SEEK = 10, SYS_FLEN = 12, SYS_GET_CMDLINE = 0x15, SYS_EXIT = 0x18 };

static int g_tt = -1;
void dgl_log_write(const char *s, size_t n){
    if (g_tt < 0){ uint32_t a[3] = { (uint32_t)":tt", 4, 3 }; g_tt = sh_call(SYS_OPEN, a); }
    uint32_t a[3] = { (uint32_t)g_tt, (uint32_t)s, (uint32_t)n };
    sh_call(SYS_WRITE, a);
}
int dgl_fs_open(const char *path){
    uint32_t a[3] = { (uint32_t)path, 1 /* "rb" */, strlen(path) };
    return sh_call(SYS_OPEN, a);
}
long dgl_fs_size(int h){ uint32_t a[1] = { (uint32_t)h }; return sh_call(SYS_FLEN, a); }
void dgl_fs_close(int h){ uint32_t a[1] = { (uint32_t)h }; sh_call(SYS_CLOSE, a); }
int dgl_fs_read(int h, uint32_t off, void *buf, uint32_t n){
    uint32_t s[2] = { (uint32_t)h, off };
    if (sh_call(SYS_SEEK, s) != 0) return -1;
    uint32_t r[3] = { (uint32_t)h, (uint32_t)buf, n };
    int left = sh_call(SYS_READ, r);                   /* 返回没读到的字节数 */
    return left < 0 ? -1 : (int)(n - (uint32_t)left);
}

static uint32_t stack_used(void){
    uintptr_t p = (uintptr_t)__stack_bottom, top = (uintptr_t)__stack_top;
    while (p < top - 256u && *(const volatile uint32_t *)p == 0xDEADBEEFu) p += 4;
    return (uint32_t)(top - p);
}
static uint32_t g_heap_peak;
void dgl_exit(int code){
    printf("QEMU: exit %d, 栈最深 %u B, 堆峰值 %u B\n", code, (unsigned)stack_used(), (unsigned)g_heap_peak);
    uint32_t a[2] = { 0x20026, (uint32_t)code };      /* ADP_Stopped_ApplicationExit */
    sh_call(SYS_EXIT, a);
    for (;;) {}
}

/* 堆: 每块前 8 字节记大小; free 只回收最后一块 (Doom 的 malloc 很少, 大头是一次性的 zone) */
static char *g_brk = __heap_start, *g_last;
void *malloc(size_t n){
    n = (n + 7u) & ~7u;
    if ((size_t)(__heap_end - g_brk) < n + 8u) return NULL;
    char *p = g_brk;
    *(uint32_t *)p = (uint32_t)n;
    g_brk += n + 8u; g_last = p + 8;
    if ((uint32_t)(g_brk - __heap_start) > g_heap_peak) g_heap_peak = (uint32_t)(g_brk - __heap_start);
    return p + 8;
}
void free(void *p){ if (p && p == g_last){ g_brk = (char *)p - 8; g_last = NULL; } }
void *calloc(size_t n, size_t m){ void *p = malloc(n * m); if (p) memset(p, 0, n * m); return p; }
void *realloc(void *p, size_t n){
    if (!p) return malloc(n);
    uint32_t old = *(uint32_t *)((char *)p - 8);
    if (n <= old) return p;
    if (p == g_last && (size_t)(__heap_end - (char *)p) >= ((n + 7u) & ~7u)){
        n = (n + 7u) & ~7u; *(uint32_t *)((char *)p - 8) = (uint32_t)n; g_brk = (char *)p + n;
        if ((uint32_t)(g_brk - __heap_start) > g_heap_peak) g_heap_peak = (uint32_t)(g_brk - __heap_start);
        return p;
    }
    void *q = malloc(n);
    if (q) memcpy(q, p, old);
    return q;
}

/* 半主机命令行 -> argc / argv */
int main(int argc, char **argv);
static char g_cmd[1024];
static char *g_argv[32];
void qemu_main(void){
    uint32_t a[2] = { (uint32_t)g_cmd, sizeof g_cmd - 1 };
    int argc = 0;
    if (sh_call(SYS_GET_CMDLINE, a) == 0){
        char *s = g_cmd;
        while (*s && argc < 31){
            while (*s == ' ') *s++ = 0;
            if (!*s) break;
            g_argv[argc++] = s;
            while (*s && *s != ' ') s++;
        }
    }
    if (!argc) g_argv[argc++] = "doom";
    g_argv[argc] = NULL;
    exit(main(argc, g_argv));
}
