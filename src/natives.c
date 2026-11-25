/*
 * fpj2me — natives.c : the native method registry.
 *
 * Every java.lang / javax.microedition method the games call that has
 * no .class file resolves here (class stubs from interp.c carry only
 * the name chain). Missing entries fail LOUDLY through fpj_panic at
 * the exact call site — extend this table per game.
 *
 * Object model conventions (payload slots of T_OBJECT stubs):
 *   String        = T_ARR_C object directly: [u32 len][u16 chars...]
 *   StringBuffer  = slot0 T_ARR_C buffer (ARR_LEN=capacity), slot1 count
 *   Vector        = slot0 T_ARR_A elements, slot3 count
 *   Hashtable     = slot0 T_ARR_A pairs [k,v,...]
 *   Random        = slot1/2 = 64-bit seed
 *   Image         = slot0 JImage*, slot1 w, slot2 h
 *   Graphics      = slot0 JImage*, 1..4 clip, 5..6 translate,
 *                   7 color565, 8 argb
 *   Player        = slot0 SilentPlayer*
 *   RecordStore   = slot1 rms handle
 */
#include "fpj2me.h"
#include "midp.h"
#include "jar.h"
#include "libc_shims.h"
#include "fp_font.h"
#include <stdio.h>    /* snprintf: overlay + fatal screen text    */
#include <stdlib.h>   /* exit(): fatal path reboots to fpmenu      */

/* ------------------------------------------------------------------ */
/* runtime vitals + the on-target fatal screen                          */
/* ------------------------------------------------------------------ */
/* LIBC_SDIO=3 has NO log channel: everything the old code only sent to
 * fp_dbg (uncaught exceptions in paint()/keyPressed(), missing native
 * methods) was INVISIBLE on the phone and looked exactly like the AC1
 * post-splash freeze. These globals + fpj_diag_overlay()/fpj_fatal()
 * put the same information ON THE LCD instead.                       */
char fpj_miss_native[96];        /* last unmodeled "cls.name" called   */
u32  fpj_paint_count;            /* paint() deliveries                 */
u32  fpj_last_paint_ms;          /* fp_time_ms() of the last paint     */
extern u32 fpj_key_events;       /* defined in midp_input.c            */

/* raw keypad codes the firmware produces but fp_platform.c maps to
 * nothing (KP_NONE) — a dead key audit. Weak: host rigs have no
 * fp_platform.c, target_main's strong definition wins.              */
__attribute__((weak)) int fpj_dead_raw[2] = { -1, -1 };

/* wrap-draw for the 16x8-char grid, returns the next free row       */
static int draw_wrap(int row, const char *s, int max_rows, u16 fg)
{
    while (s && *s && max_rows > 0) {
        char line[17];
        int i;
        for (i = 0; i < 16 && s[i]; i++) line[i] = s[i];
        line[i] = 0;
        fp_draw_text(2, row * FP_FONT_H, line, fg);
        s += i;
        row++;
        max_rows--;
    }
    return row;
}

/* ------------------------------------------------------------------ */
/* THE FATAL SCREEN (target issue 3c)                                   */
/* ------------------------------------------------------------------ */
/* Any exception that unwinds to a C boundary the game did NOT catch
 * (paint(), keyPressed()/keyReleased(), a cooperative thread's run(),
 * startApp) lands here: full class name + message on a red screen,
 * with heap/GC/VM vitals so an OutOfMemoryError is self-evident.
 * The hook then ends the app (default: keypress or 10 s -> exit(3) ->
 * watchdog reset -> fpmenu; host rigs override to record + return). */
__attribute__((weak)) void fpj_on_fatal(void)
{
    fp_key_t k; int pressed, waited = 0;
    for (;;) {
        while (fp_key_event(&k, &pressed))
            if (pressed) break;
        if (++waited > 500) break;         /* ~10 s fallback             */
        fp_sleep_ms(20);
    }
    rms_close_all();
    exit(3);
}

void fpj_fatal(const char *ctx)
{
    char cls[80], msg[64];
    JObj *eo = obj(fpj_pending_exc);

    cls[0] = msg[0] = 0;
    if (eo && eo->clazz) {               /* dotted class name           */
        const char *p = ((JClass *)eo->clazz)->name;
        int i = 0;
        for (; p && *p && i < 79; p++)
            cls[i++] = (*p == '/') ? '.' : *p;
        cls[i] = 0;
    }
    if (eo && eo->type == T_OBJECT && eo->size >= 8) {
        JObj *mo = obj((JRef)OBJ_PAYLOAD(eo)[1]);
        if (mo && mo->type == T_ARR_C) {
            u32 n = ARR_LEN(mo);
            if (n > 63) n = 63;
            for (u32 k = 0; k < n; k++) msg[k] = (char)(u8)((u16 *)ARR_DATA(mo))[k];
            msg[n] = 0;
        }
    }

    fpj_telemetry = 0;                   /* nothing paints over this    */
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0xF800);
    fp_draw_text(2, 1 * FP_FONT_H, "uncaught exception", 0xFFFF);
    {   int r = draw_wrap(3, cls[0] ? cls : "(null)", 2, 0xFFFF);
        if (msg[0]) draw_wrap(r, msg, 2, 0xFFFF); }
    fp_draw_text(2, 7 * FP_FONT_H, ctx, 0x07FF);
    {
        char b[28];
        snprintf(b, sizeof b, "hf=%uk gc=%u",
                 (unsigned)(heap_free_bytes() / 1024), (unsigned)fpj_gc_count);
        fp_draw_text(2, 9 * FP_FONT_H, b, 0xFFFF);
        snprintf(b, sizeof b, "v=%uk", (unsigned)(fpj_vcpu / 1000));
        fp_draw_text(2, 10 * FP_FONT_H, b, 0xFFFF);
    }
    if (fpj_miss_native[0])
        draw_wrap(12, fpj_miss_native, 2, 0x0000);   /* black on red    */
    fp_draw_text(2, 17 * FP_FONT_H, "press any key", 0x07FF);
    fp_lcd_flush();

    fp_dbg("fpj2me FATAL ");
    fp_dbg(ctx);
    fp_dbg(cls);
    fp_dbg(msg);
    fpj_pending_exc = 0;
    fpj_on_fatal();                     /* exit(3) / host records      */
}

/* ------------------------------------------------------------------
 * diag overlay: 5 top rows of live vitals (opt-in — create an empty
 * file named "diag" in the game folder). Reading it answers every
 * post-splash freeze question WITHOUT a USB log:
 *   t/gc/c    uptime s, GC runs (racing = pressure), classes loaded
 *   v/hf      VM dispatches (frozen = stuck in C: SD read/inflate/GC),
 *             heap free KB
 *   k/p/lp    key events queued (0 while typing = keypad/dead mapping),
 *             paints delivered, ms since last paint
 *   C:        class parse_one touched last (frozen = loading it)
 *   M:        last library method that hit the no-op fallback — the
 *             classic silent killer (e.g. Object.wait returning null)
 * ------------------------------------------------------------------ */
void fpj_diag_overlay(void)
{
    static u32 last_ms;
    u32 now = fp_time_ms();
    if (!fpj_diag || (u32)(now - last_ms) < 500) return;
    last_ms = now;

    /* M: wraps onto a 6th row when the full "Class.method" does not
     * fit one row — the old 14-char clip showed only "com/nokia/mid/"
     * and hid the actual method (Sound.getState, ...) at the screen
     * edge, exactly the detail that names the stall                 */
    u32 miss_len = 0;
    while (fpj_miss_native[miss_len] && miss_len < 94) miss_len++;
    int rows = (fpj_miss_native[0] && miss_len > 14) ? 6 : 5;

    char b[28];
    fp_fill_rect(0, 0, FP_LCD_W, rows * FP_FONT_H, 0x0000);
    snprintf(b, sizeof b, "t=%u gc=%u c=%d",
             now / 1000, (unsigned)fpj_gc_count, class_count());
    fp_draw_text(2, 0, b, 0x07FF);
    snprintf(b, sizeof b, "v=%uk hf=%uk",
             (unsigned)(fpj_vcpu / 1000), (unsigned)(heap_free_bytes() / 1024));
    fp_draw_text(2, 1 * FP_FONT_H, b, 0x07FF);
    snprintf(b, sizeof b, "k=%u p=%u lp=%u",
             (unsigned)fpj_key_events, (unsigned)fpj_paint_count,
             fpj_paint_count ? (unsigned)(now - fpj_last_paint_ms) : 0u);
    fp_draw_text(2, 2 * FP_FONT_H, b, 0x07FF);
    if (fpj_last_class[0]) {
        char c2[20]; c2[0] = 'C'; c2[1] = ':';
        int i; for (i = 0; i < 14 && fpj_last_class[i]; i++) c2[2 + i] = fpj_last_class[i];
        c2[2 + i] = 0;
        fp_draw_text(2, 3 * FP_FONT_H, c2, 0xFFE0);
    }
    if (fpj_miss_native[0]) {
        /* full Class.method: chars 0..13 on row 4, the rest on row 5 */
        char m2[20];
        m2[0] = 'M'; m2[1] = ':';
        int i;
        for (i = 0; i < 14 && fpj_miss_native[i]; i++) m2[2 + i] = fpj_miss_native[i];
        m2[2 + i] = 0;
        fp_draw_text(2, 4 * FP_FONT_H, m2, 0xF800);
        if (miss_len > 14) {
            for (i = 0; i < 15 && fpj_miss_native[14 + i]; i++)
                m2[i] = fpj_miss_native[14 + i];
            m2[i] = 0;
            fp_draw_text(2, 5 * FP_FONT_H, m2, 0xF800);
        }
    } else if (fpj_dead_raw[0] >= 0) {
        snprintf(b, sizeof b, "R:%02x,%02x dead",
                 (unsigned)fpj_dead_raw[0],
                 fpj_dead_raw[1] >= 0 ? (unsigned)fpj_dead_raw[1] : 0u);
        fp_draw_text(2, 4 * FP_FONT_H, b, 0xF800);
    }
    /* NO fp_lcd_flush() here: the overlay now renders only on REAL
     * flushes (fp_lcd_flush on target / the host stub) — see
     * fp_platform.c. It used to submit its own LCD refresh from
     * inside fpj_io_pump() DURING SD reads: a mid-frame refresh of a
     * half-composed game frame + a second DMA transfer per tick.  */
}

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */
static u32  slen(JObj *o)      { return ARR_LEN(o); }
static u16 *schars(JObj *o)    { return (u16 *)ARR_DATA(o); }

static JRef mkstr(const char *s)               /* fresh (non-interned) */
{
    /* route through the UTF-8 decoder: manifest values / internal text
     * can be non-ASCII; ASCII input decodes 1:1 (unchanged behavior)  */
    return str_from_utf8(s, (u32)strlen(s));
}

/* Java String -> C buffer (Latin-1 truncation, NUL-terminated) */
static int str_to_c(JRef r, char *buf, int bufsz)
{
    JObj *o = obj(r);
    if (!o || o->type != T_ARR_C) { buf[0] = 0; return -1; }
    u32 n = slen(o);
    if (n > (u32)bufsz - 1) n = (u32)bufsz - 1;
    for (u32 i = 0; i < n; i++) buf[i] = (char)(u8)schars(o)[i];
    buf[n] = 0;
    return (int)n;
}

static JRef native_new(const char *cls, u32 slots)   /* stub instance */
{
    JClass *c = class_load(cls);
    if (!c) return 0;
    u32 words = c->instance_words > slots ? c->instance_words : slots;
    return ref_alloc(T_OBJECT, words * 4, c);
}

#define SLOT(o, i) (OBJ_PAYLOAD(o)[i])
#define THISO(a)   obj((a)[0].ref)

/* Native pointers stored in object payloads occupy TWO words so the
 * 64-bit host build survives; on 32-bit ARM the high word is 0 and
 * this costs nothing. Sites: Image/Graphics/Player wrappers.        */
#define SET_PTR(o, i, p) do { uintptr_t _p = (uintptr_t)(p); \
        SLOT(o, i) = (u32)_p; SLOT(o, i + 1) = (u32)(_p >> 32); } while (0)
#define GET_PTR(o, i) ((void *)(uintptr_t)(SLOT(o, i) | \
        ((u64)(u32)SLOT(o, i + 1) << 32)))

/* args marshaling shorthand: arg i as int / ref */
static s32 AI(const Slot *a, int i) { return (s32)a[i].w; }
static JRef AR(const Slot *a, int i) { return a[i].ref; }

/* natives that cache a JRef in a C static across allocations MUST pin
 * it (GC root) — cur_display/cur_canvas/cur_runtime. Balanced by
 * natives_reset's gc_unpin(n_pins).                                   */
static int n_pins;
static void pin_ref(JRef r)
{
    if (!r) return;
    gc_pin(r);
    n_pins++;
}
static void unpin_ref(void)
{
    if (n_pins > 0) { gc_unpin(n_pins); n_pins = 0; }
}

/* ------------------------------------------------------------------ */
/* java.lang.Object                                                     */
/* ------------------------------------------------------------------ */
static void nat_Object_init(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

static void nat_Object_hashCode(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = a[0].w * 2654435761u; }

static void nat_Object_equals(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = (a[0].ref == a[1].ref) ? 1 : 0; }

static void nat_Object_toString(const Slot *a, int n, Slot *r)
{
    (void)n;
    static char buf[64];
    JObj *o = THISO(a);
    const char *cn = (o && o->clazz) ? ((JClass *)o->clazz)->name : "?";
    int i = 0;
    while (cn[i] && i < 40) { buf[i] = cn[i]; i++; }
    buf[i++] = '@';
    u32 h = a[0].w * 2654435761u;
    for (int k = 28; k >= 0; k -= 4)
        buf[i++] = "0123456789ABCDEF"[(h >> k) & 15];
    buf[i] = 0;
    r[0].ref = mkstr(buf);
}

/* ------------------------------------------------------------------ */
/* java.lang.Class mirrors + resource streams                           */
/*                                                                      */
/* The Gameloft idiom ""getClass().getResourceAsStream(name)" loads     */
/* every data file (fonts, levels, IGP config) straight out of the      */
/* jar. It used to fall into the no-op fallback: getClass() pushed      */
/* garbage (uninitialized native return), the stream came back null,    */
/* and the game state built from the missing data crashed the VM       */
/* (AC1: f.bx stayed -1 -> f.bw[-1] -> array-bounds panic).            */
/* ------------------------------------------------------------------ */
static JRef class_mirror(JClass *c)
{
    if (!c) return 0;
    if (!c->static_ref) {
        JClass *cc = class_load("java/lang/Class");
        if (!cc) return 0;
        JRef m = ref_alloc(T_OBJECT, 8, cc);
        if (!m) return 0;
        JObj *o = obj(m);
        SET_PTR(o, 0, c);                 /* mirrored JClass*          */
        c->static_ref = m;                /* GC root (heap.c marks it) */
    }
    return c->static_ref;
}

/* public alias: interp.c's `ldc class` pushes a real mirror through
 * this (a null Class literal became AC1's invokevirtual-on-null)    */
JRef fpj_class_mirror(JClass *c) { return class_mirror(c); }

/* allocate a Throwable-subclass instance with the message in slot 1
 * (nat_Throwable_getMessage reads it). Super chains are modeled in
 * stub_super_of so catch(Throwable/Exception/RuntimeException) match. */
JRef fpj_new_exception(const char *cls, const char *msg)
{
    JRef r = native_new(cls, 4);
    JObj *o = obj(r);
    if (!o) return 0;
    if (msg) {
        JRef m = mkstr(msg);
        if (m) SLOT(o, 1) = m;
    }
    return r;
}

/* Classes we model are enumerated right below the registry table
 * (lib_known + nat_Class_forName live there — they search it).      */

static void nat_Object_getClass(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    JObj *o = THISO(a);
    if (!o) return;
    if (o->type == T_OBJECT && o->clazz)
        r[0].ref = class_mirror((JClass *)o->clazz);
    else if (o->type == T_ARR_C)          /* String payload object     */
        r[0].ref = class_mirror(class_load("java/lang/String"));
    else                                   /* arrays: a plain mirror    */
        r[0].ref = class_mirror(class_load("java/lang/Object"));
}

static void nat_Class_getName(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    JObj *o = THISO(a);
    if (!o) return;
    JClass *c = (JClass *)GET_PTR(o, 0);
    if (!c || !c->name) return;
    char buf[96]; int i = 0;
    for (const char *p = c->name; *p && i < 95; p++)
        buf[i++] = (*p == '/') ? '.' : *p;
    buf[i] = 0;
    r[0].ref = mkstr(buf);
}

/* ---- open resource stream bookkeeping (close + suite shutdown) ---- */
#define MAX_STREAMS 12
static u8 *stream_bufs[MAX_STREAMS];      /* live entry buffers        */

static void stream_track(u8 *p)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!stream_bufs[i]) { stream_bufs[i] = p; return; }
}

static void stream_untrack(u8 *p)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (stream_bufs[i] == p) { stream_bufs[i] = 0; return; }
}

static void stream_free_all(void)          /* natives_reset path       */
{
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (stream_bufs[i]) { fp_free(stream_bufs[i]); stream_bufs[i] = 0; }
    }
}

static void nat_Class_getResourceAsStream(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;                          /* null = not found (Java)   */
    char path[96];
    str_to_c(AR(a, 1), path, sizeof path);
    const char *p = path[0] == '/' ? path + 1 : path;
    u32 sz = 0;
    u8 *data = 0;
    if (game_jar_ok()) {
        const JarEntry *e = jar_find(jar_game(), p);
        if (e) data = jar_read(jar_game(), e, &sz);
    }
    if (!data && boot_jar_ok()) {
        const JarEntry *e = jar_find(jar_boot(), p);
        if (e) data = jar_read(jar_boot(), e, &sz);
    }
    if (!data) { fp_dbg("res miss: "); fp_dbg(p); return; }
    JRef s = native_new("java/io/InputStream", 6);
    JObj *o = obj(s);
    if (!o) { fp_free(data); return; }
    SET_PTR(o, 0, data);                   /* slot 0/1: buffer         */
    SLOT(o, 2) = 0;                        /* slot 2: position         */
    SLOT(o, 3) = sz;                       /* slot 3: length           */
    stream_track(data);
    r[0].ref = s;
}

/* ---- javax.microedition.io "comm:usb" (Round 10: UsbCast) ------------
 * fpdoom's usbio pipe (see platform.h) exposed as a MIDP stream:
 *   Connector.open("comm:usb")              -> StreamConnection
 *   sc.openInputStream()  -> java/io/InputStream  (slot 4 == 1)
 *   sc.openOutputStream() -> java/io/OutputStream (slot 0 == 1)
 * The InputStream carries an abort flag in slot 5: close() sets it
 * and every blocked native read polls it, so the EXIT KEY always
 * breaks a waiting read (UsbCast returns to the launcher; no hard
 * lock, no battery pull). While waiting, reads pump the frame pump
 * every ~2 ms — keypad events and repaints keep flowing exactly
 * like the SD-read io pump (fpj_io_pump) that Round 8 verified on
 * hardware. USB semantics of the stream (documented deviation):
 * read() blocks until data, close(), or link-EOF; -1 is returned
 * ONLY for close()/EOF, never as an idle timeout, so the MIDlet's
 * readFully-style loops never mistake a slow host for a dead one. */
#define USB_IN_MODE(o)  (SLOT((o), 4) == 1)
#define USB_OUT_MODE(o) (SLOT((o), 0) == 1)

/* fill exactly len bytes; 1 = complete, 0 = aborted (close/EOF)
 *
 * Round 12 — TIGHT-LOOP BULK RX (the 0.2 fps fix). A 40,960-byte
 * frame is 641 64-byte bulk packets; the old loop slept 2 ms after
 * every partial drain, and fp_sleep_ms(2) -> sys_wait_ms(2) on the
 * SC6531E quantizes to a full hardware-timer tick (~10-16 ms), so
 * hundreds of refills of the 2048-byte usb_buf ring cost >5 s per
 * frame, starved EP2 (endless NAK) and tripped the host's 5 s ACK
 * timeout mid-transfer. The fill is now a three-phase state machine:
 *
 *   ACTIVE (first byte seen, rem > 0): spin fp_usb_read() in a
 *     tight loop with NO sleep. Each call IS "spin-poll
 *     usb_check_int() + usb_read(p, rem, 0)": fpdoom's usb_read()
 *     pumps the UDC interrupt handler (EP0 enumeration included)
 *     whenever the ring has >= 64 bytes free, then drains the ring
 *     to empty before returning - so the ring never overflows and
 *     the host never NAKs longer than one loop pass.
 *   Keypad/abort polling stays LIVE at ~16 ms cadence (fp_time_ms):
 *     midp_paint_pump() + a close()-flag recheck every 16 ms of
 *     streaming — the exit key still breaks any read, without
 *     per-packet pump overhead slowing the bulk transfer.
 *   IDLE (not one byte of this read yet): the ONLY place we still
 *     yield — pump events, then fp_sleep_ms(2). That is the
 *     sanctioned pre-first-byte wait, and it keeps Round 11's EP0
 *     enumeration service (fpj_usb_pump runs from fp_sleep_ms)
 *     alive while no MIDlet reader owns the ring.                    */
#define FPJ_USB_KEY_POLL_MS 16u

static int usb_fill(JObj *o, u8 *dst, u32 len)
{
    u32 got = 0;
    u32 last_poll = 0;                     /* 0 = poll on first gap */
    while (got < len) {
        if (SLOT(o, 5)) return 0;          /* close() abort        */
        int k = fp_usb_read(dst + got, len - got);
        if (k < 0) return 0;               /* link gone (host)     */
        got += (u32)k;
        if (got >= len) break;             /* complete             */

        if (got == 0) {
            /* IDLE: no first byte yet — keys + repaints + the EP0
             * enumeration service, then yield the wire wait       */
            midp_paint_pump();
            fp_sleep_ms(2);
            continue;
        }

        /* ACTIVE: tight spin, no sleep. Keypad/exit poll at ~16 ms
         * so a blocked bulk read still aborts on the exit key     */
        u32 now = fp_time_ms();
        if (last_poll == 0 || (u32)(now - last_poll) >= FPJ_USB_KEY_POLL_MS) {
            last_poll = now;
            midp_paint_pump();
            if (SLOT(o, 5)) return 0;      /* key press -> close() */
        }
    }
    return 1;
}

/* Connector.open(String): "comm:usb" (optionally "comm:usb;...") is
 * the live USB pipe; anything else throws the MIDP-mandated
 * ConnectionNotFoundException (a stub class — Round 9 machinery). */
static void nat_Connector_open(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    char url[64];
    if (!AR(a, 0)) {
        fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
        return;
    }
    str_to_c(AR(a, 0), url, sizeof url);
    if (!strncmp(url, "comm:usb", 8) && (url[8] == 0 || url[8] == ';')) {
        /* Round 11: lazy cold-init (idempotent): boot already brought
         * the controller up, but if this MIDlet runs under a harness
         * that skipped target_main's call, the first open still arms
         * the pipe instead of hanging on a dead controller. Failure
         * (mock-only on host) degrades to the MIDP exception.      */
        if (fp_usb_init() != 0) {
            fpj_throw(fpj_new_exception(
                "javax/microedition/io/ConnectionNotFoundException", 0));
            return;
        }
        JRef c = native_new("javax/microedition/io/StreamConnection", 2);
        JObj *o = obj(c);
        if (o) SLOT(o, 0) = 1;             /* live USB connection    */
        r[0].ref = c;
    } else {
        fpj_throw(fpj_new_exception(
            "javax/microedition/io/ConnectionNotFoundException", 0));
    }
}

