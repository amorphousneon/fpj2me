/*
 * fpj2me — jar.c : JAR/ZIP loader over fpdoom libc_embed + miniz tinfl.
 *
 * Strategy (keeps RAM flat and tiny):
 *  1. Read the last 66 KB of the file and scan backwards for the
 *     End-of-Central-Directory record (max zip comment is 65535 B).
 *  2. Read the whole central directory ONCE into fp_alloc() memory
 *     (a 600-class Gameloft jar has ~600-900 entries => ~40 KB).
 *  3. Per entry read: seek to its local header, skip name+extra,
 *     read csize bytes, inflate with tinfl_decompress_mem_to_mem().
 *
 * Peak RAM per jar_read(): csize + usize (e.g. 90 KB PNG => ~250 KB).
 */
#include "fpj2me.h"
#include "jar.h"
#include "libc_shims.h"

/* tinfl via miniz.c compiled once elsewhere with MINIZ_NO_STDIO.      */
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO        /* keep <stdio.h> out of the -nostdlib build */
#endif
#include "miniz.h"

/* ---- static inflate state ------------------------------------------ *
 * sizeof(tinfl_decompressor) is 8376 bytes. miniz's stock
 * tinfl_decompress_mem_to_mem() keeps it as a LOCAL — that is 8.4 KB
 * of C stack per jar_read(). The app's stack region is fixed by
 * fpdoom's sc6531e_fdl.ld (EXTRA_SIZE 0x9000 = 36 KB); the old
 * recursive class_load put one jar_read per hierarchy level on it,
 * so a real Gameloft super chain (10+ classes) overflowed the stack
 * and froze the phone mid-boot. The runtime is single-threaded: one
 * static decompressor is safe and keeps the stack flat.            */
static tinfl_decompressor g_infl;

