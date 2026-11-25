/*
 * fpj2me — midp.h : MIDP 2.0 bridge types + constants the natives
 * implement. One C module per subsystem (image / graphics / input /
 * rms / media / lifecycle).
 */
#ifndef FPJ2ME_MIDP_H
#define FPJ2ME_MIDP_H

#include "fpj2me.h"

/* ----- Images ------------------------------------------------------- */
typedef struct JImage {
    u16     w, h;
    u16    *px;        /* RGB565, row-major, 0x0000 == transparent     */
    u8      immutable; /* MIDP: createImage(w,h) => mutable             */
} JImage;

/* transparent sentinel: alpha < 128 decodes to 0x0000; pure opaque
 * black is remapped to 0x0001 (imperceptible on 65k panels, and it
 * keeps the blitter branch-free). Trade-off documented in README.   */
#define PX_TRANS 0x0000
#define PX_BLACK 0x0001

/* midp_image.c */
JImage *image_from_png(const u8 *data, u32 size);   /* createImage(bytes)*/
JImage *image_create(u16 w, u16 h);                 /* mutable buffer    */
JImage *image_from_argb(const u32 *argb, int w, int h, int process_alpha);
                                                   /* createRGBImage    */
JImage *image_sub(JImage *src, u16 x, u16 y, u16 w, u16 h,
                  int transform);                   /* createImage(img,..)*/
void    image_free(JImage *im);

/* ----- Graphics ------------------------------------------------------ */
/* MIDP Graphics anchor bits (TOP=16 LEFT=4 etc.)                      */
#define GA_LEFT 4
#define GA_RIGHT 8
#define GA_HCENTER 1
#define GA_TOP 16
#define GA_BOTTOM 32
#define GA_VCENTER 2
#define GA_BASELINE 64

/* Sprite.TRANS_* — verified against JSR-118 constant-values page:    */
enum {
    TRANS_NONE         = 0,
    TRANS_MIRROR_ROT180 = 1,
    TRANS_MIRROR        = 2,
    TRANS_ROT180        = 3,
    TRANS_MIRROR_ROT270 = 4,
    TRANS_ROT90         = 5,
    TRANS_ROT270        = 6,
    TRANS_MIRROR_ROT90  = 7
};

typedef struct MGraphics {
    JImage  *target;       /* screen or a mutable image                 */
    short    cx, cy, cw, ch;   /* clip, already intersected w/ target   */
    short    tx, ty;       /* translate                                          */
    u16      color;        /* current RGB565 color                             */
} MGraphics;

/* midp_graphics.c */
void gfx_reset(MGraphics *g, JImage *target);
void gfx_clip(MGraphics *g, int x, int y, int w, int h);   /* setClip   */
void gfx_clip_rect(MGraphics *g, int x, int y, int w, int h); /* clipRect */
void gfx_translate(MGraphics *g, int dx, int dy);
void gfx_color_rgb(MGraphics *g, int r, int gg, int b);
void gfx_color_int(MGraphics *g, u32 argb);
void gfx_fill_rect(MGraphics *g, int x, int y, int w, int h);
void gfx_draw_rect(MGraphics *g, int x, int y, int w, int h);
void gfx_draw_line(MGraphics *g, int x1, int y1, int x2, int y2);
/* arcs (Round 9): drawArc/fillArc share the parametric stepper —
 * fill=1 fills the PIE; angles in MIDP degrees (0 = 3 o'clock, CCW) */
void gfx_arc(MGraphics *g, int x, int y, int w, int h,
             int start, int sweep, int fill);
void gfx_draw_roundrect(MGraphics *g, int x, int y, int w, int h,
                        int arcw, int arch);
/* shared pure-C sin/cos (Math natives + arc stepper; no libm)      */
void gfx_sincos(double x, double *so, double *co);
void gfx_fill_triangle(MGraphics *g, int x1, int y1, int x2, int y2,
                       int x3, int y3);
void gfx_draw_image(MGraphics *g, const JImage *im,
                    int x, int y, int anchor);