static void nat_SC_openInputStream(const Slot *a, int n, Slot *r)
{
    (void)a; (void)n;
    JRef s = native_new("java/io/InputStream", 6);
    JObj *o = obj(s);
    if (o) SLOT(o, 4) = 1;                 /* usb mode (slot5 abort) */
    r[0].ref = s;
}

static void nat_SC_openOutputStream(const Slot *a, int n, Slot *r)
{
    (void)a; (void)n;
    JRef s = native_new("java/io/OutputStream", 2);
    JObj *o = obj(s);
    if (o) SLOT(o, 0) = 1;                 /* usb mode               */
    r[0].ref = s;
}

static void nat_SC_close(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }            /* pipe is shareable       */

/* Connector.openInputStream/openOutputStream convenience forms       */
static void nat_Connector_openInputStream(const Slot *a, int n, Slot *r)
{
    Slot c[1] = {{0}};
    nat_Connector_open(a, 1, c);
    if (fpj_pending_exc || !c[0].ref) { r[0].ref = 0; return; }
    nat_SC_openInputStream(c, 1, r);
}

static void nat_Connector_openOutputStream(const Slot *a, int n, Slot *r)
{
    Slot c[1] = {{0}};
    nat_Connector_open(a, 1, c);
    if (fpj_pending_exc || !c[0].ref) { r[0].ref = 0; return; }
    nat_SC_openOutputStream(c, 1, r);
}

static void nat_Connector_close(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ---- java.io.OutputStream: the comm:usb TX side -------------------- */
static void nat_OutputStream_write_I(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (!t) return;
    if (USB_OUT_MODE(t)) {
        u8 b = (u8)AI(a, 1);
        fp_usb_write(&b, 1);              /* UsbCast's 1-byte ACK     */
    }
    /* any other OutputStream sink: silent no-op (degrade policy)     */
}

static void nat_OutputStream_write_bulk(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a), *arr = obj(AR(a, 1));
    if (!t || !arr) {
        fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
        return;
    }
    s32 off, cnt;
    if (n >= 4) { off = AI(a, 2); cnt = AI(a, 3); }   /* write([BII)  */
    else        { off = 0;     cnt = (s32)ARR_LEN(arr); }
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(arr)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0));
        return;
    }
    if (USB_OUT_MODE(t) && cnt)
        fp_usb_write(ARR_DATA(arr) + off, (u32)cnt);
}

static void nat_OutputStream_flush(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }   /* usb writes are synchronous anyway */

static void nat_OutputStream_close(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ---- java.io.InputStream over the captured entry buffer ------------ */
static u8 *stream_buf_of(JObj *o)          { return (u8 *)GET_PTR(o, 0); }

static void nat_InputStream_read(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = -1;                           /* EOF / closed / null      */
    JObj *o = THISO(a);
    if (!o) return;
    if (USB_IN_MODE(o)) {                  /* comm:usb: block, pump,  */
        u8 b;                              /* abort on close()/EOF    */
        if (usb_fill(o, &b, 1)) r[0].w = b;
        return;
    }
    u8 *buf = stream_buf_of(o);
    u32 pos = SLOT(o, 2), len = SLOT(o, 3);
    if (!buf || pos >= len) return;
    r[0].w = buf[pos];
    SLOT(o, 2) = pos + 1;
}

static void nat_InputStream_read_bulk(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = -1;
    JObj *o = THISO(a);
    JObj *dst = obj(AR(a, 1));
    s32 off, cnt;
    if (n >= 4) { off = AI(a, 2); cnt = AI(a, 3); }   /* read([BII)I   */
    else        { off = 0;     cnt = dst ? (s32)ARR_LEN(dst) : 0; }
    if (!o || !dst) {      /* Java: null this / null buffer -> NPE     */
        fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
        return;
    }
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(dst)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0));
        return;
    }
    if (cnt == 0) { r[0].w = 0; return; }  /* len==0: 0 even at EOF   */
    if (USB_IN_MODE(o)) {
        /* comm:usb: fill the WHOLE request or report abort as -1;
         * partial data is never lost (the loop keeps what it got)   */
        if (usb_fill(o, ARR_DATA(dst) + off, (u32)cnt)) r[0].w = cnt;
        return;
    }
    u8 *buf = stream_buf_of(o);
    u32 pos = SLOT(o, 2), len = SLOT(o, 3);
    if (!buf || pos >= len) return;         /* EOF: -1                 */
    u32 avail = len - pos;
    /* RAM-backed stream: satisfy the WHOLE request or hit EOF — the
     * exact contract GLLib's readFully-style loops rely on          */
    u32 k = (u32)cnt < avail ? (u32)cnt : avail;
    if (k) {
        memcpy(ARR_DATA(dst) + off, buf + pos, k);
        SLOT(o, 2) = pos + k;
    }
    r[0].w = k;
}

static void nat_InputStream_available(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = 0;
    JObj *o = THISO(a);
    if (!o) return;
    if (USB_IN_MODE(o)) { r[0].w = (s32)fp_usb_available(); return; }
    u32 pos = SLOT(o, 2), len = SLOT(o, 3);
    r[0].w = (pos < len) ? (s32)(len - pos) : 0;
}

static void nat_InputStream_close(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *o = THISO(a);
    if (!o) return;
    if (USB_IN_MODE(o)) {
        SLOT(o, 5) = 1;                   /* abort any blocked read   */
        return;
    }
    u8 *buf = stream_buf_of(o);
    if (buf) { stream_untrack(buf); fp_free(buf); }
    SET_PTR(o, 0, 0);
    SLOT(o, 2) = SLOT(o, 3) = 0;
}

static void nat_InputStream_skip(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = 0; r[1].w = 0;
    JObj *o = THISO(a);
    if (!o) return;
    s64 want = (s64)((u64)a[1].w | (u64)a[2].w << 32);
    if (USB_IN_MODE(o)) {                 /* read & discard in chunks  */
        u8 tmp[64];
        s64 did = 0;
        while (did < want) {
            u32 k = (u64)(want - did) > sizeof tmp ? sizeof tmp
                                                 : (u32)(want - did);
            if (!usb_fill(o, tmp, k)) break;
            did += k;
        }
        r[0].w = (u32)(u64)did; r[1].w = (u32)((u64)did >> 32);
        return;
    }
    u32 pos = SLOT(o, 2), len = SLOT(o, 3);
    s64 avail = (pos < len) ? (s64)(len - pos) : 0;
    s64 k = want < avail ? want : avail;
    if (k > 0) SLOT(o, 2) = (u32)(pos + k);   /* exact offset advance */
    r[0].w = (u32)(u64)k; r[1].w = (u32)((u64)k >> 32);
}

/* ------------------------------------------------------------------
 * java.io.DataInputStream / ByteArrayInputStream / DataOutputStream /
 * ByteArrayOutputStream — the exact read/write pair GLLib uses to
 * parse resource files and build RMS savegames. DataInputStream
 * wraps ANY stream: slot 0 = inner stream JRef, slot 1 = kind
 * (1 = ptr-backed InputStream, 2 = arr-backed ByteArrayInputStream).
 * ------------------------------------------------------------------ */
static JObj *dis_inner(const JObj *t) { return obj(SLOT(t, 0)); }

/* one byte from the wrapped stream, polymorphic on its layout */
static s32 dis_getb(JObj *t)
{
    JObj *in = dis_inner(t);
    if (!in) return -1;
    if (SLOT(t, 1) == 2) {                   /* ByteArrayInputStream    */
        JObj *arr = obj(SLOT(in, 0));
        u32 pos = SLOT(in, 1), cnt = SLOT(in, 2);
        if (!arr || pos >= cnt) return -1;
        SLOT(in, 1) = pos + 1;
        return ARR_DATA(arr)[pos];
    }
    /* java/io/InputStream: slots 0/1 buffer ptr, 2 pos, 3 len        */
    u8 *buf = (u8 *)GET_PTR(in, 0);
    u32 pos = SLOT(in, 2), len = SLOT(in, 3);
    if (!buf || pos >= len) return -1;
    SLOT(in, 2) = pos + 1;
    return buf[pos];
}

static s32 dis_read_bulk(JObj *t, u8 *dst, s32 off, s32 cnt)
{
    if (cnt <= 0) return 0;
    s32 got = 0;
    while (got < cnt) {
        s32 b = dis_getb(t);
        if (b < 0) break;
        dst[off + got++] = (u8)b;
    }
    return got;
}

static void nat_DIS_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (!t) return;
    JObj *in = obj(AR(a, 1));
    JClass *c = (in && in->clazz) ? (JClass *)in->clazz : 0;
    SLOT(t, 0) = AR(a, 1);
    SLOT(t, 1) = (c && !strcmp(c->name, "java/io/ByteArrayInputStream")) ? 2 : 1;
}

static void nat_DIS_read(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? dis_getb(t) : -1; }

static void nat_DIS_read_bulk(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = -1;
    JObj *t = THISO(a), *dst = obj(AR(a, 1));
    if (!t || !dst) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    s32 off = (n >= 4) ? AI(a, 2) : 0;
    s32 cnt = (n >= 4) ? AI(a, 3) : (s32)ARR_LEN(dst);
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(dst)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0)); return;
    }
    r[0].w = dis_read_bulk(t, ARR_DATA(dst), off, cnt);
}

static void nat_DIS_readFully(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a), *dst = obj(AR(a, 1));
    if (!t || !dst) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    s32 off = (n >= 4) ? AI(a, 2) : 0;
    s32 cnt = (n >= 4) ? AI(a, 3) : (s32)ARR_LEN(dst);
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(dst)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0)); return;
    }
    s32 got = dis_read_bulk(t, ARR_DATA(dst), off, cnt);
    if (got < cnt)              /* Java: EOF before len -> EOFException */
        fpj_throw(fpj_new_exception("java/io/EOFException", 0));
}

static s32 dis_get2(JObj *t)   /* big-endian u16, -1 on EOF */
{
    s32 hi = dis_getb(t);
    s32 lo = dis_getb(t);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 8) | lo;
}

static void nat_DIS_readShort(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 v = t ? dis_get2(t) : -1;
  if (v < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
  r[0].w = (u32)(s32)(s16)v; }

static void nat_DIS_readUnsignedShort(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 v = t ? dis_get2(t) : -1;
  if (v < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
  r[0].w = (u32)v; }

static void nat_DIS_readInt(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    r[0].w = 0;
    s32 v = 0;
    for (int k = 0; k < 4; k++) {
        s32 b = t ? dis_getb(t) : -1;
        if (b < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
        v = (v << 8) | b;
    }
    r[0].w = (u32)v;
}

static void nat_DIS_readLong(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    r[0].w = r[1].w = 0;
    u64 v = 0;
    for (int k = 0; k < 8; k++) {
        s32 b = t ? dis_getb(t) : -1;
        if (b < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
        v = (v << 8) | (u32)b;
    }
    r[0].w = (u32)v; r[1].w = (u32)(v >> 32);
}

static void nat_DIS_readByte(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 b = t ? dis_getb(t) : -1;
  if (b < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
  r[0].w = (u32)(s32)(s8)b; }

static void nat_DIS_readUnsignedByte(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 b = t ? dis_getb(t) : -1;
  if (b < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
  r[0].w = (u32)b; }

static void nat_DIS_readBoolean(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 b = t ? dis_getb(t) : -1;
  if (b < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
  r[0].w = (b != 0); }

static void nat_DIS_readChar(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 v = t ? dis_get2(t) : -1;
  if (v < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
  r[0].w = (u32)v; }

static void nat_DIS_skipBytes(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  s32 want = AI(a, 1), got = 0;
  while (got < want && (t ? dis_getb(t) : -1) >= 0) got++;
  r[0].w = got; }

/* DataInputStream.skip(J)J: real Java DELEGATES to the wrapped stream;
 * the no-op fallback returning 0 made GLLib's skip-then-read resource
 * loops spin (the trace showed it as a logged missing native). Counts
 * the bytes actually consumed through the polymorphic inner stream.  */
static void nat_DIS_skip(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = 0; r[1].w = 0;
    JObj *t = THISO(a);
    if (!t) return;
    s64 want = (s64)((u64)a[1].w | (u64)a[2].w << 32);
    if (want <= 0) return;                  /* Java: negative -> 0     */
    s64 got = 0;
    while (got < want && dis_getb(t) >= 0) got++;
    r[0].w = (u32)(u64)got; r[1].w = (u32)((u64)got >> 32);
}

static void nat_DIS_available(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].w = 0;
    JObj *t = THISO(a);
    if (!t) return;
    JObj *in = dis_inner(t);
    if (!in) return;
    if (SLOT(t, 1) == 2) {
        u32 pos = SLOT(in, 1), cnt = SLOT(in, 2);
        r[0].w = (pos < cnt) ? (s32)(cnt - pos) : 0;
    } else {
        u32 pos = SLOT(in, 2), len = SLOT(in, 3);
        r[0].w = (pos < len) ? (s32)(len - pos) : 0;
    }
}

static void nat_DIS_close(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (!t) return;
    JObj *in = dis_inner(t);
    if (in && SLOT(t, 1) == 1) {   /* delegate: frees the entry buffer */
        Slot args[1] = { {0} };
        args[0].ref = SLOT(t, 0);
        Slot rr[2] = { {0}, {0} };
        nat_InputStream_close(args, 1, rr);
    }
    SLOT(t, 0) = 0;
}

static void nat_DIS_readUTF(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    JObj *t = THISO(a);
    s32 nlen = t ? dis_get2(t) : -1;
    if (nlen < 0) { fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
    /* modified UTF-8: 1..3 byte sequences, 0x00 as C0 80 */
    u32 cap = (u32)nlen < 4096 ? (u32)nlen : 4096;
    u16 *ch = (u16 *)fp_alloc((cap ? cap : 1) * 2);
    u32 nc = 0;
    for (u32 i = 0; i < (u32)nlen; i++) {
        s32 b1 = dis_getb(t);
        if (b1 < 0) { fp_free(ch); fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
        u16 c;
        if (b1 < 0x80) c = (u16)b1;
        else if ((b1 & 0xE0) == 0xC0) {
            s32 b2 = dis_getb(t); i++;
            if (b2 < 0) { fp_free(ch); fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
            c = (u16)(((b1 & 0x1F) << 6) | (b2 & 0x3F));
        } else {
            s32 b2 = dis_getb(t), b3 = dis_getb(t); i += 2;
            if (b2 < 0 || b3 < 0) { fp_free(ch); fpj_throw(fpj_new_exception("java/io/EOFException", 0)); return; }
            c = (u16)(((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F));
        }
        if (nc < cap) ch[nc++] = c;
    }
    JRef s = ref_alloc(T_ARR_C, 4 + 2 * nc, 0);
    JObj *so = obj(s);
    if (so) { ARR_LEN(so) = nc; memcpy(schars(so), ch, nc * 2); }
    fp_free(ch);
    r[0].ref = s;
}

/* ---- ByteArrayInputStream: slot0 = byte[] ref, slot1 = pos, slot2 = count */
static void nat_BAIS_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a), *arr = obj(AR(a, 1));
    if (!t) return;
    SLOT(t, 0) = AR(a, 1);
    if (arr && n >= 4) {                 /* (byte[], offset, length)   */
        s32 off = AI(a, 2), len = AI(a, 3);
        if (off < 0 || len < 0 || (u32)(off + len) > ARR_LEN(arr)) {
            fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0));
            SLOT(t, 0) = 0; SLOT(t, 1) = SLOT(t, 2) = 0;
            return;
        }
        SLOT(t, 1) = (u32)off;
        SLOT(t, 2) = (u32)off + (u32)len;
    } else {
        SLOT(t, 1) = 0;
        SLOT(t, 2) = arr ? ARR_LEN(arr) : 0;
    }
}

static void nat_BAIS_read(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = -1;
    JObj *t = THISO(a);
    if (!t) return;
    JObj *arr = obj(SLOT(t, 0));
    u32 pos = SLOT(t, 1), cnt = SLOT(t, 2);
    if (!arr || pos >= cnt) return;
    SLOT(t, 1) = pos + 1;
    r[0].w = ARR_DATA(arr)[pos];
}

static void nat_BAIS_read_bulk(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = -1;
    JObj *t = THISO(a), *dst = obj(AR(a, 1));
    if (!t || !dst) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    s32 off = (n >= 4) ? AI(a, 2) : 0;
    s32 cnt = (n >= 4) ? AI(a, 3) : (s32)ARR_LEN(dst);
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(dst)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0)); return;
    }
    if (cnt == 0) { r[0].w = 0; return; }
    JObj *arr = obj(SLOT(t, 0));
    u32 pos = SLOT(t, 1), end = SLOT(t, 2);
    if (!arr || pos >= end) return;
    u32 avail = end - pos;
    u32 k = (u32)cnt < avail ? (u32)cnt : avail;
    memcpy(ARR_DATA(dst) + off, ARR_DATA(arr) + pos, k);
    SLOT(t, 1) = pos + k;
    r[0].w = k;
}

static void nat_BAIS_available(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  if (!t) { r[0].w = 0; return; }
  u32 pos = SLOT(t, 1), end = SLOT(t, 2);
  r[0].w = (pos < end) ? (s32)(end - pos) : 0; }

static void nat_BAIS_skip(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].w = 0; r[1].w = 0;
    JObj *t = THISO(a);
    if (!t) return;
    s64 want = (s64)((u64)a[1].w | (u64)a[2].w << 32);
    u32 pos = SLOT(t, 1), end = SLOT(t, 2);
    s64 avail = (pos < end) ? (s64)(end - pos) : 0;
    s64 k = want < avail ? want : avail;
    if (k > 0) SLOT(t, 1) = (u32)(pos + k);
    r[0].w = (u32)(u64)k; r[1].w = (u32)((u64)k >> 32);
}

static void nat_BAIS_close(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ---- ByteArrayOutputStream: slot0 = byte[] ref, slot1 = count -------- */
static void baos_write(JObj *t, const u8 *src, u32 n)
{
    if (!t) return;
    JObj *arr = obj(SLOT(t, 0));
    u32 cnt = SLOT(t, 1);
    u32 cap = arr ? ARR_LEN(arr) : 0;
    if (cnt + n > cap) {
        u32 ncap = cap ? cap * 2 : 64;
        while (ncap < cnt + n) ncap *= 2;
        JRef nb = ref_alloc(T_ARR_B, 4 + ncap, 0);
        JObj *na = obj(nb);
        if (!na) return;
        ARR_LEN(na) = ncap;
        if (arr) memcpy(ARR_DATA(na), ARR_DATA(arr), cnt);
        SLOT(t, 0) = nb;
        arr = na;
    }
    if (arr) memcpy(ARR_DATA(arr) + cnt, src, n);
    SLOT(t, 1) = cnt + n;
}

static void nat_BAOS_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (!t) return;
    u32 cap = (n >= 2 && AI(a, 1) > 0) ? (u32)AI(a, 1) : 32;
    JRef b = ref_alloc(T_ARR_B, 4 + cap, 0);
    if (obj(b)) ARR_LEN(obj(b)) = cap;
    SLOT(t, 0) = b;
    SLOT(t, 1) = 0;
}

static void nat_BAOS_write_I(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (!t) return;
    u8 b = (u8)AI(a, 1);
    baos_write(t, &b, 1);
}

static void nat_BAOS_write_bulk(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a), *arr = obj(AR(a, 1));
    if (!t || !arr) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    s32 off = AI(a, 2), cnt = AI(a, 3);
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(arr)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0)); return;
    }
    baos_write(t, ARR_DATA(arr) + off, (u32)cnt);
}

static void nat_BAOS_toByteArray(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a);
    r[0].ref = 0;
    if (!t) return;
    JObj *arr = obj(SLOT(t, 0));
    u32 cnt = SLOT(t, 1);
    JRef nb = ref_alloc(T_ARR_B, 4 + cnt, 0);
    JObj *na = obj(nb);
    if (!na) return;
    ARR_LEN(na) = cnt;
    if (cnt) memcpy(ARR_DATA(na), ARR_DATA(arr), cnt);
    r[0].ref = nb;
}

static void nat_BAOS_size(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? SLOT(t, 1) : 0; }

static void nat_BAOS_reset(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a); if (t) SLOT(t, 1) = 0; }

static void nat_BAOS_flush(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ---- DataOutputStream: slot0 = inner stream JRef -------------------- */
static JObj *dos_inner(const Slot *a)
{
    JObj *t = THISO(a);
    return t ? obj(SLOT(t, 0)) : 0;
}

static void nat_DOS_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) SLOT(t, 0) = AR(a, 1); }

static void nat_DOS_write_I(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *in = dos_inner(a);
    if (!in) return;
    u8 b = (u8)AI(a, 1);
    baos_write(in, &b, 1);
}

static void nat_DOS_write_bulk(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *in = dos_inner(a), *arr = obj(AR(a, 1));
    if (!in || !arr) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    s32 off = AI(a, 2), cnt = AI(a, 3);
    if (off < 0 || cnt < 0 || (u32)(off + cnt) > ARR_LEN(arr)) {
        fpj_throw(fpj_new_exception("java/lang/IndexOutOfBoundsException", 0)); return;
    }
    baos_write(in, ARR_DATA(arr) + off, (u32)cnt);
}

static void nat_DOS_writeShort(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *in = dos_inner(a);
    if (!in) return;
    s32 v = AI(a, 1);
    u8 b[2] = { (u8)(v >> 8), (u8)v };
    baos_write(in, b, 2);
}

static void nat_DOS_writeInt(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *in = dos_inner(a);
    if (!in) return;
    s32 v = AI(a, 1);
    u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
    baos_write(in, b, 4);
}

static void nat_DOS_writeLong(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *in = dos_inner(a);
  if (!in) return;
  u64 v = (u64)a[1].w | (u64)a[2].w << 32;
  u8 b[8];
  for (int k = 7; k >= 0; k--) { b[k] = (u8)v; v >>= 8; }
  baos_write(in, b, 8); }

static void nat_DOS_writeUTF(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *in = dos_inner(a), *s = obj(AR(a, 1));
    if (!in) return;
    if (!s || s->type != T_ARR_C) return;
    u32 len = slen(s);
    if (len > 60000) len = 60000;        /* u2 payload limit           */
    /* count modified-UTF8 bytes first */
    u32 nb = 0;
    for (u32 i = 0; i < len; i++) {
        u16 c = schars(s)[i];
        if (c == 0 || c >= 0x800) nb += 3;
        else if (c >= 0x80) nb += 2;
        else nb += 1;
    }
    u8 *b = (u8 *)fp_alloc(nb ? nb : 1);
    if (!b) return;
    u32 k = 0;
    for (u32 i = 0; i < len; i++) {
        u16 c = schars(s)[i];
        if (c == 0) { b[k++] = 0xC0; b[k++] = 0x80; }
        else if (c < 0x80) b[k++] = (u8)c;
        else if (c < 0x800) {
            b[k++] = (u8)(0xC0 | (c >> 6)); b[k++] = (u8)(0x80 | (c & 0x3F));
        } else {
            b[k++] = (u8)(0xE0 | (c >> 12));
            b[k++] = (u8)(0x80 | ((c >> 6) & 0x3F));
            b[k++] = (u8)(0x80 | (c & 0x3F));
        }
    }
    u8 hdr[2] = { (u8)(nb >> 8), (u8)nb };
    baos_write(in, hdr, 2);
    baos_write(in, b, nb);
    fp_free(b);
}

