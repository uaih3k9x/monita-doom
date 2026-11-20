/* port/r05/dg_efs.h 的主机单测: 假的 ql_f* (底下是主机 stdio, 每次最多读 chunk 字节、记 seek 次数) 上跑随机偏移读,
 * 和直接读文件比对。可选参数: 一个真文件 (例如 DOOM1.WAD); 不给就用生成的 300 KiB。
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../r05/dg_efs.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static FILE *g_open[8];
static int g_seeks, g_reads, g_chunk = 4096, g_fail_read_at = -1;
static char g_last_mode[8];
static const char *g_real;

static int f_open(const char *path, const char *mode){
    snprintf(g_last_mode, sizeof g_last_mode, "%s", mode);
    if (strcmp(path, "EFS:/monita/x.wad")) return -2;                 /* 设备: 打不开返回 <= 0 */
    for (int i = 1; i < 8; i++) if (!g_open[i]){ g_open[i] = fopen(g_real, "rb"); return g_open[i] ? i : -1; }
    return -3;
}
static int f_read(void *buf, uint32_t size, uint32_t count, int fd){
    if (size != 1) return -1;
    if (g_fail_read_at >= 0 && g_reads++ == g_fail_read_at) return -5;
    uint32_t n = count < (uint32_t)g_chunk ? count : (uint32_t)g_chunk;  /* 短读: 设备一次给多少不一定 */
    return (int)fread(buf, 1, n, g_open[fd]);
}
static int f_seek(int fd, int32_t off, int32_t whence){ g_seeks++; return fseek(g_open[fd], off, whence) ? -1 : 0; }
static int f_close(int fd){ fclose(g_open[fd]); g_open[fd] = NULL; return 0; }
static int f_size(int fd){ long p = ftell(g_open[fd]); fseek(g_open[fd], 0, SEEK_END); long s = ftell(g_open[fd]); fseek(g_open[fd], p, SEEK_SET); return (int)s; }

static const efs_ops_t OPS = { f_open, f_read, f_seek, f_close, f_size };

int main(int argc, char **argv){
    char tmp[] = "/tmp/claude_efs_XXXXXX";
    if (argc > 1) g_real = argv[1];
    else {
        int fd = mkstemp(tmp); FILE *f = fdopen(fd, "wb");
        srand(3); for (int i = 0; i < 300 * 1024; i++) fputc(rand() & 0xff, f);
        fclose(f); g_real = tmp;
    }
    FILE *ref = fopen(g_real, "rb");
    fseek(ref, 0, SEEK_END); long size = ftell(ref);
    uint8_t *all = malloc((size_t)size); fseek(ref, 0, SEEK_SET); fread(all, 1, (size_t)size, ref); fclose(ref);

    efs_init(&OPS);
    CHECK(efs_open("EFS:/monita/nope.wad") < 0, "打不开 -> <0");
    int h = efs_open("EFS:/monita/x.wad");
    CHECK(h >= 0, "打开 %d", h);
    CHECK(strcmp(g_last_mode, "rb") == 0, "只用 rb 打开 (%s)", g_last_mode);
    CHECK(efs_size(h) == size, "大小 %ld vs %ld", efs_size(h), size);

    /* 顺序读: 只在第一次 seek (或一次都不 seek) */
    uint8_t *buf = malloc(70000);
    g_seeks = 0;
    uint32_t off = 0;
    for (int i = 0; i < 20 && off < (uint32_t)size; i++){
        uint32_t n = 12345;
        int r = efs_read(h, off, buf, n);
        uint32_t want = off + n <= (uint32_t)size ? n : (uint32_t)size - off;
        CHECK(r == (int)want && memcmp(buf, all + off, want) == 0, "顺序读 @%u r=%d", off, r);
        off += want;
    }
    CHECK(g_seeks <= 1, "顺序读不重复 seek (%d 次)", g_seeks);

    /* 随机读 (短读拆成多次) */
    srand(5);
    g_chunk = 1000;
    for (int i = 0; i < 2000; i++){
        uint32_t o = (uint32_t)(rand() % size), n = (uint32_t)(rand() % 70000);
        int r = efs_read(h, o, buf, n);
        uint32_t want = o + n <= (uint32_t)size ? n : (uint32_t)size - o;
        if (r != (int)want || memcmp(buf, all + o, want)){ CHECK(0, "随机读 @%u n=%u r=%d", o, n, r); break; }
    }
    CHECK(efs_read(h, (uint32_t)size, buf, 10) == 0, "读到文件尾 = 0");

    /* 出错: 返回 <0, 下一次读重新 seek (位置不可信) 并且能读对 */
    g_reads = 0; g_fail_read_at = 0;
    CHECK(efs_read(h, 100, buf, 50) < 0, "读出错 -> <0");
    g_fail_read_at = -1; g_seeks = 0;
    CHECK(efs_read(h, 100, buf, 50) == 50 && memcmp(buf, all + 100, 50) == 0, "出错后同一位置重读能读对");
    CHECK(g_seeks == 1, "出错后重新 seek (%d)", g_seeks);

    /* 句柄用完 / 关掉再开 */
    int hs[EFS_MAX]; int k = 0;
    efs_close(h);
    for (k = 0; k < EFS_MAX; k++) hs[k] = efs_open("EFS:/monita/x.wad");
    CHECK(efs_open("EFS:/monita/x.wad") < 0, "超过 EFS_MAX 个 -> <0");
    for (k = 0; k < EFS_MAX; k++){ CHECK(hs[k] >= 0, "第 %d 个", k); efs_close(hs[k]); }
    CHECK(efs_read(hs[0], 0, buf, 1) < 0, "关掉的句柄读 -> <0");
    CHECK(efs_read(-1, 0, buf, 1) < 0 && efs_read(EFS_MAX, 0, buf, 1) < 0, "非法句柄 -> <0");

    free(buf); free(all);
    if (argc <= 1) remove(tmp);
    printf(fails ? "test_r05_efs: %d FAIL\n" : "test_r05_efs: OK\n", fails);
    return fails != 0;
}
