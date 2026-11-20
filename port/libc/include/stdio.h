/* monita-doom 精简 libc (设备 / QEMU 用): 只读文件 + 日志输出。见 port/libc/libc.c */
#ifndef DGL_STDIO_H
#define DGL_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct dgl_file FILE;
extern FILE *const dgl_stdout, *const dgl_stderr;
#define stdout dgl_stdout
#define stderr dgl_stderr
#define stdin  ((FILE *)0)
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 256
#define FILENAME_MAX 256
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
int    fflush(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fgetc(FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fputs(const char *s, FILE *f);
int    fputc(int c, FILE *f);
int    remove(const char *path);
int    rename(const char *a, const char *b);
int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    sprintf(char *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int    vsprintf(char *s, const char *fmt, va_list ap);
int    sscanf(const char *s, const char *fmt, ...);
int    puts(const char *s);
int    putchar(int c);
#define putc fputc
#define getc fgetc
#endif