static void nat_DOS_flush(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ------------------------------------------------------------------ */
/* java.lang.String (receiver is a T_ARR_C)                             */
/* ------------------------------------------------------------------ */
static void nat_String_length(const Slot *a, int n, Slot *r)
{ (void)n; JObj *o = THISO(a); r[0].w = o ? slen(o) : 0; }

static void nat_String_charAt(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *o = THISO(a);
    s32 i = AI(a, 1);
    if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    if ((u32)i >= slen(o)) { fpj_throw(fpj_new_exception("java/lang/StringIndexOutOfBoundsException", 0)); return; }
    r[0].w = schars(o)[i];
}

static int strequal(JRef x, JRef y)
{
    JObj *a = obj(x), *b = obj(y);
    if (!a || !b || a->type != T_ARR_C || b->type != T_ARR_C) return 0;
    if (slen(a) != slen(b)) return 0;
    return !memcmp(schars(a), schars(b), slen(a) * 2);
}

static void nat_String_equals(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = strequal(AR(a, 0), AR(a, 1)) ? 1 : 0; }

static int striequal(JRef x, JRef y)
{
    JObj *a = obj(x), *b = obj(y);
    if (!a || !b || a->type != T_ARR_C || b->type != T_ARR_C) return 0;
    if (slen(a) != slen(b)) return 0;
    for (u32 i = 0; i < slen(a); i++) {
        u16 ca = schars(a)[i], cb = schars(b)[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

static void nat_String_equalsIgnoreCase(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = striequal(AR(a, 0), AR(a, 1)) ? 1 : 0; }

static void nat_String_hashCode(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *o = THISO(a);
    u32 h = 0;
    if (o) for (u32 i = 0; i < slen(o); i++) h = h * 31u + schars(o)[i];
    r[0].w = h;
}

static void nat_String_toCharArray(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *o = THISO(a);
    if (!o) { r[0].ref = 0; return; }
    JRef rr = ref_alloc(T_ARR_C, o->size, 0);
    JObj *d = obj(rr);
    if (d) memcpy(OBJ_PAYLOAD(d), OBJ_PAYLOAD(o), o->size);
    r[0].ref = rr;
}

static void nat_String_valueOf_I(const Slot *a, int n, Slot *r)
{
    (void)n;
    char buf[16]; int p = 16, v = AI(a, 0), neg = v < 0;
    u32 u = neg ? (u32)(-v) : (u32)v;
    do { buf[--p] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) buf[--p] = '-';
    /* NO "buf[15] = 0" here: position 15 is ALWAYS the last digit
     * (the do/while writes it for every value incl. 0), so that line
     * clobbered the final character of every formatted int —
     * String.valueOf(5) came back "" and -200 printed as "-20".
     * out[] is NUL-terminated below, exactly like the (J) variant.  */
    char out[17]; int i = 0;
    while (p < 16) out[i++] = buf[p++];
    out[i] = 0;
    r[0].ref = mkstr(out);
}

static void nat_String_valueOf_J(const Slot *a, int n, Slot *r)
{
    (void)n;
    char buf[24];
    s64 v = (s64)((u64)a[0].w | (u64)a[1].w << 32);
    int neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v;
    int p = 24;
    do { buf[--p] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (neg) buf[--p] = '-';
    char out[25]; int i = 0;
    while (p < 24 && i < 24) out[i++] = buf[p++];
    out[i] = 0;
    r[0].ref = mkstr(out);
}

/* ---- java.lang.String constructors --------------------------------
 * 'new String(...)' from bytecode: the 'new' opcode allocates a
 * placeholder T_OBJECT for the java/lang/String stub class, but a
 * real String in this VM is a T_ARR_C char array (clazz 0). The
 * constructor builds the real array and handle_rebind() (heap.c)
 * moves the CALLER's reference onto it; the placeholder block turns
 * into ordinary garbage. The receiver is pinned across the internal
 * allocation — the invoke already popped it off the operand stack
 * into a C local the mark phase cannot see. AC2's GLLib builds
 * every text/IGP string through new String(bytes, off, len[, enc]):
 * before these, the ctor degraded to a no-op and left a hollow
 * T_OBJECT behind (length 0 / empty text everywhere).           */
static void string_ctor_bind(JRef self, JRef s)
{
    if (s) handle_rebind(self, s);
}

static void nat_String_init_empty(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    gc_pin(a[0].ref);                  /* survive an alloc-triggered GC  */
    JRef s = str_from_utf8("", 0);
    string_ctor_bind(a[0].ref, s);
    gc_unpin(1);
}

static void nat_String_init_copy(const Slot *a, int n, Slot *r)
{   /* String(String): real copy — sharing via rebind would retire the
     * source handle, and interned literals are shared by design      */
    (void)n; (void)r;
    JObj *src = obj(AR(a, 1));
    if (!src || src->type != T_ARR_C) { nat_String_init_empty(a, n, r); return; }
    gc_pin(a[0].ref);
    JRef s = ref_alloc(T_ARR_C, 4 + 2 * slen(src), 0);
    JObj *d = obj(s);
    if (d) { ARR_LEN(d) = slen(src); memcpy(schars(d), schars(src), slen(src) * 2); }
    string_ctor_bind(a[0].ref, s);
    gc_unpin(1);
}

static void nat_String_init_bytes(const Slot *a, int n, Slot *r)
{   /* String(byte[], off, len): bytes are UTF-8 (utf8_dec falls back
     * to Latin-1 per byte on malformed sequences) — the GLLib text
     * format. The 4-arg (charset) form arrives here too: MIDP games
     * pass "UTF-8" and behave identically, so the name is ignored.  */
    (void)n; (void)r;
    JObj *b = obj(AR(a, 1));
    s32 off = AI(a, 2), len = AI(a, 3);
    if (!b || b->type != T_ARR_B) { nat_String_init_empty(a, n, r); return; }
    if (off < 0 || len < 0 || (u32)off + (u32)len > ARR_LEN(b)) {
        fpj_throw(fpj_new_exception("java/lang/StringIndexOutOfBoundsException",
                                    "new String"));
        return;
    }
    gc_pin(a[0].ref);
    JRef s = str_from_utf8((const char *)ARR_DATA(b) + off, (u32)len);
    string_ctor_bind(a[0].ref, s);
    gc_unpin(1);
}

static void nat_String_init_chars(const Slot *a, int n, Slot *r)
{   /* String(char[], off, len) — same handle-rebind flow as the byte[]
     * ctor, but the source is already UTF-16 (WWG builds strings from
     * decoded char arrays). Bounds follow Java:
     * StringIndexOutOfBoundsException, not a silent empty string.     */
    (void)n; (void)r;
    JObj *b = obj(AR(a, 1));
    s32 off = AI(a, 2), len = AI(a, 3);
    if (!b || b->type != T_ARR_C) { nat_String_init_empty(a, n, r); return; }
    if (off < 0 || len < 0 || (u32)off + (u32)len > ARR_LEN(b)) {
        fpj_throw(fpj_new_exception("java/lang/StringIndexOutOfBoundsException",
                                    "new String"));
        return;
    }
    gc_pin(a[0].ref);
    JRef s = ref_alloc(T_ARR_C, 4 + 2 * (u32)len, 0);
    JObj *d = obj(s);
    if (d) {
        ARR_LEN(d) = (u32)len;
        memcpy(schars(d), ((u16 *)ARR_DATA(b)) + off, (size_t)len * 2);
    }
    string_ctor_bind(a[0].ref, s);
    gc_unpin(1);
}

static void nat_String_indexOf(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *h = THISO(a), *nd = obj(AR(a, 1));
    r[0].w = -1;
    if (!h || !nd) return;
    u32 hl = slen(h), nl = slen(nd);
    if (nl == 0) { r[0].w = 0; return; }
    for (u32 i = 0; i + nl <= hl; i++) {
        u32 j = 0;
        while (j < nl && schars(h)[i + j] == schars(nd)[j]) j++;
        if (j == nl) { r[0].w = i; return; }
    }
}

static void nat_String_substring(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *o = THISO(a);
    s32 b = AI(a, 1), e = AI(a, 2);
    if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    if (b < 0 || e < b || (u32)e > slen(o)) {
        fpj_throw(fpj_new_exception("java/lang/StringIndexOutOfBoundsException", 0));
        return;
    }
    u32 len = (u32)(e - b);
    JRef rr = ref_alloc(T_ARR_C, 4 + 2 * len, 0);
    JObj *d = obj(rr);
    if (d) { ARR_LEN(d) = len; memcpy(schars(d), schars(o) + b, len * 2); }
    r[0].ref = rr;
}

/* substring(I): to end — GLLib device-db keys use the 1-arg form */
static void nat_String_substring1(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *o = THISO(a);
    s32 b = AI(a, 1);
    if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    if (b < 0 || (u32)b > slen(o)) {
        fpj_throw(fpj_new_exception("java/lang/StringIndexOutOfBoundsException", 0));
        return;
    }
    u32 len = slen(o) - (u32)b;
    JRef rr = ref_alloc(T_ARR_C, 4 + 2 * len, 0);
    JObj *d = obj(rr);
    if (d) { ARR_LEN(d) = len; memcpy(schars(d), schars(o) + b, len * 2); }
    r[0].ref = rr;
}

/* trim(): drop chars <= ' ' at both ends (Java's exact contract) */
static void nat_String_trim(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].ref = 0;
    JObj *o = THISO(a);
    if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    u32 b = 0, e = slen(o);
    while (b < e && schars(o)[b] <= ' ') b++;
    while (e > b && schars(o)[e - 1] <= ' ') e--;
    u32 len = e - b;
    JRef rr = ref_alloc(T_ARR_C, 4 + 2 * len, 0);
    JObj *d = obj(rr);
    if (d) { ARR_LEN(d) = len; memcpy(schars(d), schars(o) + b, len * 2); }
    r[0].ref = rr;
}

/* toUpperCase()/toLowerCase(): ASCII fold, other code points untouched.
 * GLLib matches the (spoofed) platform/locale strings through these —
 * the old no-op fallback returned NULL and device probing broke.     */
static JRef str_case_fold(JObj *o, int up)
{
    if (!o) return 0;
    JRef rr = ref_alloc(T_ARR_C, o->size, 0);
    JObj *d = obj(rr);
    if (d) {
        memcpy(OBJ_PAYLOAD(d), OBJ_PAYLOAD(o), o->size);
        for (u32 i = 0; i < slen(d); i++) {
            u16 c = schars(d)[i];
            if (up && c >= 'a' && c <= 'z') schars(d)[i] = (u16)(c - 32);
            else if (!up && c >= 'A' && c <= 'Z') schars(d)[i] = (u16)(c + 32);
        }
    }
    return rr;
}
static void nat_String_toUpperCase(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = str_case_fold(THISO(a), 1); }
static void nat_String_toLowerCase(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = str_case_fold(THISO(a), 0); }

/* compareTo(String): char diff first, then length diff (JLS 20.10.16) */
static void nat_String_compareTo(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].w = 0;
    JObj *o = THISO(a), *p = obj(AR(a, 1));
    if (!o || !p) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    u32 ol = slen(o), pl = slen(p), m = ol < pl ? ol : pl;
    for (u32 i = 0; i < m; i++) {
        s32 d = (s32)schars(o)[i] - (s32)schars(p)[i];
        if (d) { r[0].w = (u32)d; return; }
    }
    r[0].w = (u32)(s32)(ol - pl);
}

static int str_starts_at(JObj *o, JObj *nd, u32 from)
{
    if (!o || !nd || from > slen(o)) return 0;
    u32 pl = slen(nd);
    if (pl > slen(o) - from) return 0;
    for (u32 i = 0; i < pl; i++)
        if (schars(o)[from + i] != schars(nd)[i]) return 0;
    return 1;
}
static void nat_String_startsWith(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = str_starts_at(THISO(a), obj(AR(a, 1)), 0) ? 1 : 0; }
static void nat_String_endsWith(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].w = 0;
    JObj *o = THISO(a), *nd = obj(AR(a, 1));
    if (!o || !nd) return;
    u32 pl = slen(nd);
    r[0].w = (pl <= slen(o) && str_starts_at(o, nd, slen(o) - pl)) ? 1 : 0;
}

/* indexOf(I): single-char search */
static void nat_String_indexOf_ch(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].w = -1;
    JObj *o = THISO(a);
    s32 ch = AI(a, 1);
    if (!o) return;
    for (u32 i = 0; i < slen(o); i++)
        if (schars(o)[i] == (u16)ch) { r[0].w = i; return; }
}

/* indexOf(String, fromIndex) */
static void nat_String_indexOf_from(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].w = -1;
    JObj *h = THISO(a), *nd = obj(AR(a, 1));
    s32 from = AI(a, 2);
    if (!h || !nd || from < 0) return;
    u32 hl = slen(h), nl = slen(nd);
    if ((u32)from > hl) return;
    for (u32 i = (u32)from; i + nl <= hl; i++) {
        u32 j = 0;
        while (j < nl && schars(h)[i + j] == schars(nd)[j]) j++;
        if (j == nl) { r[0].w = i; return; }
    }
}

/* getBytes(): chars truncated to bytes (GLLib packs text into RMS) */
static void nat_String_getBytes(const Slot *a, int n, Slot *r)
{
    (void)n; r[0].ref = 0;
    JObj *o = THISO(a);
    if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    u32 l = slen(o);
    JRef rr = ref_alloc(T_ARR_B, 4 + l, 0);
    JObj *d = obj(rr);
    if (d) {
        ARR_LEN(d) = l;
        for (u32 i = 0; i < l; i++) ARR_DATA(d)[i] = (u8)schars(o)[i];
    }
    r[0].ref = rr;
}

/* ------------------------------------------------------------------ */
/* java.lang.StringBuffer                                               */
/* ------------------------------------------------------------------ */
static void sb_init(const Slot *a, int cap, JRef init)
{
    JObj *t = THISO(a);
    if (!t) return;
    SLOT(t, 0) = 0; SLOT(t, 1) = 0;
    JRef b = ref_alloc(T_ARR_C, 4 + 2 * (u32)cap, 0);
    JObj *bo = obj(b);
    if (bo) { ARR_LEN(bo) = (u32)cap; SLOT(t, 0) = b; }
    if (init) {
        JObj *io = obj(init);
        if (io && io->type == T_ARR_C) {
            u32 n = slen(io);
            if (n > (u32)cap) n = (u32)cap;
            memcpy(schars(bo), schars(io), n * 2);
            SLOT(t, 1) = n;
        }
    }
}

static void nat_SB_init(const Slot *a, int n, Slot *r)
{ (void)r; (void)n; sb_init(a, 16, 0); }
static void nat_SB_init_I(const Slot *a, int n, Slot *r)
{ (void)r; (void)n; sb_init(a, AI(a, 1) > 0 ? AI(a, 1) : 16, 0); }
static void nat_SB_init_S(const Slot *a, int n, Slot *r)
{ (void)r; (void)n; sb_init(a, 16, AR(a, 1)); }

static void sb_append(JObj *t, const u16 *ch, u32 add)
{
    JObj *bo = obj(SLOT(t, 0));
    u32 cnt = SLOT(t, 1);
    if (!bo) return;
    if (cnt + add > slen(bo)) {               /* grow 2x                */
        u32 cap = slen(bo) * 2; if (cap < cnt + add) cap = cnt + add;
        JRef nb = ref_alloc(T_ARR_C, 4 + 2 * cap, 0);
        JObj *no = obj(nb);
        if (!no) return;
        ARR_LEN(no) = cap;
        memcpy(schars(no), schars(bo), cnt * 2);
        SLOT(t, 0) = nb;
        bo = no;
    }
    memcpy(schars(bo) + cnt, ch, add * 2);
    SLOT(t, 1) = cnt + add;
}

static void nat_SB_append_S(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a), *s = obj(AR(a, 1));
    if (t && s && s->type == T_ARR_C) sb_append(t, schars(s), slen(s));
    r[0].ref = a[0].ref;
}
static void nat_SB_append_C(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    u16 c = (u16)AI(a, 1);
    if (t) sb_append(t, &c, 1);
    r[0].ref = a[0].ref;
}
static void nat_SB_append_I(const Slot *a, int n, Slot *r)
{
    (void)n;
    Slot tmp[1] = { {0} };
    tmp[0].w = a[1].w;
    nat_String_valueOf_I(tmp, 1, r);
    JObj *t = THISO(a), *s = obj(r[0].ref);
    if (t && s) sb_append(t, schars(s), slen(s));
    r[0].ref = a[0].ref;
}
static void nat_SB_append_J(const Slot *a, int n, Slot *r)
{
    (void)n;
    Slot tmp[2] = { {0}, {0} };
    tmp[0].w = a[1].w; tmp[1].w = a[2].w;
    nat_String_valueOf_J(tmp, 2, r);
    JObj *t = THISO(a), *s = obj(r[0].ref);
    if (t && s) sb_append(t, schars(s), slen(s));
    r[0].ref = a[0].ref;
}

static void nat_SB_append_Z(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a);
    static const u16 ts[] = { 't','r','u','e' };
    static const u16 fs[] = { 'f','a','l','s','e' };
    if (t) sb_append(t, AI(a, 1) ? ts : fs, AI(a, 1) ? 4 : 5);
    r[0].ref = a[0].ref;
}

static void nat_SB_append_O(const Slot *a, int n, Slot *r)
{   /* append(Object): J2SE appends String.valueOf(obj) — "null" for a
     * null, the object's own toString() otherwise. Wrappers hit their
     * native toString; anything unmodeled falls back to the Object
     * "Cls@HEX" form so the buffer at least keeps a readable token.
     * The receiver and the source string stay pinned across
     * sb_append's buffer-growth allocation (the invoke already popped
     * both off the operand stack into C locals).                    */
    (void)n;
    JRef self = a[0].ref, arg = AR(a, 1);
    JObj *t = obj(self), *ao = obj(arg);
    if (t) {
        gc_pin(self);
        if (!ao) {
            static const u16 nul[] = { 'n','u','l','l' };
            sb_append(t, nul, 4);
        } else if (ao->type == T_ARR_C) {         /* a String          */
            gc_pin(arg);
            sb_append(t, schars(ao), slen(ao));
            gc_unpin(1);
        } else {
            const NativeEntry *tn = 0;
            if (ao->clazz)
                tn = native_lookup(((JClass *)ao->clazz)->name,
                                   "toString", "()Ljava/lang/String;");
            Slot nb[1], nr[2] = { {0}, {0} };
            nb[0].ref = arg;
            gc_pin(arg);                     /* fn may allocate        */
            if (tn) tn->fn(nb, 1, nr);
            else    nat_Object_toString(nb, 1, nr);
            gc_unpin(1);
            JRef sref = nr[0].ref;
            JObj *so = obj(sref);
            if (so && so->type == T_ARR_C) {
                gc_pin(sref);
                sb_append(t, schars(so), slen(so));
                gc_unpin(1);
            }
        }
        gc_unpin(1);
    }
    r[0].ref = self;
}
static void nat_SB_toString(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a);
    if (!t) { r[0].ref = 0; return; }
    u32 cnt = SLOT(t, 1);
    JRef rr = ref_alloc(T_ARR_C, 4 + 2 * cnt, 0);
    JObj *d = obj(rr);
    if (d) { ARR_LEN(d) = cnt;
             memcpy(schars(d), schars(obj(SLOT(t, 0))), cnt * 2); }
    r[0].ref = rr;
}
static void nat_SB_length(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? SLOT(t, 1) : 0; }
static void nat_SB_charAt(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    s32 i = AI(a, 1);
    if (!t || (u32)i >= SLOT(t, 1)) {
        fpj_throw(fpj_new_exception("java/lang/StringIndexOutOfBoundsException", 0));
        return;
    }
    r[0].w = schars(obj(SLOT(t, 0)))[i];
}
static void nat_SB_setLength(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) { u32 l = (u32)AI(a, 1); if (l <= SLOT(t, 1)) SLOT(t, 1) = l; } }

/* ------------------------------------------------------------------ */
/* java.lang.Math  —  the COMPLETE CLDC 1.1 set (Round 9)               */
/* ------------------------------------------------------------------ */
/* NO libm anywhere: fpdoom links -nostdlib + -lgcc, and sin/cos/tan/
 * sqrt/ceil/floor are NOT in libgcc, so everything below is pure C
 * (libgcc supplies only the __aeabi_d* soft-float arithmetic the
 * compiler already emits for every other double opcode — no Makefile
 * change, no -lm). NaN / ±0.0 semantics follow CLDC 1.1 (JLS):
 *   max/min : NaN wins; max(+0.0,-0.0)=+0.0, min(+0.0,-0.0)=-0.0
 *   abs     : |NaN|=NaN (sign bit cleared), |-0.0|=+0.0
 *   sin/cos : NaN and ±Inf -> NaN; sin(±0)=±0
 *   sqrt    : x<0 -> NaN, sqrt(±0)=±0, sqrt(Inf)=Inf
 *   ceil/floor : NaN->NaN, ±Inf->±Inf, integral/±0 unchanged
 * (WWG froze its boot diag on "m: java/lang/Math.max": the (JJ)J
 * overload was not registered and degraded to the zero-returning
 * no-op fallback — the exact hole this closes.)                     */
static float sf(const Slot *a, int i)
{ float f; u32 w = a[i].w; memcpy(&f, &w, 4); return f; }
static double sd(const Slot *a, int i)
{ double d; u64 u = (u64)a[i].w | (u64)a[i+1].w << 32;
  memcpy(&d, &u, 8); return d; }
static void putf(Slot *r, float f)
{ u32 w; memcpy(&w, &f, 4); r[0].w = w; }
static void putd(Slot *r, double d)
{ u64 u; memcpy(&u, &d, 8); r[0].w = (u32)u; r[1].w = (u32)(u >> 32); }

static double dnan(void)                  /* the canonical quiet NaN   */
{ u64 u = 0x7FF8000000000000ULL; double d; memcpy(&d, &u, 8); return d; }
static double d_inf(int neg)
{ u64 u = neg ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
  double d; memcpy(&d, &u, 8); return d; }

/* (J) helpers: the two-slot lo/hi marshaling used by the interpreter */
static s64 slot_j(const Slot *a, int i)
{ return (s64)((u64)a[i].w | (u64)a[i+1].w << 32); }
static void putj(Slot *r, s64 v)
{ r[0].w = (u32)(u64)v; r[1].w = (u32)((u64)v >> 32); }

/* IEEE bit helpers (no math.h signbit: target is -nostdlib)         */
static u32 fbits(float f)  { u32 w; memcpy(&w, &f, 4);  return w; }
static u64 dbits(double d) { u64 u; memcpy(&u, &d, 8);  return u; }

static float jmax_f(float x, float y)
{
    if (x != x) return x;                 /* NaN wins                  */
    if (y != y) return y;
    if (x == y && x == 0.0f)              /* ±0 pair: +0 wins for max  */
        return (fbits(x) & 0x80000000u) ? y : x;
    return (x > y) ? x : y;
}
static float jmin_f(float x, float y)
{
    if (x != x) return x;
    if (y != y) return y;
    if (x == y && x == 0.0f)              /* ±0 pair: -0 wins for min  */
        return (fbits(x) & 0x80000000u) ? x : y;
    return (x < y) ? x : y;
}
static double jmax_d(double x, double y)
{
    if (x != x) return x;
    if (y != y) return y;
    if (x == y && x == 0.0)
        return (dbits(x) >> 63) ? y : x;
    return (x > y) ? x : y;
}
static double jmin_d(double x, double y)
{
    if (x != x) return x;
    if (y != y) return y;
    if (x == y && x == 0.0)
        return (dbits(x) >> 63) ? x : y;
    return (x < y) ? x : y;
}

static void nat_Math_abs_I(const Slot *a, int n, Slot *r)
{ (void)n; s32 v = AI(a, 0);
  /* abs(INT_MIN) = INT_MIN (JLS): the widening negation wraps back   */
  r[0].w = (v < 0) ? (u32)(-(s64)v) : (u32)v; }
