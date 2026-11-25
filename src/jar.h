/*
 * fpj2me — jar.h : JAR/ZIP access straight from SD via libc_embed.
 * miniz (tinfl) is used for DEFLATE only — no MZ_FILE, no zlib header.
 */
#ifndef FPJ2ME_JAR_H
#define FPJ2ME_JAR_H

#include "platform.h"

typedef struct {
    u32 name_off;    /* offset into Jar::names                         */
    u32 lho;         /* offset of the local file header in the zip     */
    u32 csize;       /* compressed size                                 */
    u32 usize;       /* uncompressed size                               */
    u16 method;      /* 0 = stored, 8 = deflate (anything else = err)  */
} JarEntry;

typedef struct {
    fp_file  *f;
    JarEntry *ent;
    char     *names;                 /* packed entry names, NUL-term.   */
    u32       n;
} Jar;

/* Opens path on SD and indexes the central directory. 0 on success. */
int  jar_open(Jar *j, const char *path);
void jar_close(Jar *j);

/* the two classpath jars (opened by fpj2me_boot)                     */
int  jar_open_boot(void);          /* FPJ_SD_ROOT "boot.jar"          */
int  jar_open_game(const char *path);  /* any *.jar from the menu      */
void jar_close_game(void);         /* shutdown / re-boot             */
Jar *jar_boot(void);
Jar *jar_game(void);
int  boot_jar_ok(void);
int  game_jar_ok(void);
/* RMS folder name for the ACTIVE suite = sanitized jar base name
 * ("ac.jar" -> "ac"), so several games share rms/ without mixing  */
const char *jar_suite(void);

/* MIDP 2.0 application descriptor: load the sibling <jar_basename>.jad
 * that sits next to `path` (e.g. "ac1j2me.jad" next to "ac1j2me.jar").
 * Reads at most dstsz-1 bytes into dst, NUL-terminates it. Returns 0
 * when a .jad was found and read, -1 when there is none.            */
int  jad_load_sibling(const char *path, char *dst, u32 dstsz);

/* Exact-match (case-sensitive) lookup; NULL if absent.              */
const JarEntry *jar_find(const Jar *j, const char *name);

/* Prefix scan for class-path wildcards: iterate entries whose name
 * starts with `prefix` and ends with `suffix`; pass prev=NULL to
 * start. Useful for "res/" folders and device sub-dirs in GL jars.  */
const JarEntry *jar_find_next(const Jar *j, const char *prefix,
                              const char *suffix, const JarEntry *prev);

/* Decompress one entry into a fresh fp_alloc() buffer (caller frees).
 * Returns NULL on corrupt data. *out_size receives usize.           */
uint8_t *jar_read(const Jar *j, const JarEntry *e, uint32_t *out_size);

#endif
