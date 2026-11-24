/*
 * 外挂 flash (EFS:) 只读文件, 给 port/libc 的 dgl_fs_* 用。底下的 ql_f* 由调用方经 efs_ops_t 给
 * (设备: 内核导出; 主机单测: stdio 假实现, port/tests/test_r05_efs.c)。
 *   - 只用 "rb" 打开 (写类接口在导入清单里根本没有, tools/r05_validate.py 拦);
 *   - 记着每个句柄的当前位置, 顺序读不重复 seek; 出错后位置作废, 下次重新 seek;
 *   - ql_fread 一次给的可能比要的少 (没证实, 当它可能), 循环读满或读到 0。
 * 句柄约定照 extdump v2 (真机用过): ql_fopen 返回 > 0 有效。
 *   - ql_fseek 是 lseek 风格: 成功返回新偏移量 (>=0, SEEK_SET 到 off 即返回 off), 出错 <0。
 *     故这里按 "<0 才算失败" 判 (2026-11 真机实测: 返回新偏移导致 PNAMES not found, 已修)。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MONITA_DOOM_DG_EFS_H
#define MONITA_DOOM_DG_EFS_H
#include <stdint.h>

typedef struct {
    int (*open)(const char *path, const char *mode);
    int (*read)(void *buf, uint32_t size, uint32_t count, int fd);
    int (*seek)(int fd, int32_t off, int32_t whence);
    int (*close)(int fd);
    int (*size)(int fd);
} efs_ops_t;

#define EFS_MAX 4
static const efs_ops_t *g_efs;
static struct { int fd, pos; long size; } g_efs_f[EFS_MAX];     /* fd 0 = 空; pos -1 = 不可信 */

static inline void efs_init(const efs_ops_t *ops){
    g_efs = ops;
    for (int i = 0; i < EFS_MAX; i++) g_efs_f[i].fd = 0;
}

static inline int efs_open(const char *path){
    int i;
    for (i = 0; i < EFS_MAX && g_efs_f[i].fd; i++) {}
    if (i == EFS_MAX) return -1;
    int fd = g_efs->open(path, "rb");
    if (fd <= 0) return -1;
    int sz = g_efs->size(fd);
    if (sz < 0){ g_efs->close(fd); return -1; }
    g_efs_f[i].fd = fd; g_efs_f[i].pos = -1; g_efs_f[i].size = sz;
    return i;
}

static inline int efs_ok(int h){ return h >= 0 && h < EFS_MAX && g_efs_f[h].fd; }
static inline long efs_size(int h){ return efs_ok(h) ? g_efs_f[h].size : -1; }
static inline void efs_close(int h){ if (efs_ok(h)){ g_efs->close(g_efs_f[h].fd); g_efs_f[h].fd = 0; } }

/* 从 off 读 n 字节; 返回读到的字节数 (文件尾之后 = 0), <0 出错 */
static inline int efs_read(int h, uint32_t off, void *buf, uint32_t n){
    if (!efs_ok(h)) return -1;
    if (off >= (uint32_t)g_efs_f[h].size) return 0;
    if (n > (uint32_t)g_efs_f[h].size - off) n = (uint32_t)g_efs_f[h].size - off;
    if (g_efs_f[h].pos != (int)off){
        if (g_efs->seek(g_efs_f[h].fd, (int32_t)off, 0) < 0){ g_efs_f[h].pos = -1; return -1; }
        g_efs_f[h].pos = (int)off;
    }
    uint32_t got = 0;
    while (got < n){
        int r = g_efs->read((uint8_t *)buf + got, 1, n - got, g_efs_f[h].fd);
        if (r < 0){ g_efs_f[h].pos = -1; return -1; }
        if (r == 0) break;
        got += (uint32_t)r;
    }
    g_efs_f[h].pos += (int)got;
    return (int)got;
}

#endif