static void nat_Math_abs_J(const Slot *a, int n, Slot *r)
{ (void)n; s64 v = slot_j(a, 0);
  /* abs(LONG_MIN) = LONG_MIN (JLS): two's-complement wrap           */
  putj(r, (v < 0) ? -v : v); }
static void nat_Math_abs_F(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = a[0].w & 0x7FFFFFFFu; }          /* clear sign bit */
static void nat_Math_abs_D(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = a[0].w;
  r[1].w = a[1].w & 0x7FFFFFFFu; }                   /* sign in hi word */

static void nat_Math_max_I(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = AI(a, 0) > AI(a, 1) ? a[0].w : a[1].w; }
static void nat_Math_min_I(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = AI(a, 0) < AI(a, 1) ? a[0].w : a[1].w; }
static void nat_Math_max_J(const Slot *a, int n, Slot *r)
{ (void)n; putj(r, slot_j(a, 0) > slot_j(a, 2) ? slot_j(a, 0)
                                              : slot_j(a, 2)); }
static void nat_Math_min_J(const Slot *a, int n, Slot *r)
{ (void)n; putj(r, slot_j(a, 0) < slot_j(a, 2) ? slot_j(a, 0)
                                              : slot_j(a, 2)); }
static void nat_Math_max_F(const Slot *a, int n, Slot *r)
{ (void)n; putf(r, jmax_f(sf(a, 0), sf(a, 1))); }
static void nat_Math_min_F(const Slot *a, int n, Slot *r)
{ (void)n; putf(r, jmin_f(sf(a, 0), sf(a, 1))); }
static void nat_Math_max_D(const Slot *a, int n, Slot *r)
{ (void)n; putd(r, jmax_d(sd(a, 0), sd(a, 2))); }
static void nat_Math_min_D(const Slot *a, int n, Slot *r)
{ (void)n; putd(r, jmin_d(sd(a, 0), sd(a, 2))); }

/* ---- trigonometry: the kernels live in midp_graphics.c (gfx_sincos)
 * — ONE implementation shared with the arc rasterizer. Same rules:
 * NaN in -> NaN out, ±Inf -> NaN, sin(±0) = ±0.                     */
static void nat_Math_sin(const Slot *a, int n, Slot *r)
{
    (void)n;
    double x = sd(a, 0), s, c;
    if (x != x) { putd(r, x); return; }             /* NaN            */
    if (x - x != 0.0) { putd(r, dnan()); return; }  /* ±Inf -> NaN    */
    gfx_sincos(x, &s, &c);
    putd(r, s);
}
static void nat_Math_cos(const Slot *a, int n, Slot *r)
{
    (void)n;
    double x = sd(a, 0), s, c;
    if (x != x) { putd(r, x); return; }
    if (x - x != 0.0) { putd(r, dnan()); return; }
    gfx_sincos(x, &s, &c);
    putd(r, c);
}
static void nat_Math_tan(const Slot *a, int n, Slot *r)
{
    (void)n;
    double x = sd(a, 0), s, c;
    if (x != x) { putd(r, x); return; }
    if (x - x != 0.0) { putd(r, dnan()); return; }
    gfx_sincos(x, &s, &c);
    if (c == 0.0) { putd(r, d_inf(s < 0)); return; }  /* the pole     */
    putd(r, s / c);
}
static void nat_Math_sqrt(const Slot *a, int n, Slot *r)
{
    (void)n;
    double x = sd(a, 0);
    if (x != x) { putd(r, x); return; }             /* NaN in, NaN out*/
    if (x < 0)   { putd(r, dnan()); return; }       /* Java: NaN      */
    if (x == 0)  { putd(r, x); return; }            /* ±0 -> ±0 (JLS) */
    if (x - x != 0.0) { putd(r, x); return; }       /* +Inf -> +Inf   */
    double g = x > 1 ? x : 1;
    for (int i = 0; i < 40; i++) g = (g + x / g) * 0.5;
    /* AM-GM holds in REAL arithmetic only: rounding can land the
     * iterate a hair UNDER the root (sqrt(16) -> 3.9999999999999996)
     * and d2i would truncate to 3. Nudge one ulp while g*g < x.     */
    if (g > 0 && g * g < x) {
        u64 u = dbits(g) + 1;
        memcpy(&g, &u, 8);
    }
    putd(r, g);
}
static double jfloor(double x)
{
    if (x != x || x == 0.0 || x - x != 0.0) return x;  /* NaN/±0/±Inf */
    if (x < -9.0e18 || x > 9.0e18) return x;      /* |x| integral in dbl */
    s64 i = (s64)x;                               /* trunc toward zero */
    if (x < 0 && (double)i != x) i--;
    return (double)i;
}
static double jceil(double x)
{
    if (x != x || x == 0.0 || x - x != 0.0) return x;
    if (x < -9.0e18 || x > 9.0e18) return x;
    if (x > -1.0 && x < 0.0) {           /* JLS: ceil(-0.5) = -0.0     */
        u64 u = 0x8000000000000000ULL;
        double d; memcpy(&d, &u, 8); return d;
    }
    s64 i = (s64)x;
    if (x > 0 && (double)i != x) i++;
    return (double)i;
}
static void nat_Math_ceil(const Slot *a, int n, Slot *r)
{ (void)n; putd(r, jceil(sd(a, 0))); }
static void nat_Math_floor(const Slot *a, int n, Slot *r)
{ (void)n; putd(r, jfloor(sd(a, 0))); }
static void nat_Math_toRadians(const Slot *a, int n, Slot *r)
{ (void)n; putd(r, sd(a, 0) * (3.141592653589793 / 180.0)); }
static void nat_Math_toDegrees(const Slot *a, int n, Slot *r)
{ (void)n; putd(r, sd(a, 0) * (180.0 / 3.141592653589793)); }

/* ------------------------------------------------------------------ */
/* java.lang.System / Runtime / Thread / Throwable                      */
/* ------------------------------------------------------------------ */
static void nat_System_time(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; u64 t = fp_time_ms();
  r[0].w = (u32)t; r[1].w = (u32)(t >> 32); }

static void sleep_pump(u32 ms);                 /* below: blocking sleep  */

static void nat_System_sleep(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; sleep_pump(a[0].w & 0x7FFFFFFFu); }

static void nat_System_gc(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; gc_run(); }

static void nat_System_arraycopy(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *src = obj(AR(a, 0)), *dst = obj(AR(a, 2));
    s32 so = AI(a, 1), d_o = AI(a, 3), len = AI(a, 4);
    if (!src || !dst) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    if (so < 0 || d_o < 0 || len < 0 ||
        (u32)(so + len) > ARR_LEN(src) || (u32)(d_o + len) > ARR_LEN(dst)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    if (src->type != dst->type) { fpj_throw(fpj_new_exception("java/lang/ArrayStoreException", 0)); return; }
    u32 esz = src->type==T_ARR_B?1: src->type==T_ARR_C||src->type==T_ARR_S?2:
              src->type==T_ARR_J||src->type==T_ARR_D?8: 4;
    memmove(ARR_DATA(dst) + (u32)d_o * esz,
            ARR_DATA(src) + (u32)so * esz, (u32)len * esz);
}

static void nat_System_getProperty(const Slot *a, int n, Slot *r)
{
    (void)n;
    char key[64];
    str_to_c(AR(a, 0), key, sizeof key);
    const char *v = fpj_get_property(key);
    r[0].ref = v ? mkstr(v) : 0;
}

/* MIDP forbids System.exit (SecurityException on real handsets); the
 * pragmatic equivalent for a game bailing out is notifyDestroyed() —
 * return to the launcher menu instead of freezing the phone       */
static void nat_System_noop(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }   /* System.out println: silent (no USB log
                                   channel on target; host prints nothing
                                   either — stderr noise only)            */
static void nat_System_exit(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; fpj_midlet_done = 1; }

static void nat_Runtime_freeMemory(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; u64 f = heap_free_bytes();
  r[0].w = (u32)f; r[1].w = (u32)(f >> 32); }
static void nat_Runtime_totalMemory(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; u64 t = FPJ_HEAP_SIZE;
  r[0].w = (u32)t; r[1].w = (u32)(t >> 32); }
static void nat_Runtime_gc(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; gc_run(); }
static void nat_Runtime_exit(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; fpj_midlet_done = 1; }

/* Runtime.getRuntime(): pinned singleton — getDisplay pattern. It
 * MUST return non-null: `Runtime.getRuntime().freeMemory()` with a
 * null getRuntime was a direct "invokevirtual on null" panic.      */
static JRef cur_runtime;
static void nat_Runtime_get(const Slot *a, int n, Slot *r)
{
    (void)a; (void)n;
    if (!cur_runtime) {
        cur_runtime = native_new("java/lang/Runtime", 4);
        pin_ref(cur_runtime);
    }
    r[0].ref = cur_runtime;
}

/* ------------------------------------------------------------------ */
/* blocking natives that keep the VM alive                             */
/* ------------------------------------------------------------------ */
/* Gameloft (GLLib) midlets run their WHOLE frame loop inside
 * startApp(): repaint(); serviceRepaints(); ...; Thread.sleep(ms).
 * fpj2me_pump() is only reached after fpj2me_boot() returns, which
 * never happens while startApp blocks — so the blocking primitives
 * themselves must deliver repaints and drain input, or the game
 * deadlocks on its first frame with the keypad dead (the AC1 green
 * freeze).                                                       */
static void deliver_repaint(void);            /* below, near the pump  */

static void sleep_pump(u32 ms)
{
    while (ms) {
        u32 slice = ms > 10 ? 10 : ms;
        fp_sleep_ms(slice);
        ms -= slice;
        midp_frame_pump();          /* sys_event -> keyPressed etc.   */
        deliver_repaint();          /* pending repaint() requests     */
        /* diag overlay: no longer fired here — it renders on real
         * fp_lcd_flush() calls only (fp_platform.c), never mid-SD-read */
    }
}

static void nat_Thread_sleep(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; sleep_pump(a[0].w & 0x7FFFFFFFu); }
/* Thread.yield() is GLLib's frame-limiter wait: f.run() spins
 * "while (now - frameStart < 100) Thread.yield();" — a bare no-op
 * leaves paint/input delivery to the interpreter's 2^18-dispatch
 * hook, which on the 215k-ops/sec ARM9 fires only ~1.2x/SECOND
 * (the AC1 splash read as ~1 FPS on the phone with the VM healthy).
 * Pumping here — exactly like sleep_pump without the sleeping —
 * delivers one paint per yield pass: the frame limiter itself now
 * paces the 10 fps the game asks for. Re-entrancy is guarded
 * (in_pump / in_deliver) so a yield inside paint or inside the
 * pump is a clean no-op.                                          */
static void nat_Thread_yield(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; midp_paint_pump(); }

/* Thread.activeCount() — cooperative runtime: exactly one thread     */
static void nat_Thread_activeCount(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = 1; }

/* Thread.<init>(Runnable): remember the runnable in slot 0 — start()
 * needs it when the Thread base class itself was instantiated.     */
static void nat_Thread_init_R(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a); if (t) SLOT(t, 0) = AR(a, 1); }
/* Thread.<init>()V: plain no-op (the Runnable variant stores slot 0) */
static void nat_Thread_init(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* Thread.start() — COOPERATIVE, single-threaded: run the Runnable's
 * run() INLINE (to completion). Gameloft midlets put the whole game
 * loop on this thread: the loop's Thread.sleep()/serviceRepaints()
 * calls pump input + repaints from inside (sleep_pump), so the game
 * keeps rendering exactly like the blocking-startApp pattern. No
 * preemption exists, which matches the pre-RTOS fpdoom world.       */
static void nat_Thread_start(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (!t) return;
    JClass *tc = (JClass *)t->clazz;
    JMethod *run = tc ? class_find_method(tc, "run", "()V") : 0;
    JRef target = 0;
    if (run && run->native) run = 0;      /* Thread stub itself: use slot0 */
    if (run && !run->code) run = 0;
    if (!run) {                             /* new Thread(runnable)       */
        JObj *tgt = obj(SLOT(t, 0));
        JClass *rc = (tgt && tgt->clazz) ? (JClass *)tgt->clazz : 0;
        run = rc ? class_find_method(rc, "run", "()V") : 0;
        if (run && (!run->code || run->native)) run = 0;
        if (run) target = SLOT(t, 0);
    }
    if (!run) { fp_dbg("Thread.start: no run()"); return; }
    Slot args[1], rret[2];
    args[0].ref = target ? target : AR(a, 0);
    interp_call(run, args, 1, rret);
    if (fpj_pending_exc) {                  /* thread died: NAME it      */
        fpj_fatal("in thread run()");       /* red screen + exit, not a
                                               silent fp_dbg swallow    */
        fpj_pending_exc = 0;
    }
}

/* ---- java.lang.Integer (GL parses numbers out of resource text) ---- */
static int parse_int_radix(const char *s, int radix)
{
    int v = 0, any = 0, neg = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    for (; *s; s++) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d;
        any = 1;
    }
    if (!any) { fpj_throw(fpj_new_exception("java/lang/NumberFormatException", s)); return 0; }
    return neg ? -v : v;
}

/* Long.parseLong / Long.valueOf(String): same shape at s64 width     */
static s64 parse_long_radix(const char *s, int radix)
{
    s64 v = 0; int any = 0, neg = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    for (; *s; s++) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d;
        any = 1;
    }
    if (!any) { fpj_throw(fpj_new_exception("java/lang/NumberFormatException", s)); return 0; }
    return neg ? -v : v;
}

static void nat_Integer_parseInt(const Slot *a, int n, Slot *r)
{
    (void)n;
    char buf[32];
    str_to_c(AR(a, 0), buf, sizeof buf);
    r[0].w = (u32)parse_int_radix(buf, 10);
}

static void nat_Integer_parseInt_R(const Slot *a, int n, Slot *r)
{
    (void)n;
    char buf[32];
    str_to_c(AR(a, 0), buf, sizeof buf);
    s32 radix = AI(a, 1);
    if (radix < 2 || radix > 36) {
        fpj_throw(fpj_new_exception("java/lang/NumberFormatException", "radix"));
        return;
    }
    r[0].w = (u32)parse_int_radix(buf, radix);
}

static void nat_Integer_toString(const Slot *a, int n, Slot *r)
{ (void)n; nat_String_valueOf_I(a, 1, r); }

/* ---- java.lang wrapper boxing (CLDC 1.1 + Java 5 autoboxing) -------
 * AC2 froze exactly here: its settings menus box ints (Integer.valueOf)
 * and the unmodeled native degraded to a null return -> NPE -> freeze.
 * Payload layout shared by every wrapper: SLOT 0 = the value, stored
 * SIGN-EXTENDED to int range for the sub-int types (Byte/Short;
 * *Value() truncates on the way out); Long spreads its 64-bit value
 * over SLOT 0+1. Every instance carries >= 2 payload words (the
 * native_new floor) or >= 4 (the interpreter's 'new' opcode floor), so
 * all stores stay in bounds and a hand-rolled 'new Integer(3)' is
 * interchangeable with valueOf(3).
 *
 * A 256-entry Integer cache (-128..127) is DELIBERATELY skipped: the
 * GC pin stack is only 64 deep (heap.c) and would silently drop pins
 * past that. Boolean is the one wrapper whose J2SE semantics games
 * test with == (TRUE/FALSE singletons), and two pins are affordable.
 * Value identity elsewhere is covered by value-based equals().
 * ------------------------------------------------------------------ */
static JRef box_int_like(const char *cls, u32 v)
{
    JRef o = native_new(cls, 2);
    JObj *t = obj(o);
    if (t) SLOT(t, 0) = v;
    return o;
}

static JRef box_long2(u32 lo, u32 hi)
{
    JRef o = native_new("java/lang/Long", 2);
    JObj *t = obj(o);
    if (t) { SLOT(t, 0) = lo; SLOT(t, 1) = hi; }
    return o;
}

static JRef wrap_true, wrap_false;      /* Boolean singletons, pinned  */
static JRef box_boolean(int v)
{
    JRef *slot = v ? &wrap_true : &wrap_false;
    if (!*slot) {
        *slot = native_new("java/lang/Boolean", 2);
        if (!*slot) return 0;
        pin_ref(*slot);                 /* dropped by natives_reset()  */
        JObj *t = obj(*slot);
        if (t) SLOT(t, 0) = v ? 1 : 0;
    }
    return *slot;
}

/* valueOf() factories ------------------------------------------------ */
static void nat_Integer_valueOf(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = box_int_like("java/lang/Integer", a[0].w); }

static void nat_Integer_valueOf_S(const Slot *a, int n, Slot *r)
{
    (void)n; char buf[32]; str_to_c(AR(a, 0), buf, sizeof buf);
    r[0].ref = box_int_like("java/lang/Integer",
                            (u32)parse_int_radix(buf, 10));
}

/* AC2's sound-prompt call: Integer.valueOf(String, radix) — the diag
 * m: line carries no signature, so the radix overload is covered too */
static void nat_Integer_valueOf_SR(const Slot *a, int n, Slot *r)
{
    (void)n; char buf[32]; str_to_c(AR(a, 0), buf, sizeof buf);
    s32 radix = AI(a, 1);
    if (radix < 2 || radix > 36) {
        fpj_throw(fpj_new_exception("java/lang/NumberFormatException", "radix"));
        return;
    }
    r[0].ref = box_int_like("java/lang/Integer",
                            (u32)parse_int_radix(buf, radix));
}

/* parse_int_radix's inverse: Integer.toString(v, radix) / (this,radix) */
static void int_to_str_radix(char *out, int cap, s32 v, int radix)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[33]; int t = 0, neg = v < 0, i = 0;
    u32 u = neg ? 0u - (u32)v : (u32)v;   /* INT_MIN-safe */
    do { tmp[t++] = digits[u % (u32)radix]; u /= (u32)radix; } while (u);
    if (neg && i < cap - 1) out[i++] = '-';
    while (t && i < cap - 1) out[i++] = tmp[--t];
    out[i] = 0;
}

static void nat_Integer_toString_R(const Slot *a, int n, Slot *r)
{   /* static toString(int, radix): a[0]=v, a[1]=radix. (The INSTANCE
     * toString(radix) form is deliberately not modeled: its sig
     * "(I)Ljava/lang/String;" is identical to the static toString(int)
     * and the registry cannot tell them apart — CLDC omits it too.) */
    (void)n;
    s32 radix = AI(a, 1);
    if (radix < 2 || radix > 36) radix = 10;
    char out[36];
    int_to_str_radix(out, sizeof out, AI(a, 0), radix);
    r[0].ref = mkstr(out);
}

static void nat_Long_valueOf(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = box_long2(a[0].w, a[1].w); }

static void nat_Long_parseLong(const Slot *a, int n, Slot *r)
{
    (void)n; char buf[32]; str_to_c(AR(a, 0), buf, sizeof buf);
    s64 v = parse_long_radix(buf, 10);
    r[0].w = (u32)(u64)v; r[1].w = (u32)((u64)v >> 32);
}

static void nat_Long_valueOf_S(const Slot *a, int n, Slot *r)
{
    (void)n; char buf[32]; str_to_c(AR(a, 0), buf, sizeof buf);
    s64 v = parse_long_radix(buf, 10);  /* NFE pending on garbage      */
    r[0].ref = box_long2((u32)(u64)v, (u32)((u64)v >> 32));
}

static void nat_Byte_valueOf(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = box_int_like("java/lang/Byte",
                                   (u32)(s32)(s8)AI(a, 0)); }

static void nat_Byte_parseByte(const Slot *a, int n, Slot *r)
{   /* AC2 settings parsing: parseByte(String) = parseInt narrowed    */
    (void)n; char buf[32]; str_to_c(AR(a, 0), buf, sizeof buf);
    r[0].w = (u32)(s32)(s8)parse_int_radix(buf, 10);
}

static void nat_Short_valueOf(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = box_int_like("java/lang/Short",
                                   (u32)(s32)(s16)AI(a, 0)); }

static void nat_Character_valueOf(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = box_int_like("java/lang/Character", a[0].w & 0xFFFF); }

static void nat_Boolean_valueOf(const Slot *a, int n, Slot *r)
{ (void)n; r[0].ref = box_boolean(a[0].w ? 1 : 0); }

static void nat_Boolean_valueOf_S(const Slot *a, int n, Slot *r)
{   /* J2SE: "true" (case-insensitive) -> TRUE, anything else -> FALSE */
    (void)n; char buf[16]; str_to_c(AR(a, 0), buf, sizeof buf);
    r[0].ref = box_boolean(buf[0] == 't' || buf[0] == 'T');
}

/* constructors: 'new X(v)' writes the same payload as valueOf() ------ */
static void nat_Integer_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a); if (t) SLOT(t, 0) = a[1].w; }

static void nat_Long_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) { SLOT(t, 0) = a[1].w; SLOT(t, 1) = a[2].w; } }

static void nat_Byte_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) SLOT(t, 0) = (u32)(s32)(s8)a[1].w; }

static void nat_Short_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) SLOT(t, 0) = (u32)(s32)(s16)a[1].w; }

static void nat_Character_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) SLOT(t, 0) = a[1].w & 0xFFFF; }

static void nat_Boolean_init(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) SLOT(t, 0) = a[1].w ? 1 : 0; }

/* *Value() accessors -------------------------------------------------- */
static void nat_Integer_intValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? SLOT(t, 0) : 0; }

/* WWG narrows an Integer back to byte (its text-decode path);
 * Java semantics: (byte) of the int value, sign-extended            */
static void nat_Integer_byteValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = t ? (u32)(s32)(s8)(SLOT(t, 0) & 0xFF) : 0; }

static void nat_Long_longValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = t ? SLOT(t, 0) : 0; r[1].w = t ? SLOT(t, 1) : 0; }

static void nat_Byte_byteValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = t ? (u32)(s32)(s8)(SLOT(t, 0) & 0xFF) : 0; }

static void nat_Short_shortValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = t ? (u32)(s32)(s16)(SLOT(t, 0) & 0xFFFF) : 0; }

static void nat_Character_charValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? (SLOT(t, 0) & 0xFFFF) : 0; }

static void nat_Boolean_booleanValue(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = (t && SLOT(t, 0)) ? 1 : 0; }

/* toString(): instance form for every wrapper ------------------------ */
static void nat_w1_toString(const Slot *a, int n, Slot *r)
{   /* Integer / Byte / Short: SLOT 0 already holds sign-extended int  */
    (void)n; JObj *t = THISO(a);
    Slot tmp[1]; tmp[0].w = t ? SLOT(t, 0) : 0;
    nat_String_valueOf_I(tmp, 1, r);
}

static void nat_Long_toString_inst(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    Slot tmp[2];
    tmp[0].w = t ? SLOT(t, 0) : 0; tmp[1].w = t ? SLOT(t, 1) : 0;
    nat_String_valueOf_J(tmp, 1, r);
}

static void nat_Long_toString_J(const Slot *a, int n, Slot *r)
{ (void)n; nat_String_valueOf_J(a, 1, r); }      /* static toString(J) */

static void nat_Boolean_toString(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].ref = mkstr((t && SLOT(t, 0)) ? "true" : "false"); }

static JRef char_to_string(u32 c)
{   /* single UTF-16 char -> String (ASCII 1 byte, else 2-3 UTF-8)     */
    char b[4]; int k = 0; c &= 0xFFFF;
    if (c < 0x80) b[k++] = (char)c;
    else if (c < 0x800) {
        b[k++] = (char)(0xC0 | (c >> 6));
        b[k++] = (char)(0x80 | (c & 0x3F));
    } else {
        b[k++] = (char)(0xE0 | (c >> 12));
        b[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[k++] = (char)(0x80 | (c & 0x3F));
    }
    return str_from_utf8(b, (u32)k);
}

static void nat_Character_toString(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].ref = char_to_string(t ? SLOT(t, 0) : 0); }

