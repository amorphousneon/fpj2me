/*
 * fpj2me — libc_shims.h
 * The minimum C library surface that miniz.c (tinfl), stb_image.h and
 * fpj2me itself need under  -ffreestanding -nostdlib .
 *
 * Include order in exactly ONE translation unit per binary:
 *     #include "libc_shims.h"
 *     #define MINIZ_NO_STDIO
 *     #include "miniz.c"          (=> tinfl inflate only)
 *     #define STBI_ONLY_PNG
 *     #define STBI_NO_STDIO
 *     #define STBI_MALLOC  fp_alloc   ... (see midp_image.c)
 *     #include "stb_image.h"
 */
#ifndef FPJ2ME_LIBC_SHIMS_H
#define FPJ2ME_LIBC_SHIMS_H
#include <stddef.h>
#include <stdarg.h>

/* --- string / mem. Real definitions are mandatory: even with        */
/*     -ffreestanding GCC emits calls to memcpy/memset/memmove.       */
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);

/* --- malloc family: route every native allocation to fpdoom arenas. */
/*     (Java heap uses heap_alloc() separately; these are for native   */
/*      buffers: zip entries, decoded pixels, class bytes, RMS caches) */
void  *fp_alloc(size_t n);
void  *fp_realloc(void *p, size_t n);
void   fp_free(void *p);

/* With MINIZ_NO_STDIO defined, miniz needs no FILE* glue at all: we
 * read zip entries ourselves with fp_fread() and inflate with tinfl only. */

/* --- tiny printf routed to DBG_LOG (fpdoom provides one; else stub) */
int    fp_printf(const char *fmt, ...);

#endif
