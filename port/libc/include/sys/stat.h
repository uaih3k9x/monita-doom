#ifndef DGL_SYS_STAT_H
#define DGL_SYS_STAT_H
#include <sys/types.h>
struct stat { off_t st_size; mode_t st_mode; };
int mkdir(const char *path, mode_t mode);
int stat(const char *path, struct stat *st);
#define S_ISDIR(m) (0)
#endif
