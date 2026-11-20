#ifndef DGL_UNISTD_H
#define DGL_UNISTD_H
#include <sys/types.h>
int access(const char *path, int mode);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#endif