/* equals()/hashCode(): value-based, per J2SE wrapper semantics ------- */
static int wrapper_payload_eq(JObj *x, JObj *y, int cells)
{
    if (!x || !y || x->clazz != y->clazz) return 0;
    for (int i = 0; i < cells; i++)
        if (SLOT(x, i) != SLOT(y, i)) return 0;
    return 1;
}

static void nat_w1_equals(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = wrapper_payload_eq(THISO(a), obj(AR(a, 1)), 1) ? 1 : 0; }

static void nat_w2_equals(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = wrapper_payload_eq(THISO(a), obj(AR(a, 1)), 2) ? 1 : 0; }

static void nat_w1_hashCode(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? SLOT(t, 0) : 0; }

static void nat_Long_hashCode(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);       /* (int)(v ^ (v >>> 32))          */
  r[0].w = t ? (SLOT(t, 0) ^ SLOT(t, 1)) : 0; }

static void nat_Boolean_hashCode(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = (t && SLOT(t, 0)) ? 1231 : 1237; }

static void nat_Throwable_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (t) { SLOT(t, 1) = (n > 1) ? AR(a, 1) : 0; SLOT(t, 2) = 0; }
}
static void nat_Throwable_getMessage(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].ref = t ? SLOT(t, 1) : 0; }
static void nat_Throwable_toString(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *o = THISO(a);
    const char *cn = (o && o->clazz) ? ((JClass *)o->clazz)->name
                                     : "java/lang/Throwable";
    char buf[96]; int i = 0;
    for (const char *p = cn; *p && i < 90; p++)
        buf[i++] = (*p == '/') ? '.' : *p;
    buf[i] = 0;
    r[0].ref = mkstr(buf);
}
static void nat_Throwable_printStackTrace(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; fp_dbg("(printStackTrace)"); }

/* ------------------------------------------------------------------ */
/* java.util.Random / Vector / Hashtable                                */
/* ------------------------------------------------------------------ */
static void nat_Random_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (!t) return;
    u64 s = (u64)fp_time_ms() * 6364136223846793005ULL + 1442695040888963407ULL;
    SLOT(t, 1) = (u32)s; SLOT(t, 2) = (u32)(s >> 32);
}
static void nat_Random_seed(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (!t) return;
    SLOT(t, 1) = a[1].w; SLOT(t, 2) = a[2].w;
}
static u32 rnd_next(JObj *t, int bits)
{
    u64 s = (u64)SLOT(t, 1) | (u64)SLOT(t, 2) << 32;
    s = (s * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
    SLOT(t, 1) = (u32)s; SLOT(t, 2) = (u32)(s >> 32);
    return (u32)(s >> (48 - bits));
}
static void nat_Random_nextInt(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? rnd_next(t, 32) : 0; }
/* nextInt(bound): bad bound throws IllegalArgumentException (Java), the
 * unbiased-rejection loop is Java's own algorithm                        */
static void nat_Random_nextInt_B(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    s32 b = AI(a, 1);
    if (b <= 0) {
        fpj_throw(fpj_new_exception("java/lang/IllegalArgumentException", "bound"));
        return;
    }
    r[0].w = 0;
    if (!t) return;
    if ((b & -b) == b) { r[0].w = (u32)((s64)b * (s64)rnd_next(t, 31) >> 31); return; }
    for (;;) {
        s32 v = (s32)rnd_next(t, 31);
        s32 m = v % b;
        if ((s32)(v - m + (b - 1)) >= 0) { r[0].w = (u32)m; return; }
    }
}

static void vec_ensure(JObj *t, u32 need)
{
    JObj *el = obj(SLOT(t, 0));
    if (el && need <= ARR_LEN(el)) return;
    u32 cap = el ? ARR_LEN(el) : 16;
    while (cap < need) cap *= 2;
    JRef nb = ref_alloc(T_ARR_A, 4 + 4 * cap, 0);
    JObj *no = obj(nb);
    if (!no) return;
    ARR_LEN(no) = cap;
    if (el) memcpy(ARR_DATA(no), ARR_DATA(el), (u32)SLOT(t, 3) * 4);
    SLOT(t, 0) = nb;
}
static void nat_Vector_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (!t) return;
    SLOT(t, 3) = 0;
    JRef el = ref_alloc(T_ARR_A, 4 + 4 * 16, 0);
    if (obj(el)) ARR_LEN(obj(el)) = 16;
    SLOT(t, 0) = el;
}
static void nat_Vector_addElement(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (!t) return;
    vec_ensure(t, (u32)SLOT(t, 3) + 1);
    ((JRef *)ARR_DATA(obj(SLOT(t, 0))))[SLOT(t, 3)] = AR(a, 1);
    SLOT(t, 3)++;
}
static void nat_Vector_size(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? SLOT(t, 3) : 0; }
static void nat_Vector_isEmpty(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = (t && SLOT(t, 3)) ? 0 : 1; }
static void nat_Vector_elementAt(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    s32 i = AI(a, 1);
    if (!t || (u32)i >= SLOT(t, 3)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    r[0].ref = ((JRef *)ARR_DATA(obj(SLOT(t, 0))))[i];
}
static void nat_Vector_setElementAt(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    s32 i = AI(a, 2);
    if (!t || (u32)i >= SLOT(t, 3)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    ((JRef *)ARR_DATA(obj(SLOT(t, 0))))[i] = AR(a, 1);
}
static void nat_Vector_insertElementAt(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    s32 i = AI(a, 2);
    if (!t || (u32)i > SLOT(t, 3)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    vec_ensure(t, (u32)SLOT(t, 3) + 1);
    JRef *el = (JRef *)ARR_DATA(obj(SLOT(t, 0)));
    for (u32 k = SLOT(t, 3); k > (u32)i; k--) el[k] = el[k - 1];
    el[i] = AR(a, 1);
    SLOT(t, 3)++;
}
static void nat_Vector_removeElementAt(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    s32 i = AI(a, 1);
    if (!t || (u32)i >= SLOT(t, 3)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    JRef *el = (JRef *)ARR_DATA(obj(SLOT(t, 0)));
    for (u32 k = (u32)i; k + 1 < SLOT(t, 3); k++) el[k] = el[k + 1];
    SLOT(t, 3)--;
}
static void nat_Vector_removeAllElements(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a); if (t) SLOT(t, 3) = 0; }
static void nat_Vector_firstElement(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    if (!t || !SLOT(t, 3)) {
        fpj_throw(fpj_new_exception("java/util/NoSuchElementException", 0));
        return;
    }
    r[0].ref = ((JRef *)ARR_DATA(obj(SLOT(t, 0))))[0];
}
static void nat_Vector_lastElement(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    if (!t || !SLOT(t, 3)) {
        fpj_throw(fpj_new_exception("java/util/NoSuchElementException", 0));
        return;
    }
    r[0].ref = ((JRef *)ARR_DATA(obj(SLOT(t, 0))))[SLOT(t, 3) - 1];
}
static void nat_Vector_indexOf(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    r[0].w = -1;
    if (!t) return;
    JRef *el = (JRef *)ARR_DATA(obj(SLOT(t, 0)));
    for (u32 i = 0; i < SLOT(t, 3); i++)
        if (el[i] == AR(a, 1) || strequal(el[i], AR(a, 1)))
            { r[0].w = i; return; }
}

static void nat_Hashtable_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (!t) return;
    SLOT(t, 3) = 0;
    JRef pairs = ref_alloc(T_ARR_A, 4 + 4 * 32, 0);
    if (obj(pairs)) ARR_LEN(obj(pairs)) = 32;
    SLOT(t, 0) = pairs;
}
static JRef *ht_find(JObj *t, JRef key)          /* -> [key, value]    */
{
    JObj *p = obj(SLOT(t, 0));
    if (!p) return 0;
    u32 n = ARR_LEN(p) / 2;
    JRef *kv = (JRef *)ARR_DATA(p);
    for (u32 i = 0; i < n; i++)
        if (kv[i * 2] == key || strequal(kv[i * 2], key))
            return &kv[i * 2];
    return 0;
}
static void nat_Hashtable_put(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    r[0].ref = 0;
    if (!t) return;
    JRef *slot = ht_find(t, AR(a, 1));
    if (slot) { r[0].ref = slot[1]; slot[1] = AR(a, 2); return; }
    JObj *p = obj(SLOT(t, 0));
    u32 cap = ARR_LEN(p) / 2, cnt = SLOT(t, 3);
    if (cnt >= cap) {                            /* grow                */
        JRef nb = ref_alloc(T_ARR_A, 4 + 4 * (cap * 2) * 2, 0);
        JObj *np = obj(nb);
        if (!np) return;
        ARR_LEN(np) = cap * 2 * 2;
        memcpy(ARR_DATA(np), ARR_DATA(p), cap * 2 * 4);
        SLOT(t, 0) = nb;
        p = np;
    }
    JRef *kv = (JRef *)ARR_DATA(p);
    kv[cnt * 2] = AR(a, 1); kv[cnt * 2 + 1] = AR(a, 2);
    SLOT(t, 3) = cnt + 1;
}
static void nat_Hashtable_get(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    r[0].ref = 0;
    if (!t) return;
    JRef *slot = ht_find(t, AR(a, 1));
    if (slot) r[0].ref = slot[1];
}
static void nat_Hashtable_size(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = t ? SLOT(t, 3) : 0; }
static void nat_Hashtable_isEmpty(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a); r[0].w = (t && SLOT(t, 3)) ? 0 : 1; }
static void nat_Hashtable_containsKey(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = (t && ht_find(t, AR(a, 1))) ? 1 : 0; }

/* Hashtable.remove(Object) — WWG clears settings-map entries: return
 * the old value, compact the pair array (order preserved), drop the
 * count. Missing key returns null (Java).                          */
static void nat_Hashtable_remove(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    r[0].ref = 0;
    if (!t) return;
    JObj *p = obj(SLOT(t, 0));
    if (!p || p->type != T_ARR_A) return;
    JRef *kv = (JRef *)ARR_DATA(p);
    u32 cnt = SLOT(t, 3);
    for (u32 i = 0; i < cnt; i++) {
        if (kv[i * 2] == AR(a, 1) || strequal(kv[i * 2], AR(a, 1))) {
            r[0].ref = kv[i * 2 + 1];
            memmove(&kv[i * 2], &kv[(i + 1) * 2],
                    (size_t)(cnt - i - 1) * 2 * sizeof(JRef));
            kv[(cnt - 1) * 2] = 0; kv[(cnt - 1) * 2 + 1] = 0;
            SLOT(t, 3) = cnt - 1;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* javax.microedition.lcdui — Image / Graphics / Canvas / GameCanvas    */
/* ------------------------------------------------------------------ */
static JImage screen_img_storage;
static JImage *screen_img;

static JImage *screen(void)                /* LCD back buffer as JImage */
{
    if (!screen_img) {
        screen_img_storage.w = FP_LCD_W;
        screen_img_storage.h = FP_LCD_H;
        screen_img_storage.px = fp_screen;
        screen_img_storage.immutable = 1;
        screen_img = &screen_img_storage;
    }
    return screen_img;
}

static JImage *img_ptr(JRef image_ref)
{
    JObj *o = obj(image_ref);
    return (o && o->type == T_OBJECT) ? (JImage *)GET_PTR(o, 0) : 0;
}

static JRef wrap_image(JImage *im)
{
    if (!im) return 0;
    JRef r = native_new("javax/microedition/lcdui/Image", 6);
    JObj *o = obj(r);
    if (!o) return 0;
    SET_PTR(o, 0, im);
    SLOT(o, 2) = im->w;
    SLOT(o, 3) = im->h;
    return r;
}

static void nat_Image_create_str(const Slot *a, int n, Slot *r)
{
    (void)n;
    char path[96];
    str_to_c(AR(a, 0), path, sizeof path);
    const char *p = path[0] == '/' ? path + 1 : path;
    r[0].ref = 0;
    u32 sz = 0;
    const JarEntry *e = game_jar_ok() ? jar_find(jar_game(), p) : 0;
    u8 *data = e ? jar_read(jar_game(), e, &sz) : 0;
    if (!data && boot_jar_ok()) {
        e = jar_find(jar_boot(), p);
        if (e) data = jar_read(jar_boot(), e, &sz);
    }
    if (!data) { fp_dbg("img miss: "); fp_dbg(p); return; }
    JImage *im = image_from_png(data, sz);
    fp_free(data);
    r[0].ref = wrap_image(im);
    if (!im) fp_dbg("png decode failed");
}

static void nat_Image_create_bytes(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *arr = obj(AR(a, 0));
    s32 off = AI(a, 1), len = AI(a, 2);
    r[0].ref = 0;
    if (!arr) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    if (off < 0 || len < 0 || (u32)(off + len) > ARR_LEN(arr)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    JImage *im = image_from_png(ARR_DATA(arr) + off, (u32)len);
    r[0].ref = wrap_image(im);
}

static void nat_Image_create_wh(const Slot *a, int n, Slot *r)
{
    (void)n;
    s32 w = AI(a, 0), h = AI(a, 1);
    if (w <= 0 || h <= 0) {
        /* Java: IllegalArgumentException ("width/height must be > 0")  */
        fpj_throw(fpj_new_exception("java/lang/IllegalArgumentException", 0));
        return;
    }
    r[0].ref = wrap_image(image_create((u16)w, (u16)h));
}

/* createRGBImage(int[] rgb, w, h, processAlpha): ARGB8888 -> RGB565 +
 * PX_TRANS (color-key 0x0000) in ONE pass, no intermediate buffer —
 * the Java int[] is dead the moment we return, so the GC reclaims it
 * immediately (4MB PSRAM discipline).                         */
static void nat_Image_createRGB(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    JObj *arr = obj(AR(a, 0));
    s32 w = AI(a, 1), h = AI(a, 2), alpha = AI(a, 3);
    if (!arr || arr->type != T_ARR_I) {
        fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
        return;
    }
    if (w <= 0 || h <= 0 || (u32)(w * h) > ARR_LEN(arr)) {
        fpj_throw(fpj_new_exception("java/lang/IllegalArgumentException", 0));
        return;
    }
    JImage *im = image_from_argb((const u32 *)ARR_DATA(arr), w, h, alpha);
    r[0].ref = wrap_image(im);
}

/* getRGB(rgbData, offset, scanlength, x, y, w, h): back to ARGB8888 */
static void nat_Image_getRGB(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a), *arr = obj(AR(a, 1));
    JImage *im = img_ptr(AR(a, 0));
    s32 off = AI(a, 2), scan = AI(a, 3);
    s32 x = AI(a, 4), y = AI(a, 5), w = AI(a, 6), h = AI(a, 7);
    if (!t || !arr) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); return; }
    if (!im) return;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > im->w || y + h > im->h ||
        off < 0 || scan < w ||
        (u32)(off + scan * (h - 1) + w) > ARR_LEN(arr)) {
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    u32 *dst = (u32 *)ARR_DATA(arr) + off;
    for (int row = 0; row < h; row++, dst += scan) {
        const u16 *src = im->px + (size_t)(y + row) * im->w + x;
        for (int c = 0; c < w; c++) {
            u16 px = src[c];
            dst[c] = (px == PX_TRANS) ? 0
                : 0xFF000000u
                | (((u32)((px >> 11) & 31) * 255 / 31) << 16)
                | (((u32)((px >> 5) & 63) * 255 / 63) << 8)
                |  ((u32)(px & 31) * 255 / 31);
        }
    }
}

static void nat_Image_create_region(const Slot *a, int n, Slot *r)
{
    (void)n;
    JImage *src = img_ptr(AR(a, 0));
    JImage *im = image_sub(src, (u16)AI(a, 1), (u16)AI(a, 2),
                           (u16)AI(a, 3), (u16)AI(a, 4), AI(a, 5));
    r[0].ref = wrap_image(im);
}

static void nat_Image_getWidth(const Slot *a, int n, Slot *r)
{ (void)n; JImage *im = img_ptr(AR(a, 0)); r[0].w = im ? im->w : 0; }
static void nat_Image_getHeight(const Slot *a, int n, Slot *r)
{ (void)n; JImage *im = img_ptr(AR(a, 0)); r[0].w = im ? im->h : 0; }

static void nat_Image_getGraphics(const Slot *a, int n, Slot *r);

static JRef wrap_graphics(JImage *target)
{
    JRef r = native_new("javax/microedition/lcdui/Graphics", 14);
    JObj *o = obj(r);
    if (!o) return 0;
    SET_PTR(o, 0, target);
    MGraphics g;
    g.target = target;
    gfx_reset(&g, target);
    SLOT(o, 2) = g.cx; SLOT(o, 3) = g.cy; SLOT(o, 4) = g.cw; SLOT(o, 5) = g.ch;
    SLOT(o, 6) = 0; SLOT(o, 7) = 0;
    SLOT(o, 8) = 0xFFFF;
    SLOT(o, 9) = 0x00FFFFFF;
    return r;
}

static void nat_Image_getGraphics(const Slot *a, int n, Slot *r)
{
    (void)n;
    JImage *im = img_ptr(AR(a, 0));
    if (!im) { r[0].ref = 0; return; }
    r[0].ref = wrap_graphics(im);
}

/* Graphics native helpers: rebuild an MGraphics from object slots     */
static int gfx_of(JRef gref, MGraphics *g, u16 *argb)
{
    JObj *o = obj(gref);
    if (!o || o->type != T_OBJECT) return 0;
    g->target = (JImage *)GET_PTR(o, 0);
    g->cx = (short)SLOT(o, 2); g->cy = (short)SLOT(o, 3);
    g->cw = (short)SLOT(o, 4); g->ch = (short)SLOT(o, 5);
    g->tx = (short)SLOT(o, 6); g->ty = (short)SLOT(o, 7);
    g->color = (u16)SLOT(o, 8);
    if (argb) *argb = (u16)SLOT(o, 9);
    return g->target != 0;
}
static void gfx_store(JRef gref, const MGraphics *g, u32 argb)
{
    JObj *o = obj(gref);
    if (!o) return;
    SLOT(o, 2) = (u32)(u16)g->cx; SLOT(o, 3) = (u32)(u16)g->cy;
    SLOT(o, 4) = (u32)(u16)g->cw; SLOT(o, 5) = (u32)(u16)g->ch;
    SLOT(o, 6) = (u32)(u16)g->tx; SLOT(o, 7) = (u32)(u16)g->ty;
    SLOT(o, 8) = g->color;
    SLOT(o, 9) = argb;
}

static void nat_Gfx_setColor_I(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 argb;
    if (!gfx_of(AR(a, 0), &g, &argb)) return;
    u32 c = (u32)AI(a, 1);
    gfx_color_int(&g, c);
    gfx_store(AR(a, 0), &g, c & 0xFFFFFF);
}
static void nat_Gfx_setColor_III(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 argb;
    if (!gfx_of(AR(a, 0), &g, &argb)) return;
    gfx_color_rgb(&g, AI(a, 1), AI(a, 2), AI(a, 3));
    gfx_store(AR(a, 0), &g, ((u32)AI(a, 1) << 16) | ((u32)(AI(a, 2) & 255) << 8)
                                          | (u32)(AI(a, 3) & 255));
}
static void nat_Gfx_getColor(const Slot *a, int n, Slot *r)
{
    (void)n; MGraphics g; u16 argb;
    if (gfx_of(AR(a, 0), &g, &argb)) r[0].w = argb; else r[0].w = 0;
}
static void nat_Gfx_fillRect(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; MGraphics g; u16 c;
  if (gfx_of(AR(a, 0), &g, &c)) gfx_fill_rect(&g, AI(a,1), AI(a,2), AI(a,3), AI(a,4)); }
static void nat_Gfx_drawRect(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; MGraphics g; u16 c;
  if (gfx_of(AR(a, 0), &g, &c)) gfx_draw_rect(&g, AI(a,1), AI(a,2), AI(a,3), AI(a,4)); }
static void nat_Gfx_drawLine(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; MGraphics g; u16 c;
  if (gfx_of(AR(a, 0), &g, &c)) gfx_draw_line(&g, AI(a,1), AI(a,2), AI(a,3), AI(a,4)); }
static void nat_Gfx_fillTriangle(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    gfx_fill_triangle(&g, AI(a, 1), AI(a, 2), AI(a, 3), AI(a, 4),
                      AI(a, 5), AI(a, 6));
}
static void nat_Gfx_setClip(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    gfx_clip(&g, AI(a, 1), AI(a, 2), AI(a, 3), AI(a, 4));
    gfx_store(AR(a, 0), &g, c);
}
/* clipRect: INTERSECT with the current clip (JSR-118) — the inverse
 * of setClip's replace semantics; negative w/h just empties it     */
static void nat_Gfx_clipRect(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    gfx_clip_rect(&g, AI(a, 1), AI(a, 2), AI(a, 3), AI(a, 4));
    gfx_store(AR(a, 0), &g, c);
}
/* drawArc / fillArc (Round 9, WWG): negative width/height raises
 * IllegalArgumentException exactly like JSR-118 — the game's own
 * catch blocks expect it, and a silent no-op would hide bad state   */
static void nat_Gfx_drawArc(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    if (AI(a, 3) < 0 || AI(a, 4) < 0) {
        fpj_throw(fpj_new_exception("java/lang/IllegalArgumentException",
                                    "drawArc"));
        return;
    }
    gfx_arc(&g, AI(a, 1), AI(a, 2), AI(a, 3), AI(a, 4),
            AI(a, 5), AI(a, 6), 0);
}
static void nat_Gfx_fillArc(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    if (AI(a, 3) < 0 || AI(a, 4) < 0) {
        fpj_throw(fpj_new_exception("java/lang/IllegalArgumentException",
                                    "fillArc"));
        return;
    }
    gfx_arc(&g, AI(a, 1), AI(a, 2), AI(a, 3), AI(a, 4),
            AI(a, 5), AI(a, 6), 1);
}
static void nat_Gfx_drawRoundRect(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    gfx_draw_roundrect(&g, AI(a, 1), AI(a, 2), AI(a, 3), AI(a, 4),
                       AI(a, 5), AI(a, 6));
}
static void nat_Gfx_getClipX(const Slot *a, int n, Slot *r)
{ (void)n; MGraphics g; u16 c;
  r[0].w = gfx_of(AR(a, 0), &g, &c) ? (g.cx - g.tx) : 0; }
static void nat_Gfx_getClipY(const Slot *a, int n, Slot *r)
{ (void)n; MGraphics g; u16 c;
  r[0].w = gfx_of(AR(a, 0), &g, &c) ? (g.cy - g.ty) : 0; }
static void nat_Gfx_getClipWidth(const Slot *a, int n, Slot *r)
{ (void)n; MGraphics g; u16 c;
  r[0].w = gfx_of(AR(a, 0), &g, &c) ? g.cw : 0; }
static void nat_Gfx_getClipHeight(const Slot *a, int n, Slot *r)
{ (void)n; MGraphics g; u16 c;
  r[0].w = gfx_of(AR(a, 0), &g, &c) ? g.ch : 0; }
static void nat_Gfx_translate(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    gfx_translate(&g, AI(a, 1), AI(a, 2));
    gfx_store(AR(a, 0), &g, c);
}
static void nat_Gfx_getTranslateX(const Slot *a, int n, Slot *r)
{ (void)n; MGraphics g; u16 c;
  r[0].w = gfx_of(AR(a, 0), &g, &c) ? g.tx : 0; }
static void nat_Gfx_getTranslateY(const Slot *a, int n, Slot *r)
{ (void)n; MGraphics g; u16 c;
  r[0].w = gfx_of(AR(a, 0), &g, &c) ? g.ty : 0; }
static void nat_Gfx_drawImage(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    JImage *im = img_ptr(AR(a, 1));
    if (im) gfx_draw_image(&g, im, AI(a, 2), AI(a, 3), AI(a, 4));
}
static void nat_Gfx_drawRegion(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    JImage *im = img_ptr(AR(a, 1));
    if (im) gfx_draw_region(&g, im, AI(a, 2), AI(a, 3), AI(a, 4), AI(a, 5),
                            AI(a, 6), AI(a, 7), AI(a, 8), AI(a, 9));
}
static void nat_Gfx_drawString(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    char buf[128];
    str_to_c(AR(a, 1), buf, sizeof buf);
    gfx_draw_string(&g, buf, AI(a, 2), AI(a, 3), AI(a, 4));
}
static void nat_Gfx_drawRGB(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 c;
    if (!gfx_of(AR(a, 0), &g, &c)) return;
    JObj *arr = obj(AR(a, 1));
    if (!arr) return;
    gfx_draw_rgb(&g, (const u32 *)ARR_DATA(arr), AI(a, 2), AI(a, 3),
                 AI(a, 4), AI(a, 5), AI(a, 6), AI(a, 7), AI(a, 8));
}
static void nat_Gfx_setFont(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Gfx_getFont(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].ref = native_new("javax/microedition/lcdui/Font", 2); }

/* Canvas / GameCanvas -------------------------------------------------- */
static void nat_Canvas_getWidth(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = FP_LCD_W; }
static void nat_Canvas_getHeight(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = FP_LCD_H; }
/* Canvas.<init>: the game's Canvas subclass ctor chain hits this on
 * super() — must be a real registered native, or every construction
 * logs a bogus "M:" miss line into the diag overlay              */
static void nat_Canvas_init(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Canvas_setFullScreenMode(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Canvas_serviceRepaints(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; deliver_repaint(); }   /* MIDP: blocks
    until the pending repaint has been DELIVERED — a bare LCD flush
    starved every serviceRepaints-driven game loop (paint never ran) */
static void nat_Canvas_repaint(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; midp_paint_request(); }
static void nat_Canvas_getGameAction(const Slot *a, int n, Slot *r)
{ (void)n; r[0].w = input_get_game_action(AI(a, 1)); }
static void nat_GameCanvas_flush(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; fp_lcd_flush(); }
static void nat_GameCanvas_getGraphics(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].ref = wrap_graphics(screen()); }
static void nat_GameCanvas_getKeyStates(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = input_get_key_states(); }

/* ---- com.nokia.mid.ui DirectGraphics flip/rotate --------------------
 * Nokia UI API 1.1 manipulation constants (verified against the spec):
 *   FLIP_HORIZONTAL 0x2000, FLIP_VERTICAL 0x4000,
 *   ROTATE_90 90, ROTATE_180 180, ROTATE_270 270.
 * Mapped onto the same blitter that serves drawRegion (all 8 TRANS_*).
 * ------------------------------------------------------------------ */
static void nat_DirectUtils_getDG(const Slot *a, int n, Slot *r)
{
    (void)n;
    JRef dg = native_new("com/nokia/mid/ui/DirectGraphics", 4);
    JObj *t = obj(dg);
    if (t) SLOT(t, 0) = AR(a, 0);   /* the wrapped Graphics JRef      */
    r[0].ref = dg;
}

static void nat_DG_drawImage(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    MGraphics g; u16 c;
    JObj *t = THISO(a);
    if (!t) return;
    if (!gfx_of(SLOT(t, 0), &g, &c)) return;
    JImage *im = img_ptr(AR(a, 1));
    if (!im) return;
    s32 manip = AI(a, 5);
    int transform;
    switch (manip) {
    case 0x2000: transform = TRANS_MIRROR; break;       /* FLIP_HORIZONTAL */
    case 0x4000: transform = TRANS_MIRROR_ROT180; break; /* FLIP_VERTICAL   */
    case 90:     transform = TRANS_ROT90; break;
    case 180:    transform = TRANS_ROT180; break;
    case 270:    transform = TRANS_ROT270; break;
    default:     transform = TRANS_NONE; break;
    }
    gfx_draw_region(&g, im, 0, 0, im->w, im->h, transform,
                    AI(a, 2), AI(a, 3), AI(a, 4));
}

/* drawPixels(byte[], byte[], ...) — Nokia UI API 1.1 (Round 10).
 * Modeled combo = exactly what a USB-cast MIDlet needs:
 *   TYPE_USHORT_565_RGB (=565), identity manipulation (0),
 *   transparencyMask == null (fully opaque).
 * That hits the one-memcpy-per-row gfx_draw_rgb565 path: 40,960
 * bytes from a REUSED Java byte[] straight into the target's RGB565
 * buffer — zero per-pixel math in bytecode, zero allocation, zero
 * GC churn per frame. `offset` is a BYTE offset into pixels[] (the
 * cast frame's payload starts at buf[9] right behind the header).
 * Other formats / manipulations / non-null masks degrade to silent
 * no-ops (project policy: a stray call from another jar must never
 * abort boot).                                                    */
static void nat_DG_drawPixels(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (!t) return;
    MGraphics g; u16 c;
    if (!gfx_of(SLOT(t, 0), &g, &c)) return;   /* the wrapped Graphics */
    JObj *arr = obj(AR(a, 1));
    if (!arr || arr->type != T_ARR_B) return;   /* byte[] pixels        */
    if (AR(a, 2) != 0) return;                  /* transparencyMask     */
    s32 off = AI(a, 3), scan = AI(a, 4), x = AI(a, 5), y = AI(a, 6);
    s32 w = AI(a, 7), h = AI(a, 8);
    s32 manip = AI(a, 9), format = AI(a, 10);
    if (format != 565) return;                  /* TYPE_USHORT_565_RGB  */
    if (manip != 0) return;                     /* identity only        */
    if (w <= 0 || h <= 0 || scan < w) return;
    if (off < 0 || (u32)(off + 2 * ((s64)(h - 1) * scan + w)) > ARR_LEN(arr))
        return;
    gfx_draw_rgb565(&g, ARR_DATA(arr), off, scan, x, y, w, h);
}

/* Display --------------------------------------------------------------- */
static JRef cur_canvas;
static JRef cur_display;                   /* was fn-scope: reset needs it */
static int  repaint_req;

void midp_paint_request(void) { repaint_req = 1; }

/* deliver the pending repaint: find paint(Graphics) on the current
 * canvas and run it through the REAL interpreter, then flush. Used
 * three ways: the post-boot pump, serviceRepaints() (MIDP: block
 * until delivered) and the pumping Thread.sleep (games that only
 * call repaint()+sleep still see frames). in_deliver stops the
 * interpreter's own pump hook from re-entering paint while a paint
 * is already running (paint -> game code -> hook -> paint ...).   */
static int in_deliver;
static void deliver_repaint(void)
{
    if (in_deliver || !repaint_req || !cur_canvas) return;
    in_deliver = 1;
    JObj *o = obj(cur_canvas);
    if (o && o->clazz) {
        JMethod *paint = class_find_method((JClass *)o->clazz,
                                           "paint",
                                           "(Ljavax/microedition/lcdui/Graphics;)V");
        if (paint && screen()) {
            Slot args[2], rret[2];
            args[0].ref = cur_canvas;
            args[1].ref = wrap_graphics(screen());
            interp_call(paint, args, 2, rret);
            if (fpj_pending_exc) {         /* paint threw: NAME it      */
                fpj_fatal("in paint()");   /* was: invisible fp_dbg — a
                                              game whose paint() dies on
                                              the menu screen just froze
                                              with a dead keypad        */
            }
        }
    }
    repaint_req = 0;
    in_deliver = 0;
    fpj_paint_count++;                     /* vitals                    */
    fpj_last_paint_ms = fp_time_ms();
    fp_lcd_flush();
}

/* java.lang.System.out / .err: a REAL (silent) PrintStream object.
 * GLLib's font engine calls System.out.println on its decode-error
 * path; a null there turned a recoverable "image failed" into an
 * NPE that aborted the glyph-sheet build. Pinned across GCs, wiped
 * in natives_reset() with the other suite-wide pins.              */
static JRef fpj_stdout, fpj_stderr;
static JRef std_stream(JRef *slot)
{
    if (!*slot) {
        *slot = native_new("java/io/PrintStream", 2);
        if (*slot) gc_pin(*slot);
    }
    return *slot;
}
JRef fpj_std_stream(const char *name)
{ return std_stream(name && name[0] == 'e' ? &fpj_stderr : &fpj_stdout); }

/* fpj2me_shutdown(): drop everything that pins Java objects across a
 * reboot into the next jar. Order matters — heap_vm_reset() runs after
 * and wipes the objects these refs point to.                           */
void natives_reset(void)
{
    stream_free_all();      /* resource-stream entry buffers           */
    unpin_ref();            /* canvas + display + runtime pins         */
    wrap_true = 0;          /* Boolean singleton pins (re-box lazily)  */
    wrap_false = 0;
    fpj_stdout = 0;
    fpj_stderr = 0;
    cur_canvas = 0;
    cur_display = 0;
    cur_runtime = 0;
    repaint_req = 0;
    fpj_miss_native[0] = 0; /* vitals start at zero for the next jar  */
    fpj_paint_count = 0;
    fpj_last_paint_ms = 0;
}

/* ---- Display ---- (helpers defined below, near the bridge) --------- */
static void call_canvas_void(const char *mname, JRef canvas);

static void nat_Display_get(const Slot *a, int n, Slot *r)
{ (void)a; (void)n;
  if (!cur_display) {
      cur_display = native_new("javax/microedition/lcdui/Display", 2);
      pin_ref(cur_display);         /* was UNPINNED: a GC could sweep it,
                                       leaving getDisplay() returning a
                                       dead handle -> NPE downstream   */
  }
  r[0].ref = cur_display; }

static void nat_Display_setCurrent(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    if (cur_canvas) { gc_unpin(1); if (n_pins > 0) n_pins--; }
    cur_canvas = AR(a, 1);
    if (cur_canvas) {
        gc_pin(cur_canvas);
        n_pins++;
        repaint_req = 1;
        /* MIDP delivers showNotify() when a canvas becomes current;
         * Gameloft canvases start their machinery there. Re-entrancy
         * is safe: interp_call holds the outer frame chain for the GC. */
        call_canvas_void("showNotify", cur_canvas);
    }
}
static void nat_Display_getCurrent(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].ref = cur_canvas; }

/* Font ------------------------------------------------------------------ */
static void nat_Font_getDefault(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].ref = native_new("javax/microedition/lcdui/Font", 2); }
static void nat_Font_getFont(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].ref = native_new("javax/microedition/lcdui/Font", 2); }
static void nat_Font_stringWidth(const Slot *a, int n, Slot *r)
{ (void)n; char buf[128];
  str_to_c(AR(a, 1), buf, sizeof buf);
  r[0].w = (s32)strlen(buf) * gfx_char_width(); }
