/*
 * fpj2me — tests/boot_jar.c : host rig that boots an ARBITRARY jar
 * through the real fpj2me_boot() path (same code as the phone, weak
 * telemetry overridden to print to stdout). Used to reproduce
 * on-target boot hangs with real-world jars:
 *
 *   ./boot_jar /path/to/game.jar
 *
 * Exit: 0 = boot returned, 1 = boot error code printed, 124 = hang
 * (use `timeout`), 139 = segfault (wild pointer in the parser).
 *
 * The clock is REAL (CLOCK_MONOTONIC) and fp_sleep_ms really sleeps:
 * Gameloft frame loops pace themselves with System.currentTime-
 * Millis() and Thread.sleep(); the old frozen fake clock left them
 * stuck inside the first 100 ms frame-limit wait forever (host only
 * — the phone always had a real clock).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "fpj2me.h"
#include "midp.h"
#include "platform.h"
#include "fp_font.h"
#include <time.h>

/* ---- optional 36 KB-thread mode ------------------------------------ *
 * The phone's app stack is 36 KB (sc6531e_fdl.ld EXTRA_SIZE 0x9000).
 * FPJ_STACK_KB=36 makes this rig run fpj2me_boot in a pthread with
 * EXACTLY that much stack, so any stack overflow (like the old
 * recursive class_load + per-level tinfl state) segfaults on the
 * host too instead of only freezing the phone.                      */
#ifdef __unix__
#include <pthread.h>
static const char *g_jar;
static int g_rc;
#endif
static int pump_n = 3;                 /* FPJ_PUMP_N env override      */
static void pump_frames(void)
{
    for (int i = 0; i < pump_n && !fpj_midlet_done; i++)
        fpj2me_pump();
    printf("[result] pumped %d frames, done=%d\n", pump_n, fpj_midlet_done);
}
#ifdef __unix__
static void *boot_thread(void *arg)
{
    (void)arg;
    g_rc = fpj2me_boot(g_jar);
    printf("[result] fpj2me_boot -> %d\n", g_rc);
    if (g_rc == 0) pump_frames();
    return 0;
}
#endif

/* ---- fpdoom HAL stubs (same contract as tests/hostcheck.c) --------- */
uint16_t fp_screen[FP_LCD_W * FP_LCD_H];
int fp_rawmap[KP_MAX];
static uint32_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
uint32_t fp_time_ms(void)
{
    static uint32_t t0;
    static int      t0_set;
    uint32_t now = wall_ms();
    if (!t0_set) { t0 = now; t0_set = 1; }
    return now - t0;
}
void fp_sleep_ms(uint32_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}
/* mirrors fp_platform.c on target: the diag strip renders on flush */
void fp_lcd_flush(void) { fpj_diag_overlay(); }
void fp_dbg(const char *s) { fputs(s, stderr); fputc('\n', stderr); }
uint32_t fp_keys_held(void) { return 0; }
int fp_key_event(fp_key_t *k, int *p) { (void)k; (void)p; return 0; }

/* ---- Round 10: mock USB pipe (same contract as hostcheck.c) ---------
 * FPJ_USB_FRAMES=N preloads N synthetic UsbCast frames before the
 * MIDlet boots; when they are consumed the mock reports link-EOF,
 * which makes UsbCast.run() return (the exit path the target reaches
 * with the EXIT KEY instead). TX (ACKs) go to stderr for eyeballing. */
#define BJ_RX_CAP (256 * 1024)
#define BJ_TX_CAP 256
static u8  bj_rx[BJ_RX_CAP]; static u32 bj_rx_len, bj_rx_pos;
static u8  bj_tx[BJ_TX_CAP]; static u32 bj_tx_len;
static int bj_eof;
static u16 bj_seed = 0x1234;
static void bj_feed_frame(void)
{
    u8 hdr[9] = { 'F', 'P', 'J', '2', 128, 0, 160, 0, 0 };
    if (BJ_RX_CAP - bj_rx_len < 9 + 40960) return;
    memcpy(bj_rx + bj_rx_len, hdr, 9);
    bj_rx_len += 9;
    for (u32 i = 0; i < 40960; i += 2) {     /* walkable RGB565 gradient */
        u16 v = bj_seed + (u16)(i >> 1);
        bj_rx[bj_rx_len++] = (u8)v;
        bj_rx[bj_rx_len++] = (u8)(v >> 8);
    }
}
int fp_usb_read(void *dst, unsigned len)
{
    u32 avail = bj_rx_len - bj_rx_pos;
    if (!avail) return bj_eof ? -1 : 0;
    if (len > avail) len = avail;
    memcpy(dst, bj_rx + bj_rx_pos, len);
    bj_rx_pos += len;
    return (int)len;
}
int fp_usb_write(const void *src, unsigned len)
{
    if (len > BJ_TX_CAP - bj_tx_len) len = BJ_TX_CAP - bj_tx_len;
    if (len) { memcpy(bj_tx + bj_tx_len, src, len); bj_tx_len += len; }
    return (int)len;
}
unsigned fp_usb_available(void) { return bj_rx_len - bj_rx_pos; }
void fp_usb_reset(void) { bj_rx_len = bj_rx_pos = bj_tx_len = 0; bj_eof = 0; }
/* Round 11: natives call this on comm:usb open; boot_jar's pipe is
 * "always up" (frames preloaded by fpj_bj_usb_setup), so this is a
 * permanent success - the cold-init sequence itself is hostcheck's
 * mock's business (usb_cold_seqs).                                     */
