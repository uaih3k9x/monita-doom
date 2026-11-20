/*
 * monita-doom 精简 libc: 设备 (R05 app) 和 QEMU 测速版用, 只实现 Doom 引擎用到的那些。
 *   - 文件只读 (写 / 改名 / 删除一律失败): 设备上写外挂 flash 的接口在构建时就被禁止 (fw/sdk/validate.py);
 *   - printf / fprintf(stdout|stderr) 进日志 (dgl_log_write, 设备上经 USB 发给主机);
 *   - 浮点格式 (%f %g %e) 不实现, 打印 "?" (只有 timedemo 的结束语和配置文件写回用到, 都不影响游戏);
 *   - malloc / free / realloc / calloc 由后端提供 (设备上直接是内核导出的同名函数)。
 * 后端接口见 dgl_backend.h。主机单测 port/tests/test_libc.c (DGL_TEST 时所有名字加 dgl_ 前缀, 不和系统 libc 冲突)。
 *
 * Copyright (C) 2026 Monita-CS80 维护者
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef DGL_TEST
#include "dgl_test_names.h"
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "dgl_backend.h"

int errno;

/* ---- 内存 / 字符串 ---- */
void *memcpy(void *d, const void *s, size_t n){
    uint8_t *dp = d; const uint8_t *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3u) == 0){
        while (n >= 16){
            ((uint32_t *)dp)[0] = ((const uint32_t *)sp)[0]; ((uint32_t *)dp)[1] = ((const uint32_t *)sp)[1];
            ((uint32_t *)dp)[2] = ((const uint32_t *)sp)[2]; ((uint32_t *)dp)[3] = ((const uint32_t *)sp)[3];
            dp += 16; sp += 16; n -= 16;
        }
        while (n >= 4){ *(uint32_t *)dp = *(const uint32_t *)sp; dp += 4; sp += 4; n -= 4; }
    }
    while (n--) *dp++ = *sp++;
    return d;
}
void *memmove(void *d, const void *s, size_t n){
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}
void *memset(void *d, int c, size_t n){
    uint8_t *dp = d; uint8_t v = (uint8_t)c;
    while (n && ((uintptr_t)dp & 3u)){ *dp++ = v; n--; }
    uint32_t w = v * 0x01010101u;
    while (n >= 4){ *(uint32_t *)dp = w; dp += 4; n -= 4; }
    while (n--) *dp++ = v;
    return d;
}
int memcmp(const void *a, const void *b, size_t n){
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
void *memchr(const void *s, int c, size_t n){
    const uint8_t *p = s;
    for (; n; n--, p++) if (*p == (uint8_t)c) return (void *)p;
    return NULL;
}
size_t strlen(const char *s){ const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t n){ size_t i = 0; while (i < n && s[i]) i++; return i; }
char *strcpy(char *d, const char *s){ char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n){
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s){ strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n){
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
int strcmp(const char *a, const char *b){
    while (*a && *a == *b){ a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n){
    for (; n; n--, a++, b++){
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b){
    for (;; a++, b++){
        int x = tolower((uint8_t)*a), y = tolower((uint8_t)*b);
        if (x != y || !x) return x - y;
    }
}
int strncasecmp(const char *a, const char *b, size_t n){
    for (; n; n--, a++, b++){
        int x = tolower((uint8_t)*a), y = tolower((uint8_t)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}
char *strchr(const char *s, int c){
    for (;; s++){
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}
char *strrchr(const char *s, int c){
    const char *r = NULL;
    for (;; s++){
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}
char *strstr(const char *h, const char *n){
    size_t k = strlen(n);
    if (!k) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, k)) return (char *)h;
    return NULL;
}
char *strdup(const char *s){
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
char *strerror(int e){ (void)e; return "error"; }

/* ---- 数字 ---- */
static int digit_val(int c){
    if (isdigit(c)) return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
unsigned long strtoul(const char *s, char **end, int base){
    const char *p = s;
    while (isspace((uint8_t)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-'){ neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_val((uint8_t)p[2]) < 16){ p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long v = 0; const char *start = p;
    while (digit_val((uint8_t)*p) < base){ v = v * (unsigned long)base + (unsigned long)digit_val((uint8_t)*p); p++; }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? (unsigned long)(-(long)v) : v;
}
long strtol(const char *s, char **end, int base){ return (long)strtoul(s, end, base); }
int atoi(const char *s){ return (int)strtol(s, NULL, 10); }
long atol(const char *s){ return strtol(s, NULL, 10); }
int abs(int v){ return v < 0 ? -v : v; }
long labs(long v){ return v < 0 ? -v : v; }

/* 只有配置文件里的浮点项 (mouse_acceleration 一类) 用到; 软浮点, 冷路径 */
double atof(const char *s){
    while (isspace((uint8_t)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-'){ neg = *s == '-'; s++; }
    double v = 0, scale = 1;
    while (isdigit((uint8_t)*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.'){ s++; while (isdigit((uint8_t)*s)){ v = v * 10 + (*s++ - '0'); scale *= 10; } }
    v /= scale;
    return neg ? -v : v;
}

static unsigned long g_rand = 1;
int rand(void){ g_rand = g_rand * 1103515245u + 12345u; return (int)((g_rand >> 16) & RAND_MAX); }
void srand(unsigned s){ g_rand = s; }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)){
    uint8_t *a = base;
    for (size_t i = 1; i < n; i++)                     /* 插入排序: Doom 里只有很小的数组 */
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--)
            for (size_t k = 0; k < size; k++){
                uint8_t t = a[(j - 1) * size + k]; a[(j - 1) * size + k] = a[j * size + k]; a[j * size + k] = t;
            }
}

char *getenv(const char *name){ (void)name; return NULL; }
int system(const char *cmd){ (void)cmd; return -1; }

/* ---- 退出 ---- */
static void (*g_atexit[8])(void);
static int g_natexit;
int atexit(void (*fn)(void)){
    if (g_natexit >= 8) return -1;
    g_atexit[g_natexit++] = fn;
    return 0;
}
void exit(int code){
    while (g_natexit > 0) g_atexit[--g_natexit]();
    dgl_exit(code);
}
void abort(void){ exit(134); }
void dgl_assert_fail(const char *expr, const char *file, int line){
    printf("assert failed: %s (%s:%d)\n", expr, file, line);
    exit(134);
}

/* ---- 格式化输出 ---- */
typedef struct { char *p; size_t n, len; } outbuf_t;
static void ob_put(outbuf_t *o, char c){ if (o->len + 1 < o->n) o->p[o->len] = c; o->len++; }

static void ob_pad(outbuf_t *o, char c, int k){ while (k-- > 0) ob_put(o, c); }

static void fmt_uint(outbuf_t *o, unsigned long long v, unsigned base, int upper, int neg, int plus, int space,
                     int width, int prec, int left, int zero, int alt){
    char tmp[24]; int k = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0) tmp[k++] = '0';
    while (v){ tmp[k++] = dg[v % base]; v /= base; }
    int nd = k < prec ? prec : k;
    char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    const char *pre = alt && base == 16 ? (upper ? "0X" : "0x") : alt && base == 8 && nd <= k ? "0" : "";
    int total = nd + (sign ? 1 : 0) + (int)strlen(pre);
    if (prec >= 0) zero = 0;
    if (!left && !zero) ob_pad(o, ' ', width - total);
    if (sign) ob_put(o, sign);
    for (const char *q = pre; *q; q++) ob_put(o, *q);
    if (!left && zero) ob_pad(o, '0', width - total);
    ob_pad(o, '0', nd - k);
    while (k) ob_put(o, tmp[--k]);
    if (left) ob_pad(o, ' ', width - total);
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap){
    outbuf_t o = { s, n, 0 };
    for (; *fmt; fmt++){
        if (*fmt != '%'){ ob_put(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++){
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1; else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*'){ width = va_arg(ap, int); if (width < 0){ left = 1; width = -width; } fmt++; }
        else while (isdigit((uint8_t)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.'){
            fmt++; prec = 0;
            if (*fmt == '*'){ prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else while (isdigit((uint8_t)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;                                    /* 0 int, 1 long, 2 long long, 3 size_t */
        for (;; fmt++){
            if (*fmt == 'l') lng++; else if (*fmt == 'h') {} else if (*fmt == 'z' || *fmt == 't') lng = 3; else break;
        }
        char c = *fmt;
        if (!c) break;
        switch (c){
        case 'd': case 'i': {
            long long v = lng >= 2 && lng != 3 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1u : (unsigned long long)v;
            fmt_uint(&o, u, 10, 0, v < 0, plus, space, width, prec, left, zero, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 && lng != 3 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            fmt_uint(&o, v, c == 'u' ? 10u : c == 'o' ? 8u : 16u, c == 'X', 0, 0, 0, width, prec, left, zero, alt);
            break;
        }
        case 'p':
            ob_put(&o, '0'); ob_put(&o, 'x');
            fmt_uint(&o, (uintptr_t)va_arg(ap, void *), 16, 0, 0, 0, 0, width > 2 ? width - 2 : 0, -1, left, zero, 0);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!left) ob_pad(&o, ' ', width - 1);
            ob_put(&o, ch);
            if (left) ob_pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int k = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
            if (!left) ob_pad(&o, ' ', width - k);
            for (int i = 0; i < k; i++) ob_put(&o, str[i]);
            if (left) ob_pad(&o, ' ', width - k);
            break;
        }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
            (void)va_arg(ap, double);                   /* 不做浮点格式 (见文件头) */
            ob_put(&o, '?');
            break;
        case '%': ob_put(&o, '%'); break;
        default: ob_put(&o, '%'); ob_put(&o, c); break;
        }
    }
    if (n) o.p[o.len < n ? o.len : n - 1] = 0;
    return (int)o.len;
}
int vsprintf(char *s, const char *fmt, va_list ap){ return vsnprintf(s, (size_t)-1 >> 1, fmt, ap); }
int snprintf(char *s, size_t n, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vsnprintf(s, n, fmt, ap); va_end(ap); return r;
}
int sprintf(char *s, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vsprintf(s, fmt, ap); va_end(ap); return r;
}

/* ---- sscanf: 只有 %d %i %u %x %o (int / unsigned 存储), 空白, 字面字符 ---- */
int sscanf(const char *s, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    int n = 0;
    for (; *fmt; fmt++){
        if (isspace((uint8_t)*fmt)){ while (isspace((uint8_t)*s)) s++; continue; }
        if (*fmt != '%'){
            if (*s != *fmt) break;
            s++; continue;
        }
        fmt++;
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        int base = *fmt == 'd' || *fmt == 'u' ? 10 : *fmt == 'x' || *fmt == 'X' ? 16 : *fmt == 'o' ? 8 : *fmt == 'i' ? 0 : -1;
        if (base < 0) break;
        while (isspace((uint8_t)*s)) s++;
        char *e;
        unsigned long v = strtoul(s, &e, base);
        if (e == s) break;
        *va_arg(ap, int *) = (int)v;
        n++;
        s = e;
    }
    va_end(ap);
    return n;
}

/* ---- 日志输出 ---- */
static int log_vprintf(const char *fmt, va_list ap){
    char buf[512];
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    dgl_log_write(buf, strlen(buf));
    return r;
}
int vprintf(const char *fmt, va_list ap){ return log_vprintf(fmt, ap); }
int printf(const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = log_vprintf(fmt, ap); va_end(ap); return r;
}
int puts(const char *s){ dgl_log_write(s, strlen(s)); dgl_log_write("\n", 1); return 0; }
int putchar(int c){ char ch = (char)c; dgl_log_write(&ch, 1); return c; }

/* ---- 文件: 只读 ---- */
struct dgl_file { int kind; int h; uint32_t pos, size; int eof, err; };
enum { F_FREE = 0, F_LOG = 1, F_FILE = 2 };
static struct dgl_file g_std[2] = { { F_LOG, -1, 0, 0, 0, 0 }, { F_LOG, -1, 0, 0, 0, 0 } };
FILE *const dgl_stdout = &g_std[0];
FILE *const dgl_stderr = &g_std[1];
#define NFILES 4
static struct dgl_file g_files[NFILES];

FILE *fopen(const char *path, const char *mode){
    for (const char *m = mode; *m; m++)
        if (*m == 'w' || *m == 'a' || *m == '+'){ errno = EROFS; return NULL; }
    struct dgl_file *f = NULL;
    for (int i = 0; i < NFILES; i++) if (g_files[i].kind == F_FREE){ f = &g_files[i]; break; }
    if (!f){ errno = ENOMEM; return NULL; }
    int h = dgl_fs_open(path);
    if (h < 0){ errno = ENOENT; return NULL; }
    long sz = dgl_fs_size(h);
    if (sz < 0){ dgl_fs_close(h); errno = EIO; return NULL; }
    f->kind = F_FILE; f->h = h; f->pos = 0; f->size = (uint32_t)sz; f->eof = f->err = 0;
    return f;
}
int fclose(FILE *f){
    if (!f || f->kind != F_FILE) return EOF;
    dgl_fs_close(f->h);
    f->kind = F_FREE;
    return 0;
}
size_t fread(void *p, size_t size, size_t n, FILE *f){
    if (!f || f->kind != F_FILE || !size) return 0;
    uint32_t want = (uint32_t)(size * n);
    if (f->pos >= f->size){ f->eof = 1; return 0; }
    if (want > f->size - f->pos){ want = f->size - f->pos; f->eof = 1; }
    int r = dgl_fs_read(f->h, f->pos, p, want);
    if (r < 0){ f->err = 1; return 0; }
    f->pos += (uint32_t)r;
    return (size_t)r / size;
}
size_t fwrite(const void *p, size_t size, size_t n, FILE *f){
    if (f && f->kind == F_LOG){ dgl_log_write(p, size * n); return n; }
    errno = EROFS;
    return 0;
}
int fseek(FILE *f, long off, int whence){
    if (!f || f->kind != F_FILE) return -1;
    long base = whence == SEEK_CUR ? (long)f->pos : whence == SEEK_END ? (long)f->size : 0;
    long np = base + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np; f->eof = 0;
    return 0;
}
long ftell(FILE *f){ return f && f->kind == F_FILE ? (long)f->pos : -1; }
int fflush(FILE *f){ (void)f; return 0; }
int feof(FILE *f){ return f ? f->eof : 1; }
int ferror(FILE *f){ return f ? f->err : 1; }
int fgetc(FILE *f){
    uint8_t c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}
char *fgets(char *s, int n, FILE *f){
    int i = 0;
    while (i + 1 < n){
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}
int fputs(const char *s, FILE *f){ return fwrite(s, 1, strlen(s), f) ? 0 : EOF; }
int fputc(int c, FILE *f){ char ch = (char)c; return fwrite(&ch, 1, 1, f) ? c : EOF; }
int vfprintf(FILE *f, const char *fmt, va_list ap){
    if (!f || f->kind != F_LOG){ errno = EROFS; return -1; }
    return log_vprintf(fmt, ap);
}
int fprintf(FILE *f, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r;
}
int remove(const char *path){ (void)path; errno = EROFS; return -1; }
int rename(const char *a, const char *b){ (void)a; (void)b; errno = EROFS; return -1; }
int mkdir(const char *path, mode_t mode){ (void)path; (void)mode; errno = EROFS; return -1; }
int stat(const char *path, struct stat *st){ (void)path; (void)st; errno = ENOENT; return -1; }
int access(const char *path, int mode){
    if (mode & 2) return -1;                            /* W_OK: 只读 */
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fclose(f);
    return 0;
}