static void nat_Font_charWidth(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = gfx_char_width(); }
static void nat_Font_getHeight(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = gfx_string_height(); }
static void nat_Font_getBaselinePosition(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = gfx_string_height() - 1; }

/* ------------------------------------------------------------------ */
/* javax.microedition.rms                                               */
/* ------------------------------------------------------------------ */
static int last_rs = -1;

static void nat_RS_open(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    char name[40];
    str_to_c(AR(a, 0), name, sizeof name);
    s32 create = AI(a, 1);
    int h = rms_open(jar_suite(), name, create);
    if (h < 0 && !create) {
        /* openRecordStore(name, false) on a MISSING store: real Java
         * throws RecordStoreNotFoundException, and Gameloft save-load
         * code catches exactly that — the catchable throw replaces
         * the old silent empty-store fallback (games that then probe
         * getNumRecords() == 0 misread it as a corrupted save).     */
        fpj_throw(fpj_new_exception(
            "javax/microedition/rms/RecordStoreNotFoundException", name));
        return;
    }
    if (h < 0) {
        /* create requested but the card refused (no dir, write fail) */
        fpj_throw(fpj_new_exception(
            "javax/microedition/rms/RecordStoreException", name));
        return;
    }
    last_rs = h;
    JRef o = native_new("javax/microedition/rms/RecordStore", 4);
    JObj *t = obj(o);
    if (t) SLOT(t, 1) = (u32)h;
    r[0].ref = o;
}
static void nat_RS_close_static(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    /* close THIS store (per-instance handle), not the last-opened one:
     * games with several stores interleaved closed the wrong one.   */
    if (t) {
        rms_close((int)SLOT(t, 1));
        if (last_rs == (int)SLOT(t, 1)) last_rs = -1;
    }
    else if (last_rs >= 0) { rms_close(last_rs); last_rs = -1; }
}
static void nat_RS_addRecord(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a), *arr = obj(AR(a, 1));
    if (!t) { r[0].w = -1; return; }
    s32 off = AI(a, 2), len = AI(a, 3);
    if (!arr || off < 0 || len < 0 || (u32)(off + len) > ARR_LEN(arr))
        { r[0].w = -1; return; }
    r[0].w = rms_add((int)SLOT(t, 1), ARR_DATA(arr) + off, len);
}
static void nat_RS_getRecord(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a);
    if (!t) { r[0].ref = 0; return; }
    void *data; int len;
    if (rms_get((int)SLOT(t, 1), AI(a, 1), &data, &len) < 0)
        { r[0].ref = 0; return; }
    JRef arr = ref_alloc(T_ARR_B, 4 + (u32)len, 0);
    JObj *d = obj(arr);
    if (d) { ARR_LEN(d) = (u32)len; memcpy(ARR_DATA(d), data, (u32)len); }
    fp_free(data);
    r[0].ref = arr;
}
/* getRecord(id, buffer, offset): copy into the CALLER's array and
 * return the record's length (MIDP). AC1's savegame loader reads its
 * record with this overload into a preallocated buffer.            */
static void nat_RS_getRecord_into(const Slot *a, int n, Slot *r)
{
    (void)n;
    JObj *t = THISO(a), *arr = obj(AR(a, 2));
    if (!t) { r[0].w = -1; return; }
    s32 off = AI(a, 3);
    void *data; int len;
    if (rms_get((int)SLOT(t, 1), AI(a, 1), &data, &len) < 0) {
        fpj_throw(fpj_new_exception(
            "javax/microedition/rms/InvalidRecordIDException", 0));
        return;
    }
    if (!arr || off < 0 || (u32)(off + len) > ARR_LEN(arr)) {
        fp_free(data);
        fpj_throw(fpj_new_exception(
            "java/lang/ArrayIndexOutOfBoundsException", 0));
        return;
    }
    memcpy(ARR_DATA(arr) + off, data, (u32)len);
    fp_free(data);
    r[0].w = (u32)len;
}
static void nat_RS_setRecord(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r;
    JObj *t = THISO(a), *arr = obj(AR(a, 2));
    if (!t) return;
    s32 off = AI(a, 3), len = AI(a, 4);
    if (!arr || off < 0 || len < 0 || (u32)(off + len) > ARR_LEN(arr)) return;
    rms_set((int)SLOT(t, 1), AI(a, 1), ARR_DATA(arr) + off, len);
}
static void nat_RS_getNumRecords(const Slot *a, int n, Slot *r)
{
    (void)n; JObj *t = THISO(a);
    static int ids[512];
    r[0].w = t ? rms_enum((int)SLOT(t, 1), ids, 512) : 0;
}
static void nat_RS_deleteRecord(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) rms_delete((int)SLOT(t, 1), AI(a, 1)); }

/* ------------------------------------------------------------------ */
/* javax.microedition.media                                             */
/* ------------------------------------------------------------------ */
static void nat_Manager_playTone(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Manager_createPlayer(const Slot *a, int n, Slot *r)
{
    (void)n;
    JRef o = native_new("javax/microedition/media/Player", 6);
    JObj *t = obj(o);
    if (t) SET_PTR(t, 0, media_create_player("audio/midi"));
    r[0].ref = o;
}
/* createPlayer(InputStream, type) — WWG's music path: same silent
 * player; the (ignored) stream keeps Java's two-arg overload alive */
static void nat_Manager_createPlayer_IS(const Slot *a, int n, Slot *r)
{
    (void)n; (void)a;
    JRef o = native_new("javax/microedition/media/Player", 6);
    JObj *t = obj(o);
    if (t) SET_PTR(t, 0, media_create_player("audio/midi"));
    r[0].ref = o;
}
static void player_op(const Slot *a, int n, Slot *r, int op)
{
    (void)n; (void)r;
    JObj *t = THISO(a);
    if (t) media_player_op(GET_PTR(t, 0), op);
}
static void nat_Player_realize (const Slot *a, int n, Slot *r){ player_op(a,n,r,0); }
static void nat_Player_prefetch(const Slot *a, int n, Slot *r){ player_op(a,n,r,1); }
static void nat_Player_start   (const Slot *a, int n, Slot *r){ player_op(a,n,r,2); }
static void nat_Player_stop    (const Slot *a, int n, Slot *r){ player_op(a,n,r,3); }
static void nat_Player_close   (const Slot *a, int n, Slot *r){ player_op(a,n,r,4); }
static void nat_Player_deallocate(const Slot *a,int n, Slot *r){ player_op(a,n,r,4); }
static void nat_Player_setLoopCount(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a); if (t) SLOT(t, 1) = (u32)AI(a, 1); }
static void nat_Player_addPlayerListener(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a);
  if (t) media_add_listener((void *)(uintptr_t)SLOT(t, 0),
                            (void *)(uintptr_t)AR(a, 1)); }
static void nat_Player_getState(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = t ? media_player_state((void *)(uintptr_t)SLOT(t, 0)) : 0; }
static void nat_Player_getDuration(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = (u32)-1; r[1].w = (u32)-1; }  /* -1L: unknown */
static void nat_Player_getMediaTime(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = 0; r[1].w = 0; }

void bridge_player_event(int ev) { fp_dbg("(media event)"); (void)ev; }

/* ------------------------------------------------------------------ */
/* com.nokia.mid.sound.Sound (Nokia Sound API 1.0/1.1)                  */
/* ------------------------------------------------------------------ */
/* Series-40 era Gameloft sound engine. The API's state constants:
 *   SOUND_PLAYING = 0, SOUND_STOPPED = 1
 * THE SPLASH-STALL FIX: getState() previously fell through the
 * graceful-miss no-op, which returns 0 == SOUND_PLAYING. GLLib's
 * splash state machine then waited out its FULL audio timeout per
 * transition because the "sound" never reported STOPPED — on the
 * it2163 that read as ~2.1 s/frame (0.47 FPS) with the VM otherwise
 * healthy. Always reporting SOUND_STOPPED (1) tells the game the
 * (silent) sound finished immediately and the machine advances.
 * init([BI)V is the hot one: f.b(I)V re-arms the Sound object with
 * aF[i] before every play() — it was the last com/nokia/mid native
 * on the M: line (diag showed "Sound.init" mid-splash).
 * gain is parked in the receiver's slot 1 (native stub instances
 * always have >= 4 words); everything else is a clean no-op.       */
