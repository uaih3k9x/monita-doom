/*
 * port/libc 的后端: 设备 (port/r05/dg_r05.c, EFS + USB 日志) 和 QEMU (qemu/backend.c, 半主机) 各实现一份。
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DGL_BACKEND_H
#define DGL_BACKEND_H
#include <stddef.h>
#include <stdint.h>
int  dgl_fs_open(const char *path);                              /* 只读打开; >= 0 句柄, < 0 失败 */
int  dgl_fs_read(int h, uint32_t off, void *buf, uint32_t n);    /* 从 off 读 n 字节; 返回读到的字节数, < 0 出错 */
long dgl_fs_size(int h);
void dgl_fs_close(int h);
void dgl_log_write(const char *s, size_t n);                     /* printf / stdout / stderr */
void dgl_exit(int code) __attribute__((noreturn));               /* exit() 跑完 atexit 之后 */
#endif