void gfx_draw_region(MGraphics *g, const JImage *src,
                     int sx, int sy, int sw, int sh, int transform,
                     int dx, int dy, int anchor);
void gfx_draw_rgb(MGraphics *g, const u32 *rgb, int off, int scanlen,
                  int x, int y, int w, int h, int process_alpha);
/* Round 10 (UsbCast): raw little-endian RGB565 byte-pair blit —
 * the zero-conversion fast path behind DirectGraphics.drawPixels
 * (TYPE_USHORT_565_RGB, identity manipulation). Full fidelity:
 * no PX_TRANS remap, an exact 0x0000 stays 0x0000.                */
void gfx_draw_rgb565(MGraphics *g, const u8 *px, int off, int scanlen,
                     int x, int y, int w, int h);
void gfx_draw_string(MGraphics *g, const char *utf8,
                     int x, int y, int anchor);
int  gfx_char_width(void);  int gfx_string_height(void);

/* GameCanvas off-screen buffer == fp_screen itself; flushGraphics()   */
/* calls fp_lcd_flush(). Optionally track a dirty rect union here.     */

/* ----- Keypad -------------------------------------------------------- */
/* MIDP key codes: digits are ASCII, soft keys are the de-facto
 * Nokia/SonyEricsson negatives every 2007+ Gameloft build listens to.*/
enum {
    KEY_NUM0 = 48, KEY_NUM1 = 49, KEY_NUM2 = 50, KEY_NUM3 = 51,
    KEY_NUM4 = 52, KEY_NUM5 = 53, KEY_NUM6 = 54, KEY_NUM7 = 55,
    KEY_NUM8 = 56, KEY_NUM9 = 57,
    KEY_STAR = 42, KEY_POUND = 35,
    ACT_UP = -1, ACT_DOWN = -2, ACT_LEFT = -3, ACT_RIGHT = -4,
    ACT_FIRE = -5, SOFT1 = -6, SOFT2 = -7,
    ACT_GAME_A = -9, ACT_GAME_B = -10, ACT_GAME_C = -11, ACT_GAME_D = -12
};

/* Canvas.getGameAction() result codes — OFFICIAL JSR-118 values.
 * Games compare against Canvas.UP (=1), Canvas.LEFT (=2), Canvas.RIGHT
 * (=5), Canvas.DOWN (=6), Canvas.FIRE (=8), Canvas.GAME_A..D (=9..12).
 * The negative numbers above are the device *key codes* (what keyPressed
 * receives on Nokia/SE handsets) — do NOT return them from getGameAction. */
enum {
    GAME_UP = 1, GAME_LEFT = 2, GAME_RIGHT = 5, GAME_DOWN = 6, GAME_FIRE = 8,
    GAME_A = 9, GAME_B = 10, GAME_C = 11, GAME_D = 12
};

/* GameCanvas.getKeyStates() bits: 1 << (GAME_*). Verified JSR-118. */
enum {
    KS_UP    = 1 << GAME_UP,     /* 0x0002 */
    KS_LEFT  = 1 << GAME_LEFT,   /* 0x0004 */
    KS_RIGHT = 1 << GAME_RIGHT,  /* 0x0020 */
    KS_DOWN  = 1 << GAME_DOWN,   /* 0x0040 */
    KS_FIRE  = 1 << GAME_FIRE,   /* 0x0100 */
    KS_GAME_A = 1 << GAME_A, KS_GAME_B = 1 << GAME_B,
    KS_GAME_C = 1 << GAME_C, KS_GAME_D = 1 << GAME_D
};

/* midp_input.c */
void input_pump(void);                 /* once per frame, from sys_event */
int  input_get_key(int *keycode);      /* queued MIDP keyPressed, 0=none */
int  input_is_down(int midp_keycode);
int  input_get_game_action(int midp_keycode);  /* Canvas.getGameAction  */
int  input_get_key_states(void);       /* GameCanvas.getKeyStates: bits  */
                                       /* are latched & cleared on read  */