static void nat_Sound_getState(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; r[0].w = 1; }         /* SOUND_STOPPED, never 0    */
static void nat_Sound_noop(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Sound_setGain(const Slot *a, int n, Slot *r)
{ (void)n; (void)r; JObj *t = THISO(a); if (t) SLOT(t, 1) = AI(a, 1); }
static void nat_Sound_getGain(const Slot *a, int n, Slot *r)
{ (void)n; JObj *t = THISO(a);
  r[0].w = (t && SLOT(t, 1)) ? SLOT(t, 1) : 50; }  /* mid gain default */

/* com.nokia.mid.ui.DeviceControl: lights/vibra are phone-hardware
 * effects — on a silent runtime they are pure no-ops (static calls) */
static void nat_DeviceControl_noop(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ------------------------------------------------------------------ */
/* javax.microedition.midlet.MIDlet                                     */
/* ------------------------------------------------------------------ */
/* getAppProperty — the full MIDP 2.0 property chain:
 *   1. the sibling <jar_basename>.jad (loaded at boot into fpj_jad)
 *      overrides the jar, per MIDP-2.0 "Application Descriptor"
 *   2. META-INF/MANIFEST.MF inside the jar (fpj_manifest, filled by
 *      fpj2me_boot) — both cost zero SD I/O on the hot path
 *   3. Gameloft IGP/carrier fallbacks (see fpj_app_property below)
 * Both texts are RFC-822 style "Key: Value" headers: CRLF or LF line
 * endings, case-insensitive keys, and continuation lines (a physical
 * line starting with SP/TAB folds into the previous logical line). */
static int key_ieq(const char *at, const char *end, const char *key)
{
    size_t klen = strlen(key);
    if ((size_t)(end - at) != klen) return 0;
    for (size_t i = 0; i < klen; i++) {
        char a = at[i], b = key[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* match one logical header line [lstart,lend) whose FIRST physical line
 * ends at feol (the key must live before the ':' on that first line)  */
static int prop_match_line(const char *lstart, const char *feol,
                           const char *lend, const char *key,
                           char *out, int outsz)
{
    if (!lstart) return 0;
    const char *colon = lstart;
    while (colon < feol && *colon != ':') colon++;
    if (colon == feol) return 0;                 /* not a "Key: ..." line */
    if (!key_ieq(lstart, colon, key)) return 0;
    const char *v = colon + 1;
    while (v < lend && (*v == ' ' || *v == '\t')) v++;
    const char *ve = lend;
    while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' ||
                      ve[-1] == '\t')) ve--;
    int len = (int)(ve - v);
    if (len >= outsz) return 0;                  /* absurdly long value   */
    /* unfold: continuation folds leave CR/LF inside the value — copy
     * the bytes but drop the CR/LF separators themselves             */
    int o = 0;
    for (const char *q = v; q < ve; q++)
        if (*q != '\r' && *q != '\n') out[o++] = *q;
    out[o] = 0;
    return 1;
}

static int prop_lookup(const char *text, const char *key,
                       char *out, int outsz)
{
    if (!text || !*text) return 0;
    const char *lstart = 0, *feol = 0, *lend = 0;
    const char *p = text;
    for (;;) {
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        int last = (*eol == 0);
        int cont = lstart && p < eol && (*p == ' ' || *p == '\t');
        if (cont) {
            lend = eol;                          /* RFC-822 continuation  */
        } else {
            if (prop_match_line(lstart, feol, lend, key, out, outsz))
                return 1;
            lstart = p; feol = eol; lend = eol;
        }
        if (last)                                /* finalize the tail line */
            return prop_match_line(lstart, feol, lend, key, out, outsz);
        p = eol + 1;
    }
}

/* Gameloft IGP / carrier fallbacks. GLLib's IGP init unconditionally
 * calls String.trim()/.toUpperCase() on promo/carrier properties and
 * stalls in a catch-NPE loop when getAppProperty returns null — which
 * is exactly what happens with a standalone JAR that ships no JAD.
 * Return clean non-null defaults for those keys so GLLib skips the
 * promo path; genuinely unknown keys still return null (standard
 * MIDP semantics: the "unsupported feature" probe).                   */
const char *fpj_app_property(const char *key)
{
    static char buf[256];
    if (prop_lookup(fpj_jad, key, buf, sizeof buf)) return buf;
    if (prop_lookup(fpj_manifest, key, buf, sizeof buf)) return buf;

    /* fold the query key once for the exact/prefix fallback matching */
    char up[64];
    int i;
    for (i = 0; key[i] && i < 63; i++) {
        char c = key[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        up[i] = c;
    }
    up[i] = 0;

    if (!strcmp(up, "URL-TEMPLATE-GAME")) return "http://localhost/";
    if (!strcmp(up, "URL-OPERATOR"))      return "http://localhost/";
    if (!strcmp(up, "URL-PT"))            return "0";
    if (!strncmp(up, "IGP-", 4) || !strncmp(up, "URL-", 4)) return "";
    return 0;
}

static void nat_MIDlet_getAppProperty(const Slot *a, int n, Slot *r)
{
    (void)n;
    char key[64];
    str_to_c(AR(a, 1), key, sizeof key);
    const char *v = fpj_app_property(key);
    r[0].ref = v ? mkstr(v) : 0;   /* null for unknown keys: MIDP-correct */
}
/* notifyDestroyed: the game is done — tell the launcher main loop to
 * go back to the menu instead of hanging forever                      */
static void nat_MIDlet_notifyDestroyed(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; fpj_midlet_done = 1; }
static void nat_MIDlet_platformRequest(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
/* MIDlet.<init>: the game's MIDlet subclass ctor chains here on
 * super() before startApp — registered so it never hits the miss log */
static void nat_MIDlet_init(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ---- javax.microedition.lcdui.Command / Displayable (Round 9) --------
 * WWG builds a softkey Command and attaches it to the canvas. The
 * command itself is inert here (no softkey chrome is drawn), but the
 * ctor + attach calls must be REAL natives so they neither degrade
 * to the zero-returning fallback (which logged a bogus M: line and
 * returned a hollow object) nor abort on a missing class. Label /
 * type / priority are stored for future softkey rendering.           */
static void nat_Command_init(const Slot *a, int n, Slot *r)
{
    (void)n; (void)r; JObj *t = THISO(a);
    if (t) {
        SLOT(t, 1) = AR(a, 1);              /* label (String)          */
        SLOT(t, 2) = (u32)AI(a, 2);         /* commandType             */
        SLOT(t, 3) = (u32)AI(a, 3);         /* priority                */
    }
}
static void nat_Displayable_addCommand(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Displayable_removeCommand(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }
static void nat_Displayable_setCommandListener(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

/* ------------------------------------------------------------------ */
/* key-event bridge into the Java canvas                                */
/* ------------------------------------------------------------------ */
static void call_canvas_void_int(const char *mname, JRef canvas, int arg)
{
    JObj *o = obj(canvas);
    if (!o || !o->clazz) return;
    JMethod *m = class_find_method((JClass *)o->clazz, mname, "(I)V");
    if (!m) return;                          /* default no-op method   */
    Slot args[2], rret[2];
    args[0].ref = canvas;
    args[1].w = (u32)arg;
    interp_call(m, args, 2, rret);
    if (fpj_pending_exc) {                   /* key handler threw      */
        fpj_fatal("in key handler");         /* was: invisible swallow
                                               — the "keypad does nothing"
                                               freeze */
        fpj_pending_exc = 0;
    }
}

/* no-arg canvas callback (showNotify / hideNotify)                     */
static void call_canvas_void(const char *mname, JRef canvas)
{
    JObj *o = obj(canvas);
    if (!o || !o->clazz) return;
    JMethod *m = class_find_method((JClass *)o->clazz, mname, "()V");
    if (!m) return;                          /* default no-op method   */
    Slot args[1], rret[2];
    args[0].ref = canvas;
    interp_call(m, args, 1, rret);
    if (fpj_pending_exc) {
        fpj_fatal("in canvas callback");
        fpj_pending_exc = 0;
    }
}

void bridge_key_pressed(int midp)  { if (cur_canvas) call_canvas_void_int("keyPressed", cur_canvas, midp); }
void bridge_key_released(int midp) { if (cur_canvas) call_canvas_void_int("keyReleased", cur_canvas, midp); }
void bridge_key_repeated(int midp) { if (cur_canvas) call_canvas_void_int("keyRepeated", cur_canvas, midp); }
void bridge_command(int softkey)   { (void)softkey; }

/* ------------------------------------------------------------------ */
/* the frame pump                                                        */
/* ------------------------------------------------------------------ */
void fpj2me_pump(void)
{
    midp_frame_pump();                        /* input + media + events */
    deliver_repaint();
}

/* Interpreter-driven liveness: Gameloft's main loop often POLLS input
 * in a tight loop without ever sleeping (Thread.start() here runs it
 * inline) — the old pump paths (Thread.sleep / serviceRepaints) never
 * fire, so repaint() requests pile up undelivered and the screen
 * freezes. exec_run calls this every ~2^17 instructions, which puts
 * input polling + paint delivery on the same footing a preemptive
 * KVM thread would (independent of the game's loop style).        */
static int in_pump;
void midp_paint_pump(void)
{
    if (in_pump) return;                      /* re-entrancy guard      */
    in_pump = 1;
    midp_frame_pump();
    deliver_repaint();
    /* diag vitals strip: drawn on real LCD flushes only — see
     * fpj_diag_overlay() and fp_platform.c fp_lcd_flush()           */
    in_pump = 0;
}

/* ------------------------------------------------------------------ */
/* THE REGISTRY                                                          */
/* ------------------------------------------------------------------ */
#define N(cls, name, sig, fn) { cls, name, sig, fn }

/* forward decl: forName's strictness table lives at the bottom of this
 * file (it searches THIS registry), so declare it for the entries   */
static void nat_Class_forName(const Slot *a, int n, Slot *r);

static const NativeEntry natives[] = {
    /* java.lang.Object */
    N("java/lang/Object", "<init>", "()V", nat_Object_init),
    N("java/lang/Object", "hashCode", "()I", nat_Object_hashCode),
    N("java/lang/Object", "equals", "(Ljava/lang/Object;)Z", nat_Object_equals),
    N("java/lang/Object", "toString", "()Ljava/lang/String;", nat_Object_toString),
    N("java/lang/Object", "getClass", "()Ljava/lang/Class;", nat_Object_getClass),
    N("java/lang/Class", "getName", "()Ljava/lang/String;", nat_Class_getName),
    N("java/lang/Class", "getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;", nat_Class_getResourceAsStream),
    N("java/io/InputStream", "read", "()I", nat_InputStream_read),
    N("java/io/InputStream", "read", "([B)I", nat_InputStream_read_bulk),
    N("java/io/InputStream", "read", "([BII)I", nat_InputStream_read_bulk),
    N("java/io/InputStream", "available", "()I", nat_InputStream_available),
    N("java/io/InputStream", "close", "()V", nat_InputStream_close),
    N("java/io/InputStream", "skip", "(J)J", nat_InputStream_skip),

    /* java.io data streams (GL resource parsing + RMS save building) */
    N("java/io/DataInputStream", "<init>", "(Ljava/io/InputStream;)V", nat_DIS_init),
    N("java/io/DataInputStream", "read", "()I", nat_DIS_read),
    N("java/io/DataInputStream", "read", "([B)I", nat_DIS_read_bulk),
    N("java/io/DataInputStream", "read", "([BII)I", nat_DIS_read_bulk),
    N("java/io/DataInputStream", "readFully", "([B)V", nat_DIS_readFully),
    N("java/io/DataInputStream", "readFully", "([BII)V", nat_DIS_readFully),
    N("java/io/DataInputStream", "readByte", "()B", nat_DIS_readByte),
    N("java/io/DataInputStream", "readUnsignedByte", "()I", nat_DIS_readUnsignedByte),
    N("java/io/DataInputStream", "readShort", "()S", nat_DIS_readShort),
    N("java/io/DataInputStream", "readUnsignedShort", "()I", nat_DIS_readUnsignedShort),
    N("java/io/DataInputStream", "readInt", "()I", nat_DIS_readInt),
    N("java/io/DataInputStream", "readLong", "()J", nat_DIS_readLong),
    N("java/io/DataInputStream", "readBoolean", "()Z", nat_DIS_readBoolean),
    N("java/io/DataInputStream", "readChar", "()C", nat_DIS_readChar),
    N("java/io/DataInputStream", "readUTF", "()Ljava/lang/String;", nat_DIS_readUTF),
    N("java/io/DataInputStream", "skipBytes", "(I)I", nat_DIS_skipBytes),
    N("java/io/DataInputStream", "skip", "(J)J", nat_DIS_skip),
    N("java/io/DataInputStream", "available", "()I", nat_DIS_available),
    N("java/io/DataInputStream", "close", "()V", nat_DIS_close),
    N("java/io/ByteArrayInputStream", "<init>", "([B)V", nat_BAIS_init),
    N("java/io/ByteArrayInputStream", "<init>", "([BII)V", nat_BAIS_init),
    N("java/io/ByteArrayInputStream", "read", "()I", nat_BAIS_read),
    N("java/io/ByteArrayInputStream", "read", "([B)I", nat_BAIS_read_bulk),
    N("java/io/ByteArrayInputStream", "read", "([BII)I", nat_BAIS_read_bulk),
    N("java/io/ByteArrayInputStream", "available", "()I", nat_BAIS_available),
    N("java/io/ByteArrayInputStream", "skip", "(J)J", nat_BAIS_skip),
    N("java/io/ByteArrayInputStream", "close", "()V", nat_BAIS_close),
    N("java/io/ByteArrayOutputStream", "<init>", "()V", nat_BAOS_init),
    N("java/io/ByteArrayOutputStream", "<init>", "(I)V", nat_BAOS_init),
    N("java/io/ByteArrayOutputStream", "write", "(I)V", nat_BAOS_write_I),
    N("java/io/ByteArrayOutputStream", "write", "([BII)V", nat_BAOS_write_bulk),
    N("java/io/ByteArrayOutputStream", "toByteArray", "()[B", nat_BAOS_toByteArray),
    N("java/io/ByteArrayOutputStream", "size", "()I", nat_BAOS_size),
    N("java/io/ByteArrayOutputStream", "reset", "()V", nat_BAOS_reset),
    N("java/io/ByteArrayOutputStream", "flush", "()V", nat_BAOS_flush),
    N("java/io/ByteArrayOutputStream", "close", "()V", nat_BAOS_flush),
    N("java/io/DataOutputStream", "<init>", "(Ljava/io/OutputStream;)V", nat_DOS_init),
    N("java/io/DataOutputStream", "write", "(I)V", nat_DOS_write_I),
    N("java/io/DataOutputStream", "write", "([BII)V", nat_DOS_write_bulk),
    N("java/io/DataOutputStream", "writeInt", "(I)V", nat_DOS_writeInt),
    N("java/io/DataOutputStream", "writeShort", "(I)V", nat_DOS_writeShort),
    N("java/io/DataOutputStream", "writeLong", "(J)V", nat_DOS_writeLong),
    N("java/io/DataOutputStream", "writeUTF", "(Ljava/lang/String;)V", nat_DOS_writeUTF),
    N("java/io/DataOutputStream", "writeBoolean", "(Z)V", nat_DOS_write_I),
    N("java/io/DataOutputStream", "writeByte", "(I)V", nat_DOS_write_I),
    N("java/io/DataOutputStream", "writeChar", "(I)V", nat_DOS_writeShort),
    N("java/io/DataOutputStream", "flush", "()V", nat_DOS_flush),
    N("java/io/DataOutputStream", "size", "()I", nat_BAOS_size),
    N("java/io/DataOutputStream", "close", "()V", nat_DOS_flush),

    /* javax.microedition.io "comm:usb" — the Round 10 UsbCast pipe.
     * open() is registered under BOTH return types (StreamConnection
     * for fpj2me's own jars, Connection for javac-compiled ones).   */
    N("javax/microedition/io/Connector", "open", "(Ljava/lang/String;)Ljavax/microedition/io/StreamConnection;", nat_Connector_open),
    N("javax/microedition/io/Connector", "open", "(Ljava/lang/String;)Ljavax/microedition/io/Connection;", nat_Connector_open),
    N("javax/microedition/io/Connector", "openInputStream", "(Ljava/lang/String;)Ljava/io/InputStream;", nat_Connector_openInputStream),
    N("javax/microedition/io/Connector", "openOutputStream", "(Ljava/lang/String;)Ljava/io/OutputStream;", nat_Connector_openOutputStream),
    N("javax/microedition/io/Connector", "close", "(Ljava/lang/String;)V", nat_Connector_close),
    N("javax/microedition/io/StreamConnection", "openInputStream", "()Ljava/io/InputStream;", nat_SC_openInputStream),
    N("javax/microedition/io/StreamConnection", "openOutputStream", "()Ljava/io/OutputStream;", nat_SC_openOutputStream),
    N("javax/microedition/io/StreamConnection", "close", "()V", nat_SC_close),
    /* java.io.OutputStream: comm:usb TX + no-op fallback for others  */
    N("java/io/OutputStream", "write", "(I)V", nat_OutputStream_write_I),
    N("java/io/OutputStream", "write", "([B)V", nat_OutputStream_write_bulk),
    N("java/io/OutputStream", "write", "([BII)V", nat_OutputStream_write_bulk),
    N("java/io/OutputStream", "flush", "()V", nat_OutputStream_flush),
    N("java/io/OutputStream", "close", "()V", nat_OutputStream_close),

    /* java.lang.String */
    N("java/lang/String", "length", "()I", nat_String_length),
    N("java/lang/String", "charAt", "(I)C", nat_String_charAt),
    N("java/lang/String", "equals", "(Ljava/lang/Object;)Z", nat_String_equals),
    N("java/lang/String", "equalsIgnoreCase", "(Ljava/lang/String;)Z", nat_String_equalsIgnoreCase),
    N("java/lang/String", "hashCode", "()I", nat_String_hashCode),
    N("java/lang/String", "toCharArray", "()[C", nat_String_toCharArray),
    N("java/lang/String", "valueOf", "(I)Ljava/lang/String;", nat_String_valueOf_I),
    N("java/lang/String", "valueOf", "(J)Ljava/lang/String;", nat_String_valueOf_J),
    N("java/lang/String", "indexOf", "(Ljava/lang/String;)I", nat_String_indexOf),
    N("java/lang/String", "indexOf", "(I)I", nat_String_indexOf_ch),
    N("java/lang/String", "indexOf", "(Ljava/lang/String;I)I", nat_String_indexOf_from),
    N("java/lang/String", "substring", "(II)Ljava/lang/String;", nat_String_substring),
    N("java/lang/String", "substring", "(I)Ljava/lang/String;", nat_String_substring1),
    N("java/lang/String", "trim", "()Ljava/lang/String;", nat_String_trim),
    N("java/lang/String", "toUpperCase", "()Ljava/lang/String;", nat_String_toUpperCase),
    N("java/lang/String", "toLowerCase", "()Ljava/lang/String;", nat_String_toLowerCase),
    N("java/lang/String", "compareTo", "(Ljava/lang/String;)I", nat_String_compareTo),
    N("java/lang/String", "startsWith", "(Ljava/lang/String;)Z", nat_String_startsWith),
    N("java/lang/String", "endsWith", "(Ljava/lang/String;)Z", nat_String_endsWith),
    N("java/lang/String", "getBytes", "()[B", nat_String_getBytes),

    /* java.lang.StringBuffer */
    N("java/lang/StringBuffer", "<init>", "()V", nat_SB_init),
    N("java/lang/StringBuffer", "<init>", "(I)V", nat_SB_init_I),
    N("java/lang/StringBuffer", "<init>", "(Ljava/lang/String;)V", nat_SB_init_S),
    N("java/lang/StringBuffer", "append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;", nat_SB_append_S),
    N("java/lang/StringBuffer", "append", "(C)Ljava/lang/StringBuffer;", nat_SB_append_C),
    N("java/lang/StringBuffer", "append", "(I)Ljava/lang/StringBuffer;", nat_SB_append_I),
    N("java/lang/StringBuffer", "append", "(J)Ljava/lang/StringBuffer;", nat_SB_append_J),
    N("java/lang/StringBuffer", "toString", "()Ljava/lang/String;", nat_SB_toString),
    N("java/lang/StringBuffer", "length", "()I", nat_SB_length),
    N("java/lang/StringBuffer", "charAt", "(I)C", nat_SB_charAt),
    N("java/lang/StringBuffer", "setLength", "(I)V", nat_SB_setLength),

    /* java.lang.Math — complete CLDC 1.1 set (Round 9). The (JJ)J
     * max/min are the pair WWG's boot actually tripped on.            */
    N("java/lang/Math", "abs", "(I)I", nat_Math_abs_I),
    N("java/lang/Math", "abs", "(J)J", nat_Math_abs_J),
    N("java/lang/Math", "abs", "(F)F", nat_Math_abs_F),
    N("java/lang/Math", "abs", "(D)D", nat_Math_abs_D),
    N("java/lang/Math", "max", "(II)I", nat_Math_max_I),
    N("java/lang/Math", "min", "(II)I", nat_Math_min_I),
    N("java/lang/Math", "max", "(JJ)J", nat_Math_max_J),
    N("java/lang/Math", "min", "(JJ)J", nat_Math_min_J),
    N("java/lang/Math", "max", "(FF)F", nat_Math_max_F),
    N("java/lang/Math", "min", "(FF)F", nat_Math_min_F),
    N("java/lang/Math", "max", "(DD)D", nat_Math_max_D),
    N("java/lang/Math", "min", "(DD)D", nat_Math_min_D),
    N("java/lang/Math", "sqrt", "(D)D", nat_Math_sqrt),
    N("java/lang/Math", "sin", "(D)D", nat_Math_sin),
    N("java/lang/Math", "cos", "(D)D", nat_Math_cos),
    N("java/lang/Math", "tan", "(D)D", nat_Math_tan),
    N("java/lang/Math", "ceil", "(D)D", nat_Math_ceil),
    N("java/lang/Math", "floor", "(D)D", nat_Math_floor),
    N("java/lang/Math", "toRadians", "(D)D", nat_Math_toRadians),
    N("java/lang/Math", "toDegrees", "(D)D", nat_Math_toDegrees),

    /* java.lang.System / Runtime / Thread / Throwable */
    N("java/lang/System", "currentTimeMillis", "()J", nat_System_time),
    N("java/lang/System", "sleep", "(J)V", nat_System_sleep),
    N("java/lang/System", "gc", "()V", nat_System_gc),
    N("java/lang/System", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", nat_System_arraycopy),
    N("java/lang/System", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", nat_System_getProperty),
    N("java/lang/System", "exit", "(I)V", nat_System_exit),
    N("java/io/PrintStream", "println", "(Ljava/lang/String;)V", nat_System_noop),
    N("java/io/PrintStream", "println", "(Ljava/lang/Object;)V", nat_System_noop),
    N("java/lang/Runtime", "getRuntime", "()Ljava/lang/Runtime;", nat_Runtime_get),
    N("java/lang/Runtime", "freeMemory", "()J", nat_Runtime_freeMemory),
    N("java/lang/Runtime", "totalMemory", "()J", nat_Runtime_totalMemory),
    N("java/lang/Runtime", "gc", "()V", nat_Runtime_gc),
    N("java/lang/Runtime", "exit", "(I)V", nat_Runtime_exit),
    N("java/lang/Thread", "<init>", "()V", nat_Thread_init),
    N("java/lang/Thread", "<init>", "(Ljava/lang/Runnable;)V", nat_Thread_init_R),
    N("java/lang/Thread", "sleep", "(J)V", nat_Thread_sleep),
    N("java/lang/Thread", "start", "()V", nat_Thread_start),
    N("java/lang/Thread", "yield", "()V", nat_Thread_yield),
    N("java/lang/Thread", "activeCount", "()I", nat_Thread_activeCount),
    N("java/lang/String", "<init>", "()V", nat_String_init_empty),
    N("java/lang/String", "<init>", "(Ljava/lang/String;)V", nat_String_init_copy),
    N("java/lang/String", "<init>", "([BII)V", nat_String_init_bytes),
    N("java/lang/String", "<init>", "([BIILjava/lang/String;)V", nat_String_init_bytes),
    N("java/lang/String", "<init>", "([C)V", nat_String_init_chars),
    N("java/lang/String", "<init>", "([CII)V", nat_String_init_chars),
    N("java/lang/StringBuffer", "append", "(Z)Ljava/lang/StringBuffer;", nat_SB_append_Z),
    N("java/lang/StringBuffer", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;", nat_SB_append_O),
    N("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", nat_Integer_parseInt),
    N("java/lang/Integer", "parseInt", "(Ljava/lang/String;I)I", nat_Integer_parseInt_R),
    N("java/lang/Integer", "toString", "(I)Ljava/lang/String;", nat_Integer_toString),
    N("java/lang/Integer", "toString", "(II)Ljava/lang/String;", nat_Integer_toString_R),
    N("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", nat_Integer_valueOf),
    N("java/lang/Integer", "valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", nat_Integer_valueOf_S),
    N("java/lang/Integer", "valueOf", "(Ljava/lang/String;I)Ljava/lang/Integer;", nat_Integer_valueOf_SR),
    N("java/lang/Integer", "<init>", "(I)V", nat_Integer_init),
    N("java/lang/Integer", "intValue", "()I", nat_Integer_intValue),
    N("java/lang/Integer", "byteValue", "()B", nat_Integer_byteValue),
    N("java/lang/Integer", "toString", "()Ljava/lang/String;", nat_w1_toString),
    N("java/lang/Integer", "hashCode", "()I", nat_w1_hashCode),
    N("java/lang/Integer", "equals", "(Ljava/lang/Object;)Z", nat_w1_equals),
    N("java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", nat_Boolean_valueOf),
    N("java/lang/Boolean", "valueOf", "(Ljava/lang/String;)Ljava/lang/Boolean;", nat_Boolean_valueOf_S),
    N("java/lang/Boolean", "<init>", "(Z)V", nat_Boolean_init),
    N("java/lang/Boolean", "booleanValue", "()Z", nat_Boolean_booleanValue),
    N("java/lang/Boolean", "toString", "()Ljava/lang/String;", nat_Boolean_toString),
    N("java/lang/Boolean", "hashCode", "()I", nat_Boolean_hashCode),
    N("java/lang/Boolean", "equals", "(Ljava/lang/Object;)Z", nat_w1_equals),
    N("java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", nat_Long_valueOf),
    N("java/lang/Long", "valueOf", "(Ljava/lang/String;)Ljava/lang/Long;", nat_Long_valueOf_S),
    N("java/lang/Long", "parseLong", "(Ljava/lang/String;)J", nat_Long_parseLong),
    N("java/lang/Long", "<init>", "(J)V", nat_Long_init),
    N("java/lang/Long", "longValue", "()J", nat_Long_longValue),
    N("java/lang/Long", "toString", "(J)Ljava/lang/String;", nat_Long_toString_J),
    N("java/lang/Long", "toString", "()Ljava/lang/String;", nat_Long_toString_inst),
    N("java/lang/Long", "hashCode", "()I", nat_Long_hashCode),
    N("java/lang/Long", "equals", "(Ljava/lang/Object;)Z", nat_w2_equals),
    N("java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;", nat_Byte_valueOf),
    N("java/lang/Byte", "parseByte", "(Ljava/lang/String;)B", nat_Byte_parseByte),
    N("java/lang/Byte", "<init>", "(B)V", nat_Byte_init),
    N("java/lang/Byte", "byteValue", "()B", nat_Byte_byteValue),
    N("java/lang/Byte", "toString", "()Ljava/lang/String;", nat_w1_toString),
    N("java/lang/Byte", "hashCode", "()I", nat_w1_hashCode),
    N("java/lang/Byte", "equals", "(Ljava/lang/Object;)Z", nat_w1_equals),
    N("java/lang/Short", "valueOf", "(S)Ljava/lang/Short;", nat_Short_valueOf),
    N("java/lang/Short", "<init>", "(S)V", nat_Short_init),
    N("java/lang/Short", "shortValue", "()S", nat_Short_shortValue),
    N("java/lang/Short", "toString", "()Ljava/lang/String;", nat_w1_toString),
    N("java/lang/Short", "hashCode", "()I", nat_w1_hashCode),
    N("java/lang/Short", "equals", "(Ljava/lang/Object;)Z", nat_w1_equals),
    N("java/lang/Character", "valueOf", "(C)Ljava/lang/Character;", nat_Character_valueOf),
    N("java/lang/Character", "<init>", "(C)V", nat_Character_init),
    N("java/lang/Character", "charValue", "()C", nat_Character_charValue),
    N("java/lang/Character", "toString", "()Ljava/lang/String;", nat_Character_toString),
    N("java/lang/Character", "hashCode", "()I", nat_w1_hashCode),
    N("java/lang/Character", "equals", "(Ljava/lang/Object;)Z", nat_w1_equals),
    N("java/lang/Throwable", "<init>", "()V", nat_Throwable_init),
    N("java/lang/Throwable", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/Throwable", "getMessage", "()Ljava/lang/String;", nat_Throwable_getMessage),
    N("java/lang/Throwable", "toString", "()Ljava/lang/String;", nat_Throwable_toString),
    N("java/lang/Throwable", "printStackTrace", "()V", nat_Throwable_printStackTrace),

    /* Standard java.lang exception stubs (Round 9): <init> registered
     * on the EXACT class so 'new X()' / 'new X(msg)' from bytecode
     * resolves immediately and Class.forName finds them via
     * lib_known; getMessage()/toString() still resolve up the stub
     * chain to the Throwable natives. The super chains themselves
     * (IndexOutOfBounds -> RuntimeException -> Exception -> Throwable
     * ...) live in interp.c stub_super_of so catch blocks match.    */
    N("java/lang/IndexOutOfBoundsException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/IndexOutOfBoundsException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/ArrayIndexOutOfBoundsException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/ArrayIndexOutOfBoundsException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/StringIndexOutOfBoundsException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/StringIndexOutOfBoundsException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/IllegalArgumentException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/IllegalArgumentException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/IllegalStateException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/IllegalStateException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/NullPointerException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/NullPointerException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/ArithmeticException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/ArithmeticException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/NumberFormatException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/NumberFormatException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/ClassCastException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/ClassCastException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),
    N("java/lang/RuntimeException", "<init>", "()V", nat_Throwable_init),
    N("java/lang/RuntimeException", "<init>", "(Ljava/lang/String;)V", nat_Throwable_init),

    /* java.util */
    N("java/util/Random", "<init>", "()V", nat_Random_init),
    N("java/util/Random", "<init>", "(J)V", nat_Random_seed),
    N("java/util/Random", "setSeed", "(J)V", nat_Random_seed),
    N("java/util/Random", "nextInt", "()I", nat_Random_nextInt),
    N("java/util/Random", "nextInt", "(I)I", nat_Random_nextInt_B),
    N("java/util/Vector", "<init>", "()V", nat_Vector_init),
    N("java/util/Vector", "addElement", "(Ljava/lang/Object;)V", nat_Vector_addElement),
    N("java/util/Vector", "size", "()I", nat_Vector_size),
    N("java/util/Vector", "isEmpty", "()Z", nat_Vector_isEmpty),
    N("java/util/Vector", "elementAt", "(I)Ljava/lang/Object;", nat_Vector_elementAt),
    N("java/util/Vector", "setElementAt", "(Ljava/lang/Object;I)V", nat_Vector_setElementAt),
    N("java/util/Vector", "insertElementAt", "(Ljava/lang/Object;I)V", nat_Vector_insertElementAt),
    N("java/util/Vector", "removeElementAt", "(I)V", nat_Vector_removeElementAt),
    N("java/util/Vector", "removeAllElements", "()V", nat_Vector_removeAllElements),
    N("java/util/Vector", "firstElement", "()Ljava/lang/Object;", nat_Vector_firstElement),
    N("java/util/Vector", "lastElement", "()Ljava/lang/Object;", nat_Vector_lastElement),
    N("java/util/Vector", "indexOf", "(Ljava/lang/Object;)I", nat_Vector_indexOf),
    N("java/util/Hashtable", "<init>", "()V", nat_Hashtable_init),
    N("java/util/Hashtable", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", nat_Hashtable_put),
    N("java/util/Hashtable", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", nat_Hashtable_get),
    N("java/util/Hashtable", "size", "()I", nat_Hashtable_size),
    N("java/util/Hashtable", "isEmpty", "()Z", nat_Hashtable_isEmpty),
    N("java/util/Hashtable", "containsKey", "(Ljava/lang/Object;)Z", nat_Hashtable_containsKey),
    N("java/util/Hashtable", "remove", "(Ljava/lang/Object;)Ljava/lang/Object;", nat_Hashtable_remove),

    /* javax.microedition.lcdui.Image */
    N("javax/microedition/lcdui/Image", "createImage", "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;", nat_Image_create_str),
    N("javax/microedition/lcdui/Image", "createImage", "([BII)Ljavax/microedition/lcdui/Image;", nat_Image_create_bytes),
    N("javax/microedition/lcdui/Image", "createImage", "(II)Ljavax/microedition/lcdui/Image;", nat_Image_create_wh),
    N("javax/microedition/lcdui/Image", "createImage", "(Ljavax/microedition/lcdui/Image;IIIII)Ljavax/microedition/lcdui/Image;", nat_Image_create_region),
    N("javax/microedition/lcdui/Image", "createRGBImage", "([IIIZ)Ljavax/microedition/lcdui/Image;", nat_Image_createRGB),
    N("javax/microedition/lcdui/Image", "getRGB", "([IIIIII)V", nat_Image_getRGB),
    N("javax/microedition/lcdui/Image", "getWidth", "()I", nat_Image_getWidth),
    N("javax/microedition/lcdui/Image", "getHeight", "()I", nat_Image_getHeight),
    N("javax/microedition/lcdui/Image", "getGraphics", "()Ljavax/microedition/lcdui/Graphics;", nat_Image_getGraphics),

    /* com.nokia.mid.ui: drawImage flips via the shared blitter */
    N("com/nokia/mid/ui/DirectUtils", "getDirectGraphics", "(Ljavax/microedition/lcdui/Graphics;)Lcom/nokia/mid/ui/DirectGraphics;", nat_DirectUtils_getDG),
    N("com/nokia/mid/ui/DirectGraphics", "drawImage", "(Ljavax/microedition/lcdui/Image;IIII)V", nat_DG_drawImage),
    N("com/nokia/mid/ui/DirectGraphics", "drawPixels", "([B[BIIIIIIII)V", nat_DG_drawPixels),

    /* javax.microedition.lcdui.Graphics */
    N("javax/microedition/lcdui/Graphics", "setColor", "(I)V", nat_Gfx_setColor_I),
    N("javax/microedition/lcdui/Graphics", "setColor", "(III)V", nat_Gfx_setColor_III),
    N("javax/microedition/lcdui/Graphics", "getColor", "()I", nat_Gfx_getColor),
    N("javax/microedition/lcdui/Graphics", "fillRect", "(IIII)V", nat_Gfx_fillRect),
    N("javax/microedition/lcdui/Graphics", "drawRect", "(IIII)V", nat_Gfx_drawRect),
    N("javax/microedition/lcdui/Graphics", "drawLine", "(IIII)V", nat_Gfx_drawLine),
    N("javax/microedition/lcdui/Graphics", "fillTriangle", "(IIIIII)V", nat_Gfx_fillTriangle),
    N("javax/microedition/lcdui/Graphics", "clipRect", "(IIII)V", nat_Gfx_clipRect),
    N("javax/microedition/lcdui/Graphics", "drawArc", "(IIIIII)V", nat_Gfx_drawArc),
    N("javax/microedition/lcdui/Graphics", "fillArc", "(IIIIII)V", nat_Gfx_fillArc),
    N("javax/microedition/lcdui/Graphics", "drawRoundRect", "(IIIIII)V", nat_Gfx_drawRoundRect),
    N("javax/microedition/lcdui/Graphics", "setClip", "(IIII)V", nat_Gfx_setClip),
    N("javax/microedition/lcdui/Graphics", "getClipX", "()I", nat_Gfx_getClipX),
    N("javax/microedition/lcdui/Graphics", "getClipY", "()I", nat_Gfx_getClipY),
    N("javax/microedition/lcdui/Graphics", "getClipWidth", "()I", nat_Gfx_getClipWidth),
    N("javax/microedition/lcdui/Graphics", "getClipHeight", "()I", nat_Gfx_getClipHeight),
    N("javax/microedition/lcdui/Graphics", "translate", "(II)V", nat_Gfx_translate),
    N("javax/microedition/lcdui/Graphics", "getTranslateX", "()I", nat_Gfx_getTranslateX),
    N("javax/microedition/lcdui/Graphics", "getTranslateY", "()I", nat_Gfx_getTranslateY),
    N("javax/microedition/lcdui/Graphics", "drawImage", "(Ljavax/microedition/lcdui/Image;III)V", nat_Gfx_drawImage),
    N("javax/microedition/lcdui/Graphics", "drawRegion", "(Ljavax/microedition/lcdui/Image;IIIIIIII)V", nat_Gfx_drawRegion),
    N("javax/microedition/lcdui/Graphics", "drawString", "(Ljava/lang/String;III)V", nat_Gfx_drawString),
    N("javax/microedition/lcdui/Graphics", "drawRGB", "([IIIIIIIZ)V", nat_Gfx_drawRGB),
    N("javax/microedition/lcdui/Graphics", "setFont", "(Ljavax/microedition/lcdui/Font;)V", nat_Gfx_setFont),
    N("javax/microedition/lcdui/Graphics", "getFont", "()Ljavax/microedition/lcdui/Font;", nat_Gfx_getFont),

    /* Canvas / GameCanvas */
    N("javax/microedition/lcdui/Canvas", "getWidth", "()I", nat_Canvas_getWidth),
    N("javax/microedition/lcdui/Canvas", "<init>", "()V", nat_Canvas_init),
    N("javax/microedition/lcdui/Canvas", "getHeight", "()I", nat_Canvas_getHeight),
    N("javax/microedition/lcdui/Canvas", "setFullScreenMode", "(Z)V", nat_Canvas_setFullScreenMode),
    N("javax/microedition/lcdui/Canvas", "serviceRepaints", "()V", nat_Canvas_serviceRepaints),
    N("javax/microedition/lcdui/Canvas", "repaint", "()V", nat_Canvas_repaint),
    N("javax/microedition/lcdui/Canvas", "getGameAction", "(I)I", nat_Canvas_getGameAction),
    N("javax/microedition/lcdui/game/GameCanvas", "flushGraphics", "()V", nat_GameCanvas_flush),
    N("javax/microedition/lcdui/game/GameCanvas", "flushGraphics", "(IIII)V", nat_GameCanvas_flush),
    N("javax/microedition/lcdui/game/GameCanvas", "getGraphics", "()Ljavax/microedition/lcdui/Graphics;", nat_GameCanvas_getGraphics),
    N("javax/microedition/lcdui/game/GameCanvas", "getKeyStates", "()I", nat_GameCanvas_getKeyStates),

    /* Display / Font */
    N("javax/microedition/lcdui/Display", "getDisplay", "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", nat_Display_get),
    N("javax/microedition/lcdui/Display", "setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V", nat_Display_setCurrent),
    /* Command / Displayable softkey plumbing (Round 9): inert but
     * real, so the WWG menu path never hits the no-op fallback     */
    N("javax/microedition/lcdui/Command", "<init>", "(Ljava/lang/String;II)V", nat_Command_init),
    N("javax/microedition/lcdui/Displayable", "addCommand", "(Ljavax/microedition/lcdui/Command;)V", nat_Displayable_addCommand),
    N("javax/microedition/lcdui/Displayable", "removeCommand", "(Ljavax/microedition/lcdui/Command;)V", nat_Displayable_removeCommand),
    N("javax/microedition/lcdui/Displayable", "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V", nat_Displayable_setCommandListener),
    N("javax/microedition/lcdui/Display", "getCurrent", "()Ljavax/microedition/lcdui/Displayable;", nat_Display_getCurrent),
    N("javax/microedition/lcdui/Font", "getDefaultFont", "()Ljavax/microedition/lcdui/Font;", nat_Font_getDefault),
    N("javax/microedition/lcdui/Font", "getFont", "(III)Ljavax/microedition/lcdui/Font;", nat_Font_getFont),
    N("javax/microedition/lcdui/Font", "stringWidth", "(Ljava/lang/String;)I", nat_Font_stringWidth),
    N("javax/microedition/lcdui/Font", "charWidth", "(C)I", nat_Font_charWidth),
    N("javax/microedition/lcdui/Font", "getHeight", "()I", nat_Font_getHeight),
    N("javax/microedition/lcdui/Font", "getBaselinePosition", "()I", nat_Font_getBaselinePosition),

    /* rms */
    N("javax/microedition/rms/RecordStore", "openRecordStore", "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;", nat_RS_open),
    N("javax/microedition/rms/RecordStore", "closeRecordStore", "()V", nat_RS_close_static),
    N("javax/microedition/rms/RecordStore", "addRecord", "([BII)I", nat_RS_addRecord),
    N("javax/microedition/rms/RecordStore", "getRecord", "(I)[B", nat_RS_getRecord),
    N("javax/microedition/rms/RecordStore", "getRecord", "(I[BI)I", nat_RS_getRecord_into),
    N("javax/microedition/rms/RecordStore", "setRecord", "(I[BII)V", nat_RS_setRecord),
    N("javax/microedition/rms/RecordStore", "getNumRecords", "()I", nat_RS_getNumRecords),
    N("javax/microedition/rms/RecordStore", "deleteRecord", "(I)V", nat_RS_deleteRecord),

    /* media */
    N("javax/microedition/media/Manager", "playTone", "(III)V", nat_Manager_playTone),
    N("javax/microedition/media/Manager", "createPlayer", "(Ljava/lang/String;Ljava/lang/String;)Ljavax/microedition/media/Player;", nat_Manager_createPlayer),
    N("javax/microedition/media/Manager", "createPlayer", "(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;", nat_Manager_createPlayer_IS),
    N("javax/microedition/media/Player", "realize", "()V", nat_Player_realize),
    N("javax/microedition/media/Player", "prefetch", "()V", nat_Player_prefetch),
    N("javax/microedition/media/Player", "start", "()V", nat_Player_start),
    N("javax/microedition/media/Player", "stop", "()V", nat_Player_stop),
    N("javax/microedition/media/Player", "close", "()V", nat_Player_close),
    N("javax/microedition/media/Player", "deallocate", "()V", nat_Player_deallocate),
    N("javax/microedition/media/Player", "setLoopCount", "(I)V", nat_Player_setLoopCount),
    N("javax/microedition/media/Player", "addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", nat_Player_addPlayerListener),
    N("javax/microedition/media/Player", "getState", "()I", nat_Player_getState),
    N("javax/microedition/media/Player", "getDuration", "()J", nat_Player_getDuration),
    N("javax/microedition/media/Player", "getMediaTime", "()J", nat_Player_getMediaTime),

    /* com.nokia.mid.sound.Sound — GLLib's Series-40 sound engine.
     * getState() MUST report SOUND_STOPPED(1): 0 = SOUND_PLAYING made
     * the splash state machine wait out its full audio timeout per
     * frame (~2.1 s/frame on the it2163). Constructor variants cover
     * the byte[] and URL forms; play(loop)/init(type) cover both
     * overloads GLLib builds ship with.                              */
    N("com/nokia/mid/sound/Sound", "<init>", "([BI)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "<init>", "(Ljava/lang/String;I)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "<init>", "(Ljava/lang/String;)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "init", "([BI)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "init", "(I)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "init", "()V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "play", "()V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "play", "(I)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "stop", "()V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "release", "()V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "resume", "()V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "setGain", "(I)V", nat_Sound_setGain),
    N("com/nokia/mid/sound/Sound", "getGain", "()I", nat_Sound_getGain),
    N("com/nokia/mid/sound/Sound", "setSoundListener", "(Lcom/nokia/mid/sound/SoundListener;)V", nat_Sound_noop),
    N("com/nokia/mid/sound/Sound", "getState", "()I", nat_Sound_getState),

    /* com.nokia.mid.ui.DeviceControl — lights/vibra: silent no-ops    */
    N("com/nokia/mid/ui/DeviceControl", "setLights", "(II)V", nat_DeviceControl_noop),
    N("com/nokia/mid/ui/DeviceControl", "setLights", "(I)Z", nat_DeviceControl_noop),
    N("com/nokia/mid/ui/DeviceControl", "startVibra", "(IJ)V", nat_DeviceControl_noop),
    N("com/nokia/mid/ui/DeviceControl", "startVibra", "(J)V", nat_DeviceControl_noop),
    N("com/nokia/mid/ui/DeviceControl", "stopVibra", "()V", nat_DeviceControl_noop),
    N("com/nokia/mid/ui/DeviceControl", "flashLights", "(J)V", nat_DeviceControl_noop),

    /* midlet */
    N("javax/microedition/midlet/MIDlet", "getAppProperty", "(Ljava/lang/String;)Ljava/lang/String;", nat_MIDlet_getAppProperty),
    N("javax/microedition/midlet/MIDlet", "<init>", "()V", nat_MIDlet_init),
    N("javax/microedition/midlet/MIDlet", "notifyDestroyed", "()V", nat_MIDlet_notifyDestroyed),
    N("javax/microedition/midlet/MIDlet", "platformRequest", "(Ljava/lang/String;)Z", nat_MIDlet_platformRequest),

    /* Class.forName lives at the bottom (needs the full registry)  */
    N("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", nat_Class_forName),
};

const NativeEntry *native_lookup(const char *cls, const char *name,
                                 const char *sig)
{
    for (u32 i = 0; i < sizeof natives / sizeof natives[0]; i++)
        if (!strcmp(natives[i].cls, cls) && !strcmp(natives[i].name, name)
            && !strcmp(natives[i].sig, sig))
            return &natives[i];
    return 0;
}

/* ------------------------------------------------------------------ */
/* the graceful-miss fallback                                           */
/* ------------------------------------------------------------------ */
/* jar calls a library method we have not modeled (com/nokia/mid/sound,
 * ...). Returning 0 used to mean "unresolved" -> fpj_panic -> a SILENT
 * frozen green screen on the phone. Instead: log it once (resolve_virtual
 * caches the synthesized method on the stub class, so the log doubles
 * as the TODO list) and hand back a no-op whose zeroed return is a
 * valid J/false/0/null.                                              */
static void nat_missing_noop(const Slot *a, int n, Slot *r)
{ (void)a; (void)n; (void)r; }

const NativeEntry *native_missing(const char *cls, const char *name,
                                  const char *sig)
{
    fp_printf("fpj2me: native missing %s.%s%s (no-op)\n", cls, name, sig);
    /* vitals: remember the LAST missing call ON TARGET (no log channel
     * there) — the diag overlay shows "M:cls.name" and the fatal screen
     * prints it too. A game stuck right after a splash + an M: line is
     * the smoking gun (e.g. Object.wait() no-op'd to null).          */
    {   int i = 0;
        const char *p;
        for (p = cls; p && *p && i < 60; p++) fpj_miss_native[i++] = *p;
        if (i < 92) fpj_miss_native[i++] = '.';
        for (p = name; p && *p && i < 94; p++) fpj_miss_native[i++] = *p;
        fpj_miss_native[i] = 0;
    }
    static NativeEntry e;
    e.cls = cls; e.name = name; e.sig = sig; e.fn = nat_missing_noop;
    return &e;
}

/* ------------------------------------------------------------------
 * Class.forName strictness + known-library table
 * ------------------------------------------------------------------ */
static const char *const known_libs[] = {
    "java/lang/Object", "java/lang/Class", "java/lang/Runnable",
    "java/lang/Thread", "java/lang/String", "java/lang/StringBuffer",
    "java/lang/StringBuilder", "java/lang/Math", "java/lang/System",
    "java/lang/Runtime", "java/lang/Integer", "java/lang/Long",
    "java/lang/Boolean", "java/lang/Character", "java/lang/Byte",
    "java/lang/Short", "java/lang/Throwable", "java/lang/Exception",
    "java/lang/RuntimeException", "java/lang/Error",
    "java/util/Vector", "java/util/Hashtable", "java/util/Random",
    "java/util/Enumeration", "java/util/Stack", "java/util/Timer",
    "java/util/TimerTask",
    "java/io/InputStream", "java/io/OutputStream", "java/io/PrintStream",
    "java/io/DataInputStream", "java/io/DataOutputStream",
    "java/io/ByteArrayInputStream", "java/io/ByteArrayOutputStream",
    "java/io/IOException", "java/io/EOFException",
    "javax/microedition/lcdui/Displayable", "javax/microedition/lcdui/Canvas",
    "javax/microedition/lcdui/game/GameCanvas", "javax/microedition/lcdui/game/Layer",
    "javax/microedition/lcdui/game/Sprite", "javax/microedition/lcdui/game/TiledLayer",
    "javax/microedition/lcdui/game/LayerManager", "javax/microedition/lcdui/Screen",
    "javax/microedition/lcdui/Form", "javax/microedition/lcdui/List",
    "javax/microedition/lcdui/TextBox", "javax/microedition/lcdui/Alert",
    "javax/microedition/lcdui/Command", "javax/microedition/lcdui/Item",
    "javax/microedition/lcdui/CommandListener",  /* WWG menu interface  */
    "javax/microedition/midlet/MIDlet",
    "javax/microedition/rms/RecordStore",
    "javax/microedition/rms/RecordListener",
    "javax/microedition/media/Manager", "javax/microedition/media/Player",
    "javax/microedition/media/PlayerListener",
    "com/nokia/mid/ui/DirectUtils",       /* drawImage flips: modeled  */
    "com/nokia/mid/ui/DirectGraphics",
    "javax/microedition/io/Connector",    /* Round 10: comm:usb pipe   */
    "javax/microedition/io/StreamConnection",
    "javax/microedition/io/Connection",
    "javax/microedition/io/ConnectionNotFoundException",
    "com/nokia/mid/sound/Sound",           /* modeled: silent, STOPPED  */
    "com/nokia/mid/sound/SoundListener",
    "com/nokia/mid/ui/DeviceControl",      /* modeled: lights/vibra off */
};

static int lib_known(const char *name)
{
    for (u32 i = 0; i < sizeof known_libs / sizeof known_libs[0]; i++)
        if (!strcmp(known_libs[i], name)) return 1;
    /* any class with a registered native is modeled too */
    for (u32 i = 0; i < sizeof natives / sizeof natives[0]; i++)
        if (!strcmp(natives[i].cls, name)) return 1;
    return 0;
}

static void nat_Class_forName(const Slot *a, int n, Slot *r)
{
    (void)n;
    r[0].ref = 0;
    char name[96];
    str_to_c(AR(a, 0), name, sizeof name);
    if (!name[0]) {
        fpj_throw(fpj_new_exception("java/lang/ClassNotFoundException", ""));
        return;
    }
    /* Class.forName takes DOTTED names; our registry is slashed     */
    for (char *p = name; *p; p++) if (*p == '.') *p = '/';

    JClass *c = 0;
    char path[112];
    if (strlen(name) + 7 < sizeof path) {
        strcpy(path, name); strcat(path, ".class");
        int found = 0;
        if (game_jar_ok() && jar_find(jar_game(), path)) found = 1;
        if (!found && boot_jar_ok() && jar_find(jar_boot(), path)) found = 1;
        if (found) c = class_load(name);
    }
    if (!c && lib_known(name)) c = class_load(name);   /* modeled stub */
    if (!c) {
        /* the CATCHABLE "unsupported API" signal the game expects    */
        fpj_throw(fpj_new_exception("java/lang/ClassNotFoundException", name));
        return;
    }
    r[0].ref = class_mirror(c);
}
