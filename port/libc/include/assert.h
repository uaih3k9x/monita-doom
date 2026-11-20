#ifndef DGL_ASSERT_H
#define DGL_ASSERT_H
void dgl_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : dgl_assert_fail(#e, __FILE__, __LINE__))
#endif
#endif