int fp_usb_init(void) { return 0; }
void fpj_bj_usb_setup(int nframes)
{
    for (int i = 0; i < nframes; i++) bj_feed_frame();
    bj_eof = 1;          /* after the scripted frames: link gone       */
    fprintf(stderr, "[usb] fed %d synthetic frames (EOF after)\n", nframes);
}
void fpj_bj_usb_report(void)
{
    fprintf(stderr, "[usb] tx bytes: %u: ", bj_tx_len);
    for (u32 i = 0; i < bj_tx_len && i < 32; i++)
        fputc(bj_tx[i] >= 32 && bj_tx[i] < 127 ? bj_tx[i] : '?', stderr);
    fputc('\n', stderr);
}

fp_file *fp_fopen(const char *p, const char *m) { return (fp_file *)fopen(p, m); }
int fp_fread(void *b, int s, int n, fp_file *f) { return (int)fread(b, s, n, (FILE *)f); }
int fp_fwrite(const void *b, int s, int n, fp_file *f) { return (int)fwrite(b, s, n, (FILE *)f); }
int fp_fseek(fp_file *f, long o, int w) { return fseek((FILE *)f, o, w); }
long fp_ftell(fp_file *f) { return ftell((FILE *)f); }
int fp_fclose(fp_file *f) { return fclose((FILE *)f); }

void *fp_alloc(size_t n) { return malloc(n ? n : 1); }
void *fp_realloc(void *p, size_t n) { return realloc(p, n); }
void fp_free(void *p) { free(p); }
int fp_printf(const char *fmt, ...) { va_list a; va_start(a, fmt);
    int r = vprintf(fmt, a); va_end(a); return r; }

/* strong overrides of interp.c's weak no-op telemetry */
int fpj_telemetry_dummy;   /* keep the linker honest (unused)          */

void fpj_stage(const char *msg)
{
    printf("[stage] %s\n", msg);
    fflush(stdout);
}

void fpj_stage_class(const char *name)
{
    printf("[class] %s\n", name);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: boot_jar <jar> [stack_kb]\n"); return 2; }
    g_jar = argv[1];

    const char *pn = getenv("FPJ_PUMP_N");
    if (pn && *pn) {
        pump_n = (int)strtol(pn, 0, 10);
        if (pump_n < 0) pump_n = 3;
    }

    /* Round 10: preload synthetic UsbCast frames when asked for      */
    const char *uf = getenv("FPJ_USB_FRAMES");
    if (uf && *uf) {
        int nf = (int)strtol(uf, 0, 10);
        if (nf < 0) nf = 0;
        if (nf > 5) nf = 5;
        fpj_bj_usb_setup(nf);
    }

#ifdef __unix__
    const char *skb = getenv("FPJ_STACK_KB");
    if (argc > 2) skb = argv[2];
    if (skb && *skb) {
        long kb = strtol(skb, 0, 10);
        if (kb > 0) {
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, (size_t)kb * 1024);
            pthread_t th;
            if (pthread_create(&th, &at, boot_thread, 0) != 0) {
                fprintf(stderr, "pthread_create failed\n"); return 2;
            }
            pthread_join(th, 0);
            fpj_bj_usb_report();
            return g_rc != 0 ? 1 : 0;
        }
    }
#endif
    g_rc = fpj2me_boot(g_jar);
    printf("[result] fpj2me_boot -> %d\n", g_rc);
    fpj_bj_usb_report();
    if (g_rc != 0) return 1;

    /* pump frames like target_main would, then stop. FPJ_PUMP_N runs
     * a longer slice (default 3): boot-time hangs AND first-frames
     * crashes (paint-path NPEs) both reproduce here.                 */
    pump_frames();
    return 0;
}