void input_key_released_flush(void);   /* synthesize keyReleased events  */
void midp_frame_pump(void);            /* input + media + event dispatch */
void midp_paint_pump(void);            /* frame_pump + repaint delivery,
                                          called from the interpreter's
                                          dispatch loop (liveness even
                                          when the game never sleeps) */
void input_reset(void);                /* drop held/queued key state     */

/* key-event bridges — weak no-ops here, REAL implementations in
 * natives.c dispatch into the Java canvas (keyPressed etc.)         */
void bridge_key_pressed(int midp) __attribute__((weak));
void bridge_key_released(int midp) __attribute__((weak));
void bridge_key_repeated(int midp) __attribute__((weak));
void bridge_command(int softkey) __attribute__((weak));

/* ----- Lifecycle (midp_input.c drives it) --------------------------- */
typedef struct {
    void (*start_app)(void *this_);
    void (*pause_app)(void *this_);
    void (*destroy_app)(void *this_, int unconditional);
} MidletVTable;

void midp_run(const char *jad_path);   /* loads jars, boots MIDlet loop */
void midp_paint_request(void);         /* Canvas.repaint() => flag      */
void fpj2me_pump(void);                /* natives.c: events + repaint    */

/* ---- boot / shutdown (interp.c) ------------------------------------ */
/* boots the MIDlet named by fpj2me_manifest_midlet() inside `jar`
 * (a .jar path relative to the working directory). Returns 0 on
 * success, negative error codes on failure (see target_main.c).     */
int  fpj2me_boot(const char *jar);
/* full VM reset so the launcher menu can boot the next jar: unpins
 * the canvas, drops input/media state, flushes RMS, frees tracked
 * images, closes the game jar and wipes heap/handles/classes/meta.  */
void fpj2me_shutdown(void);
/* manifest text of the ACTIVE suite (filled by fpj2me_boot, read by
 * MIDlet.getAppProperty) + the sibling .jad text (MIDP 2.0: overrides
 * manifest keys) + notifyDestroyed() handshake flag                 */
extern char fpj_manifest[2048];
extern char fpj_jad[2048];
extern int  fpj_midlet_done;
/* full property chain used by MIDlet.getAppProperty: sibling .jad ->
 * MANIFEST.MF -> Gameloft IGP/URL non-null fallbacks -> NULL.
 * Returns a shared static buffer (single-threaded VM) or NULL.      */
const char *fpj_app_property(const char *key);

/* ----- RMS ----------------------------------------------------------- */
/* midp_rms.c — see file header for the on-disk format.                */
int  rms_open(const char *suite, const char *name, int create);
int  rms_add(int rs, const void *data, int len);      /* -> record id   */
int  rms_get(int rs, int id, void **data, int *len);
int  rms_set(int rs, int id, const void *data, int len);
int  rms_delete(int rs, int id);
int  rms_enum(int rs, int *ids_out, int max);         /* sorted ids     */
void rms_close(int rs);
void rms_close_all(void);              /* shutdown: flush + close all     */

/* ----- Media (silent no-ops) ---------------------------------------- */
/* midp_media.c */
enum { PL_UNREALIZED = 100, PL_REALIZED = 200,
       PL_PREFETCHED = 300, PL_STARTED = 400, PL_CLOSED = 0 };
enum { EV_END_OF_MEDIA = 1, EV_STARTED = 2, EV_STOPPED = 3 };

void *media_create_player(const char *content_type);
int   media_player_state(void *player);
int   media_player_op(void *player, int op /*0 rel 1 pref 2 start 3 stop 4 close*/);
void  media_add_listener(void *player, void *listener_ref);
void  media_pump(void);   /* deliver queued END_OF_MEDIA to listeners   */
void  media_reset(void);  /* shutdown: drop player + queued events      */

/* ----- shutdown helpers (natives.c / midp_image.c) ------------------- */
void natives_reset(void);   /* unpin canvas, drop Display/repaint caches */
void image_free_all(void);  /* free tracked native image buffers        */

#endif