static size_t fp_inflate(void *out, size_t out_n,
                         const void *src, size_t src_n)
{
    /* mirrors tinfl_decompress_mem_to_mem(..., flags=0) with the
     * 8.4 KB state in BSS instead of on the C stack                  */
    size_t in_n = src_n, out_left = out_n;
    tinfl_init(&g_infl);
    tinfl_status st = tinfl_decompress(&g_infl, (const mz_uint8 *)src,
                                       &in_n, (mz_uint8 *)out,
                                       (mz_uint8 *)out, &out_left,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return (st != TINFL_STATUS_DONE) ? (size_t)TINFL_DECOMPRESS_MEM_TO_MEM_FAILED
                                     : out_left;
}

#define EOCD_SIG   0x06054b50u
#define CEN_SIG    0x02014b50u
#define LOC_SIG    0x04034b50u

/* ---- classpath jars ------------------------------------------------- */
static Jar boot_jar, game_jar;
static int boot_ok, game_ok;
static char suite_name[36];        /* sanitized jar base name          */

int  jar_open_boot(void) { boot_ok = (jar_open(&boot_jar, FPJ_SD_ROOT "boot.jar") == 0); return boot_ok ? 0 : -1; }

int jar_open_game(const char *path)
{
    /* FPJ_SD_ROOT is "" on target (paths resolve via --dir curdir) and
     * the sandbox prefix on the host tests — concat without snprintf  */
    char full[128];
    int i = 0;
    for (const char *r = FPJ_SD_ROOT; *r && i < 110; r++) full[i++] = *r;
    for (const char *p = path; *p && i < 126; p++)       full[i++] = *p;
    full[i] = 0;

    game_ok = (jar_open(&game_jar, full) == 0);
    suite_name[0] = 0;
    if (!game_ok) return -1;

    /* suite name = jar basename, cut at the extension, lowercased,
     * [a-z0-9_-] only, <=32 chars: rms/<suite>/<store>.rms             */
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    int si = 0;                       /* suite-name fill index            */
    for (const char *p = base; *p && *p != '.' && si < 32; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')
            suite_name[si++] = c;
    }
    suite_name[si] = 0;               /* si==0 -> fallback below          */
    if (!suite_name[0]) {             /* name was empty/all-stripped      */
        suite_name[0] = 's'; suite_name[1] = 0;
    }
    return 0;
}

void jar_close_game(void)
{
    jar_close(&game_jar);
    game_ok = 0;
}

Jar *jar_boot(void) { return &boot_jar; }
Jar *jar_game(void) { return &game_jar; }
int  boot_jar_ok(void) { return boot_ok; }
int  game_jar_ok(void) { return game_ok; }
const char *jar_suite(void) { return suite_name[0] ? suite_name : "suite"; }

int jad_load_sibling(const char *path, char *dst, u32 dstsz)
{
    if (!dst || dstsz < 2) return -1;
    dst[0] = 0;

    /* full path = FPJ_SD_ROOT + path (same concat as jar_open_game)   */
    char full[128];
    int i = 0;
    for (const char *r = FPJ_SD_ROOT; *r && i < 110; r++) full[i++] = *r;
    for (const char *p = path; *p && i < 122; p++)        full[i++] = *p;
    full[i] = 0;

    /* swap the trailing ".jar" for ".jad" (case-insensitive); a path
     * with no .jar extension just gets ".jad" appended                */
    int base_len = i;
    if (i >= 4 && full[i-4] == '.' &&
        (full[i-3] == 'j' || full[i-3] == 'J') &&
        (full[i-2] == 'a' || full[i-2] == 'A') &&
        (full[i-1] == 'r' || full[i-1] == 'R'))
        base_len = i - 4;
    if (base_len + 4 + 1 > (int)sizeof full) return -1;
    full[base_len++] = '.';
    full[base_len++] = 'j';
    full[base_len++] = 'a';
    full[base_len++] = 'd';
    full[base_len] = 0;

    fp_file *f = fp_fopen(full, "rb");
    if (!f) return -1;                      /* no sibling .jad: fine   */

    int got = fp_fread(dst, 1, (int)(dstsz - 1), f);
    fp_fclose(f);
    if (got <= 0) return -1;
    dst[got] = 0;                           /* JADs are tiny text files */
    return 0;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((u32)p[3]<<24); }

static long fsize(fp_file *f)
{
    long cur = fp_ftell(f);
    fp_fseek(f, 0, 2 /*SEEK_END*/);
    long end = fp_ftell(f);
    fp_fseek(f, cur, 0 /*SEEK_SET*/);
    return end;
}

int jar_open(Jar *j, const char *path)
{
    memset(j, 0, sizeof *j);
    j->f = fp_fopen(path, "rb");
    if (!j->f) return -1;

    long fsz = fsize(j->f);
    long win = fsz < 66000 ? fsz : 66000;

    if (fpj_telemetry) fpj_stage("fopen ok");

    u8 *buf = fp_alloc((size_t)win);
    if (!buf) { fp_fclose(j->f); j->f = 0; return -1; }
    fp_fseek(j->f, fsz - win, 0);
    if (fp_fread(buf, 1, (int)win, j->f) != win) goto fail;

    /* scan backwards for EOCD */
    long eocd = -1;
    for (long i = win - 22; i >= 0; i--)
        if (rd32(buf + i) == EOCD_SIG) { eocd = i; break; }
    if (eocd < 0) goto fail;
    if (fpj_telemetry) fpj_stage("eocd");

    u32 n_ent  = rd16(buf + eocd + 10);
    u32 cd_off = rd32(buf + eocd + 16);
    u32 cd_sz  = rd32(buf + eocd + 12);

    /* zip64 not expected on 2007-era MIDP jars — reject defensively  */
    if (cd_off == 0xFFFFFFFFu || n_ent == 0xFFFFu) goto fail;

    u8 *cd = fp_alloc(cd_sz);
    if (!cd) goto fail;
    fp_fseek(j->f, (long)cd_off, 0);
    if (fp_fread(cd, 1, (int)cd_sz, j->f) != (int)cd_sz) goto fail_cd;
    if (fpj_telemetry) fpj_stage("cdir");

    j->ent   = fp_alloc(n_ent * sizeof(JarEntry));
    j->names = fp_alloc(cd_sz);          /* upper bound; names only     */
    if (!j->ent || !j->names) goto fail_cd;

    u32 p = 0, k = 0, no = 0;
    while (k < n_ent && p + 46 <= cd_sz) {
        if (rd32(cd + p) != CEN_SIG) goto fail_cd;
        u16 method = rd16(cd + p + 10);
        u32 csize  = rd32(cd + p + 20);
        u32 usize  = rd32(cd + p + 24);
        u16 nlen   = rd16(cd + p + 28);
        u16 elen   = rd16(cd + p + 30);
        u16 clen   = rd16(cd + p + 32);
        u32 lho    = rd32(cd + p + 42);
        if (method != 0 && method != 8) { p += 46 + nlen + elen + clen; continue; }
        j->ent[k].name_off = no;
        j->ent[k].lho    = lho;
        j->ent[k].csize  = csize;
        j->ent[k].usize  = usize;
        j->ent[k].method = method;
        memcpy(j->names + no, cd + p + 46, nlen);
        j->names[no + nlen] = 0;
        no += (u32)nlen + 1;
        k++;
        p += 46 + nlen + elen + clen;
    }
    j->n = k;
    fp_free(cd);
    fp_free(buf);
    return 0;

fail_cd: fp_free(cd);
fail:    fp_free(buf); fp_fclose(j->f); memset(j, 0, sizeof *j);
    return -1;
}

void jar_close(Jar *j)
{
    if (j->f) fp_fclose(j->f);
    if (j->ent)  fp_free(j->ent);
    if (j->names) fp_free(j->names);
    memset(j, 0, sizeof *j);
}

const JarEntry *jar_find(const Jar *j, const char *name)
{
    for (u32 i = 0; i < j->n; i++)
        if (!strcmp(j->names + j->ent[i].name_off, name))
            return &j->ent[i];
    return 0;
}

const JarEntry *jar_find_next(const Jar *j, const char *prefix,
                              const char *suffix, const JarEntry *prev)
{
    size_t pl = strlen(prefix), sl = strlen(suffix);
    u32 start = prev ? (u32)(prev - j->ent) + 1 : 0;
    for (u32 i = start; i < j->n; i++) {
        const char *nm = j->names + j->ent[i].name_off;
        size_t l = strlen(nm);
        if (l >= pl + sl && !memcmp(nm, prefix, pl) &&
            !memcmp(nm + l - sl, suffix, sl))
            return &j->ent[i];
    }
    return 0;
}

uint8_t *jar_read(const Jar *j, const JarEntry *e, uint32_t *out_size)
{
    uint8_t lhdr[30];
    fp_fseek(j->f, (long)e->lho, 0);
    if (fp_fread(lhdr, 1, 30, j->f) != 30) return 0;
    if (rd32(lhdr) != LOC_SIG) return 0;
    if (fpj_telemetry) fpj_stage("entry hdr");

    u16 nlen = rd16(lhdr + 26), elen = rd16(lhdr + 28);
    fp_fseek(j->f, (long)(e->lho + 30 + nlen + elen), 0);

    uint8_t *out = fp_alloc(e->usize ? e->usize : 1);
    if (!out) return 0;

    if (e->method == 0) {                      /* stored: straight copy */
        if (fp_fread(out, 1, (int)e->csize, j->f) != (int)e->csize)
            { fp_free(out); return 0; }
    } else {                                   /* deflate: raw stream   */
        uint8_t *src = fp_alloc(e->csize ? e->csize : 1);
        if (!src) { fp_free(out); return 0; }
        if (fp_fread(src, 1, (int)e->csize, j->f) != (int)e->csize)
            { fp_free(src); fp_free(out); return 0; }
        if (fpj_telemetry) fpj_stage("inflate");
        /* raw DEFLATE: NO zlib header => non-wrapping output only     */
        size_t got = fp_inflate(out, e->usize, src, e->csize);
        fp_free(src);
        if (got != e->usize) { fp_free(out); return 0; }
    }
    if (fpj_telemetry) fpj_stage("entry ok");
    if (out_size) *out_size = e->usize;
    return out;
}
