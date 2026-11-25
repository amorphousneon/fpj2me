/*
 * fpj2me host check — compiles the bridge modules natively against
 * stubbed fp_* primitives and validates:
 *   1. zip central-directory scan + STORED entry reads (jar.c)
 *   2. drawRegion for all 8 TRANS_* transforms (midp_graphics.c)
 *   3. clip / fill / drawString sanity
 *   4. RecordStore log semantics incl. reopen (midp_rms.c)
 * Build: tests/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "../src/platform.h"
#include "../src/fpj_lcd.h"
#include "../src/fpj_key.h"
#include "../src/fpj2me.h"
#include "../src/jar.h"
#include "../src/midp.h"
#include "../src/fp_font.h"

/* ---- fpdoom HAL stubs ---------------------------------------------- */
uint16_t fp_screen[FP_LCD_W * FP_LCD_H];
int fp_rawmap[KP_MAX];
static uint32_t now_ms;
uint32_t fp_time_ms(void) { return now_ms; }
void fp_sleep_ms(uint32_t ms) { now_ms += ms; }
/* mirrors fp_platform.c on target: the diag strip renders on flush */
void fp_lcd_flush(void) { fpj_diag_overlay(); }
void fp_dbg(const char *s) { fputs(s, stderr); fputc('\n', stderr); }
uint32_t fp_keys_held(void) { return 0; }

/* scripted key events for the input-semantics tests */
static const struct { fp_key_t k; int down; } key_script[] = {
    { KP_LEFT, 1 }, { KP_LEFT, 0 },   /* tap: must latch in getKeyStates */
    { KP_5,    1 },                  /* digit 5 held: acts as FIRE      */
};
static int key_script_i;
int fp_key_event(fp_key_t *k, int *p) {
    if (key_script_i >= (int)(sizeof key_script / sizeof key_script[0]))
        return 0;
    *k = key_script[key_script_i].k;
    *p = key_script[key_script_i].down;
    key_script_i++;
    return 1;
}

fp_file *fp_fopen(const char *p, const char *m) { return (fp_file *)fopen(p, m); }
int fp_fread(void *b, int s, int n, fp_file *f) { return (int)fread(b, s, n, (FILE *)f); }
int fp_fwrite(const void *b, int s, int n, fp_file *f) { return (int)fwrite(b, s, n, (FILE *)f); }
int fp_fseek(fp_file *f, long o, int w) { return fseek((FILE *)f, o, w); }
long fp_ftell(fp_file *f) { return ftell((FILE *)f); }
int fp_fclose(fp_file *f) { return fclose((FILE *)f); }

/* libc shim impls routed to host libc / malloc */
void *fp_alloc(size_t n) { return malloc(n ? n : 1); }
void *fp_realloc(void *p, size_t n) { return realloc(p, n); }
void fp_free(void *p) { free(p); }
int fp_printf(const char *fmt, ...) { va_list a; va_start(a, fmt);
    int r = vprintf(fmt, a); va_end(a); return r; }
/* bridge_key_XXX and bridge_command defaults live in midp_input.c */

/* REAL vendors are linked now (vendor/miniz.c with tinfl, and
 * vendor/stb_impl.c with the PNG decoder) — see run.sh. */

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

static void test_jar(void)
{
    Jar j;
    CHECK(jar_open(&j, "test.jar") == 0, "jar_open");
    CHECK(j.n == 6, "jar: 6 entries indexed");

    u32 sz = 0;
    uint8_t *b = jar_read(&j, jar_find(&j, "a/b/Hello.class"), &sz);
    CHECK(b && sz == 8 && !memcmp(b, "CAFEBABE", 8), "jar: stored .class read");
    free(b);

    b = jar_read(&j, jar_find(&j, "res/sheet.png"), &sz);
    CHECK(b && sz == 4097 && b[0] == 0x89 && b[4096] == 0xFF, "jar: 4K png entry read");
    free(b);

    const JarEntry *e = jar_find_next(&j, "res/", ".png", 0);
    CHECK(e && !strcmp(j.names + e->name_off, "res/sheet.png"), "jar: prefix scan");
    CHECK(jar_find(&j, "nope.txt") == 0, "jar: miss returns NULL");
    CHECK(jar_find(&j, "d/deflate.bin") != 0, "jar: deflate entry indexed");
    b = jar_read(&j, jar_find(&j, "d/deflate.bin"), &sz);
    CHECK(b && sz == 33 && !memcmp(b, "deflated-content-deflated-content", 33),
          "jar: REAL tinfl inflate of deflate entry");
    free(b);
    jar_close(&j);
}

static void test_png(void)
{
    /* decode the blitter demo frame (a real PNG) through the same path
     * Image.createImage(byte[],off,len) uses on target                   */
    FILE *f = fopen("blitter_demo.png", "rb");
    CHECK(f != 0, "png: open demo asset");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return; }
    fclose(f);

    JImage *im = image_from_png(buf, (u32)n);
    free(buf);
    CHECK(im && im->w == FP_LCD_W && im->h == FP_LCD_H,
          "png: real stb decode to full-screen RGB565");
    if (!im) return;
    int lit = 0;
    for (int i = 0; i < im->w * im->h; i++) if (im->px[i]) lit++;
    CHECK(lit > 2000, "png: decoded pixels non-empty");
    image_free(im);
}

/* 2x2 source: A=0x0100 B=0x0200 C=0x0300 D=0x0400 (0x0000/0x0001 reserved) */
static JImage *mk2x2(void)
{
    JImage *im = image_create(2, 2);
    im->px[0] = 0x0100; im->px[1] = 0x0200;
    im->px[2] = 0x0300; im->px[3] = 0x0400;
    return im;
}

static JImage *mk4x4(void)
{
    JImage *im = image_create(4, 4);
    for (int i = 0; i < 16; i++) im->px[i] = (u16)(0x0100 + i);
    return im;
}

static void test_transforms(void)
{
    JImage *src = mk2x2(), *dst;
    MGraphics g;

    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_NONE, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->px[0]==0x0100 && dst->px[1]==0x0200 && dst->px[2]==0x0300 && dst->px[3]==0x0400,
          "TRANS_NONE identity");
    image_free(dst);

    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_MIRROR, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->px[0]==0x0200 && dst->px[1]==0x0100 && dst->px[2]==0x0400 && dst->px[3]==0x0300,
          "TRANS_MIRROR");
    image_free(dst);

    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_ROT90, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->px[0]==0x0300 && dst->px[1]==0x0100 && dst->px[2]==0x0400 && dst->px[3]==0x0200,
          "TRANS_ROT90 cw");
    image_free(dst);

    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_ROT270, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->px[0]==0x0200 && dst->px[1]==0x0400 && dst->px[2]==0x0100 && dst->px[3]==0x0300,
          "TRANS_ROT270");
    image_free(dst);

    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_MIRROR_ROT180, 0, 0, GA_TOP | GA_LEFT);
    /* mirror + rot180 == flip vertical: [[C,D],[A,B]]                  */
    CHECK(dst->px[0]==0x0300 && dst->px[1]==0x0400 && dst->px[2]==0x0100 && dst->px[3]==0x0200,
          "TRANS_MIRROR_ROT180 = flip vertical");
    image_free(dst);

    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_MIRROR_ROT270, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->px[0]==0x0100 && dst->px[1]==0x0300 && dst->px[2]==0x0200 && dst->px[3]==0x0400,
          "TRANS_MIRROR_ROT270 = transpose");
    image_free(dst);

    /* odd-sized rect, rotation must swap dims: 4x2 -> 2x4.
     * 90deg cw: dst top row = src bottom row.                          */
    JImage *s42 = image_create(4, 2);
    for (int i = 0; i < 8; i++) s42->px[i] = (u16)(0x0100 + i);
    dst = image_create(2, 4); gfx_reset(&g, dst);
    gfx_draw_region(&g, s42, 0, 0, 4, 2, TRANS_ROT90, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->w == 2 && dst->h == 4 &&
          dst->px[0]==0x0104 && dst->px[1]==0x0100 &&
          dst->px[6]==0x0107 && dst->px[7]==0x0103, "ROT90 on 4x2 dims+pixels");
    image_free(dst); image_free(s42);

    /* transparency: 0x0000 skipped */
    src->px[1] = PX_TRANS;
    dst = image_create(2, 2); gfx_reset(&g, dst);
    gfx_fill_rect(&g, 0, 0, 2, 2);          /* fill color 0xFFFF default */
    gfx_draw_region(&g, src, 0, 0, 2, 2, TRANS_NONE, 0, 0, GA_TOP | GA_LEFT);
    CHECK(dst->px[1] == 0xFFFF, "transparent pixel skipped");
    image_free(dst);
    image_free(src);
}


/* --------------------------------------------------------------------
 * AC1 font-engine regressions (2026-11-25 (3)):
 * (1) setClip must REPLACE the clip (JSR-118), not intersect: GLLib
 *     draws every glyph as setClip(cell)+drawImage(sheet,offset) and
 *     with intersect semantics the 2nd..Nth glyph clip was empty, so
 *     only the first glyph of every drawString rasterized — the
 *     "DO YOU WANT SOUND?" screen showed a single 'Y' and looked
 *     frozen while the game was actually waiting for a key.
 * (2) setColor(0)+fillRect must be OPAQUE black (0x0001), not the
 *     PX_TRANS color-key 0x0000 — otherwise a cleared mutable image
 *     composites over stale frames when blitted (AC1 copyright text
 *     bleeding through the sound prompt).
 * (3) mutable images start WHITE per JSR-118 ("blank images
 *     containing only white pixels"), not transparent-zero.
 * ------------------------------------------------------------------ */
static void test_setclip_replace_black(void)
{
    /* (1) two glyph cells in a row: cell2's setClip(2,0,2,2) after
     * cell1's setClip(0,0,2,2) must still admit pixels.            */
    JImage *sheet = image_create(4, 2);
    for (int i = 0; i < 8; i++) sheet->px[i] = (u16)(0xF100 + i);
    JImage *dst = image_create(4, 2);
    MGraphics g; gfx_reset(&g, dst);
    gfx_clip(&g, 0, 0, 2, 2);                      /* glyph 1 cell   */
    gfx_draw_image(&g, sheet, 0, 0, GA_TOP | GA_LEFT);
    gfx_clip(&g, 2, 0, 2, 2);                      /* glyph 2 cell   */
    gfx_draw_image(&g, sheet, 0, 0, GA_TOP | GA_LEFT);
    int g1 =  dst->px[0] >= 0xF100 && dst->px[1] >= 0xF100 &&
              dst->px[4] >= 0xF100 && dst->px[5] >= 0xF100;
    int g2 =  dst->px[2] >= 0xF100 && dst->px[3] >= 0xF100 &&
              dst->px[6] >= 0xF100 && dst->px[7] >= 0xF100;
    CHECK(g1, "setClip REPLACE: glyph 1 cell rasterized");
    CHECK(g2, "setClip REPLACE: 2nd glyph cell rasterized (AC1 font fix)");
    image_free(sheet); image_free(dst);

    /* (2) black fill is opaque, so blits REPLACE instead of composite */
    dst = image_create(8, 8);
    for (int i = 0; i < 64; i++) dst->px[i] = 0xF800;   /* stale red */
    gfx_reset(&g, dst);
    gfx_color_rgb(&g, 0, 0, 0);
    gfx_fill_rect(&g, 0, 0, 8, 8);
    int opaque = 1;
    for (int i = 0; i < 64; i++) if (dst->px[i] != PX_BLACK) opaque = 0;
    CHECK(opaque, "setColor(0)+fillRect = OPAQUE black (0x0001)");
    /* and it erases when used as a blit source: clear a red dst    */
    JImage *red = image_create(8, 8);
    for (int i = 0; i < 64; i++) red->px[i] = 0xF800;
    gfx_reset(&g, red);
    gfx_draw_image(&g, dst, 0, 0, GA_TOP | GA_LEFT);
    int erased = 1;
    for (int i = 0; i < 64; i++) if (red->px[i] != PX_BLACK) erased = 0;
    CHECK(erased, "opaque-black image blit REPLACES target (no bleed)");
    image_free(dst); image_free(red);

    /* (3) mutable images start white (JSR-118)                      */
    dst = image_create(6, 6);
    int white = 1;
    for (int i = 0; i < 36; i++) if (dst->px[i] != 0xFFFF) white = 0;
    CHECK(white, "mutable image starts WHITE (JSR-118 blank)");
    image_free(dst);
}

static void test_clip_fill_string(void)
{
    JImage *dst = mk4x4();
    MGraphics g; gfx_reset(&g, dst);
    gfx_color_rgb(&g, 255, 0, 0);
    gfx_clip(&g, 1, 1, 2, 2);               /* interior 2x2              */
    gfx_fill_rect(&g, -4, -4, 12, 12);      /* overdraw: must stay in clip */
    int ok = 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            u16 want = (x >= 1 && x < 3 && y >= 1 && y < 3) ? 0xF800
                                                         : (u16)(0x0100 + y*4 + x);
            if (dst->px[y*4+x] != want) ok = 0;
        }
    CHECK(ok, "setClip + fillRect clipping");
    image_free(dst);

    dst = image_create(64, 16); gfx_reset(&g, dst);
    gfx_draw_string(&g, "fpj2me", 0, 0, GA_TOP | GA_LEFT);
    int lit = 0;
    for (int i = 0; i < 64*16; i++) if (dst->px[i]) lit++;
    CHECK(lit > 40, "drawString renders glyphs");
    image_free(dst);
}

static void test_rms(void)
{
    remove("/tmp/fpj2me/rms/tests/Save.rms");
    int rs = rms_open("tests", "Save", 1);
    CHECK(rs >= 0, "rms_open create");

    int id1 = rms_add(rs, "hello", 5);
    int id2 = rms_add(rs, "world!", 6);
    CHECK(id1 == 1 && id2 == 2, "rms: ids monotonic");

    char *data; int len;
    CHECK(rms_get(rs, id1, (void **)&data, &len) == 0 &&
          len == 5 && !memcmp(data, "hello", 5), "rms_get after add (no reopen)");
    free(data);

    CHECK(rms_set(rs, id1, "HELLO", 5) == 0, "rms_set");
    CHECK(rms_get(rs, id1, (void **)&data, &len) == 0 &&
          !memcmp(data, "HELLO", 5), "rms_get after set");
    free(data);

    int ids[16];
    int n = rms_enum(rs, ids, 16);
    CHECK(n == 2 && ids[0] == 1 && ids[1] == 2, "rms_enum sorted");

    rms_close(rs);

    rs = rms_open("tests", "Save", 0);
    CHECK(rs >= 0, "rms reopen");
    CHECK(rms_get(rs, id1, (void **)&data, &len) == 0 &&
          len == 5 && !memcmp(data, "HELLO", 5), "rms: set survives reopen (log semantics)");
    free(data);
    CHECK(rms_get(rs, id2, (void **)&data, &len) == 0 &&
          len == 6 && !memcmp(data, "world!", 6), "rms: add survives reopen");
    free(data);

    CHECK(rms_delete(rs, id1) == 0, "rms_delete");
    CHECK(rms_get(rs, id1, (void **)&data, &len) != 0, "deleted record gone");
    rms_close(rs);
}

/* ---- VM: classfile parser + interpreter + native dispatch -------- */
static void test_vm(void)
{
    Slot ret[2] = {{0}, {0}};
    /* drain key events queued by test_input while NO canvas exists,
     * so they cannot leak into the paint-cycle checks below           */
    fpj2me_pump();
    /* boot FIRST: it opens the classpath jars (a class_load before the
     * jars are open would cache a native-only stub and stay broken)   */
    CHECK(fpj2me_boot("game.jar") == 0,
          "VM: jar manifest -> MIDlet-1 -> class_load -> startApp");

    /* ---- the AC1 freeze regression --------------------------------
     * The test midlet is Gameloft-shaped: startApp() BLOCKS inside
     * DemoScr.run() (repaint + serviceRepaints + Thread.sleep loops)
     * and only returns when paint() has been delivered 6 times —
     * fpj2me_boot() returning 0 already proves the loop completed.
     * The statics tell HOW: painted==6 (3 per delivery path), spins
     * and spins2 far below the 80-iteration timeout. Before the fix
     * the loops burned all 80 spins with painted==0 (paint starved). */
    JClass *ds = class_load("DemoScr");
    int painted = -1, spins = -1, spins2 = -1, clinit_cl = -1;
    for (u16 i = 0; ds && i < ds->n_fields; i++) {
        if (!strcmp(ds->fields[i].name, "painted"))
            painted = (s32)ds->fields[i].static_val;
        else if (!strcmp(ds->fields[i].name, "spins"))
            spins = (s32)ds->fields[i].static_val;
        else if (!strcmp(ds->fields[i].name, "spins2"))
            spins2 = (s32)ds->fields[i].static_val;
        else if (!strcmp(ds->fields[i].name, "cl"))
            clinit_cl = (s32)ds->fields[i].static_val;
    }
    CHECK(painted == 6,
          "VM: blocking startApp got 6 paints (serviceRepaints + sleep)");
    CHECK(spins >= 0 && spins < 80,
          "VM: serviceRepaints delivered paint inside startApp");
    CHECK(spins2 >= 0 && spins2 < 80,
          "VM: pumping Thread.sleep delivered repaints");
    CHECK(clinit_cl == 9,
          "VM: <clinit> ran at 'new' (first active use, DemoScr.cl == 9)");

    /* missing library native: no panic, zeroed no-op return (the old
     * behavior was fpj_panic = silent green freeze on the phone)   */
    CHECK(fpj_call_static("Tst", "run3", "()I", ret) && (s32)ret[0].w == 0,
          "VM: missing native degrades to no-op (no panic)");

    CHECK(fpj_call_static("Tst", "run", "()I", ret) && (s32)ret[0].w == 5,
          "VM: parse Tst.class, exec consts/iadd/putstatic/getstatic");
    CHECK(fpj_call_static("Tst", "run2", "()I", ret) && (s32)ret[0].w == 5,
          "VM: native dispatch via stub class (Math.abs(-5))");
    CHECK(fpj_call_static("Tst", "run4", "()I", ret) && (s32)ret[0].w == 41,
          "VM: <clinit> initialized static y == 41");
    CHECK(fpj_call_static("Tst", "run5", "()I", ret) && (s32)ret[0].w == 1,
          "VM: lcmp operand order (100L > 50L)");

    /* M1 visible smoke test — the same classfiles the phone runs.
     * startApp() did Display.getDisplay(this).setCurrent(new DemoScr())
     * and already drew through the blocking loop; force one more
     * frame through the post-boot pump path and re-verify it.       */
    memset(fp_screen, 0, sizeof fp_screen);
    midp_paint_request();
    fpj2me_pump();                              /* pump-driven paint   */
    CHECK(fp_screen[30 * 128 + 48] == 0x0660,   /* RGB565(0x00CC00)     */
          "VM: paint() drew the green box");
    /* 0x0001 = PX_BLACK: setColor(0)+fillRect is OPAQUE black now.
     * It used to write 0x0000, which the blitter treats as the
     * transparent color-key — a mutable image "cleared" that way
     * composited over stale frames instead of replacing them (the
     * AC1 layering bug: copyright text bleeding through the sound
     * prompt). 0x0000 would still be "transparent", not "black".  */
    CHECK(fp_screen[2 * 128 + 2] == 0x0001,
          "VM: paint() cleared to OPAQUE black (0x0001, not PX_TRANS)");
    CHECK(fp_screen[0] == 0xFFFF,
          "VM: drawRect() white border");
    bridge_key_pressed(-5);                     /* FIRE -> keyPressed   */
    fpj2me_pump();                              /* repaint              */
    CHECK(fp_screen[30 * 128 + 48] == 0xC800,   /* RGB565(0xCC0000)     */
          "VM: keyPressed -> repaint (box red)");

    /* RSOFT exits the smoke midlet: keyPressed(-7) -> Tst.inst.
     * notifyDestroyed() -> the launcher's exit flag (menu returns)    */
    bridge_key_pressed(SOFT2);
    CHECK(fpj_midlet_done == 1,
          "VM: notifyDestroyed -> launcher exit flag");

    /* launcher menu flow: midlet exits -> fpj2me_shutdown() wipes the
     * VM -> booting the SAME jar again must re-parse, re-allocate and
     * re-paint (what fp_menu.c + target_main.c do on the phone)       */
    fpj2me_shutdown();
    CHECK(fpj2me_boot("game.jar") == 0,
          "VM: re-boot after full shutdown (menu return)");
    memset(fp_screen, 0, sizeof fp_screen);
    midp_paint_request();
    fpj2me_pump();
    CHECK(fp_screen[30 * 128 + 48] == 0x0660,
          "VM: paint() after re-boot");
    CHECK(fpj_manifest[0] != 0 &&
          strstr(fpj_manifest, "MIDlet-1") != NULL,
          "VM: manifest cached for getAppProperty");
}

/* ---- MIDP 2.0 property chain (.jad + manifest + IGP fallback) ------ *
 * run.sh planted a sibling game.jad next to game.jar (loaded during
 * the boot in test_vm): it overrides manifest keys, folds one value
 * over an RFC-822 continuation line and mixes CRLF/LF endings.     */
static void test_app_property(void)
{
    const char *v;
    v = fpj_app_property("MIDlet-Name");
    CHECK(v && !strcmp(v, "JAD-Name"),
          "props: sibling .jad overrides MANIFEST.MF key");
    v = fpj_app_property("MIDlet-Version");
    CHECK(v && !strcmp(v, "1.0"),
          "props: absent in .jad -> manifest fallback");
    v = fpj_app_property("midlet-name");
    CHECK(v && !strcmp(v, "JAD-Name"),
          "props: case-insensitive key lookup");
    v = fpj_app_property("X-Long-Value");
    CHECK(v && !strcmp(v, "start continued-end"),
          "props: RFC-822 continuation unfolded (CRLF+LF mix)");
    v = fpj_app_property("X-Empty");
    CHECK(v && !*v,
          "props: present-but-empty value -> empty string");

    /* Gameloft IGP/carrier fallbacks: the keys AC1's GLLib probes that
     * are in NEITHER .jad nor manifest must come back non-null — the
     * old null returns made GLLib NPE on .trim()/.toUpperCase() and
     * stall in a catch-loop right after the Ubisoft splash           */
    v = fpj_app_property("URL-TEMPLATE-GAME");
    CHECK(v && !strcmp(v, "http://localhost/"),
          "props: URL-TEMPLATE-GAME non-null fallback (GLLib NPE fix)");
    v = fpj_app_property("URL-OPERATOR");
    CHECK(v && !strcmp(v, "http://localhost/"),
          "props: URL-OPERATOR non-null fallback");
    v = fpj_app_property("URL-PT");
    CHECK(v && !strcmp(v, "0"),
          "props: URL-PT non-null fallback");
    v = fpj_app_property("IGP-ANYTHING");
    CHECK(v && !*v, "props: IGP-* -> empty string (not null)");
    v = fpj_app_property("URL-WHATEVER");
    CHECK(v && !*v, "props: URL-* -> empty string (not null)");

    /* genuinely unknown keys stay null: MIDP semantics preserved     */
    CHECK(fpj_app_property("NoSuch-Key") == 0,
          "props: unknown key -> null (MIDP semantics intact)");

    /* raw sibling loader: found / absent                             */
    char buf[64];
    CHECK(jad_load_sibling("game.jar", buf, sizeof buf) == 0 && buf[0] != 0,
          "props: jad_load_sibling finds the .jad");
    CHECK(jad_load_sibling("boot.jar", buf, sizeof buf) == -1,
          "props: jad_load_sibling returns -1 when absent");

    /* shutdown wipes the .jad together with the manifest             */
    fpj2me_shutdown();
    CHECK(fpj_app_property("MIDlet-Name") == 0,
          "props: shutdown clears .jad+manifest (no stale keys)");
}

/* ---- launcher font (fp_menu.c / target_main.c screens) ------------- */
static void test_font(void)
{
    memset(fp_screen, 0, sizeof fp_screen);
    fp_draw_text(0, 0, "fpj2me", 0xFFFF);
    int lit = 0;
    for (int i = 0; i < 128 * 16; i++) if (fp_screen[i]) lit++;
    CHECK(lit > 60, "font: fp_draw_text renders glyphs");
    fp_draw_text_row(0, 2, "sel", 0x0000, 0xFFFF);
    CHECK(fp_screen[16 * 128] == 0xFFFF,      /* row 2, col 0 filled   */
          "font: selection row fills");
}

/* ---- GC ----------------------------------------------------------- */
static void test_gc(void)
{
    u32 f0 = heap_free_bytes();
    for (int i = 0; i < 500; i++) ref_alloc(T_ARR_B, 64, 0);   /* garbage */
    gc_run();
    CHECK(heap_free_bytes() >= f0 - 4096, "GC: garbage reclaimed (mark&sweep)");

    JRef keep = ref_alloc(T_ARR_A, 4 + 4 * 8, 0);
    JObj *ko = obj(keep);
    CHECK(ko != 0, "GC: alloc array");
    if (ko) ARR_LEN(ko) = 8;
    gc_pin(keep);
    for (int i = 0; i < 500; i++) ref_alloc(T_ARR_B, 64, 0);
    gc_run();
    ko = obj(keep);
    CHECK(ko && ARR_LEN(ko) == 8, "GC: pinned object survives collections");
    gc_unpin(1);
    gc_run();
    CHECK(obj(keep) == 0, "GC: unpinned object collected");
}

static void test_input(void)
{
    /* Canvas.getGameAction MUST return JSR-118 action values — javac
     * inlined Canvas.UP==1 etc. into every 2007+ game jar.            */
    CHECK(input_get_game_action(ACT_UP)    == GAME_UP   &&
          input_get_game_action(ACT_DOWN)  == GAME_DOWN &&
          input_get_game_action(ACT_LEFT)  == GAME_LEFT &&
          input_get_game_action(ACT_RIGHT) == GAME_RIGHT &&
          input_get_game_action(ACT_FIRE)  == GAME_FIRE,
          "getGameAction: nav keycodes -> official values");
    CHECK(input_get_game_action(KEY_NUM2) == GAME_UP &&
          input_get_game_action(KEY_NUM4) == GAME_LEFT &&
          input_get_game_action(KEY_NUM6) == GAME_RIGHT &&
          input_get_game_action(KEY_NUM8) == GAME_DOWN &&
          input_get_game_action(KEY_NUM5) == GAME_FIRE,
          "getGameAction: digits double as nav pad");
    CHECK(input_get_game_action(SOFT1) == 0 &&
          input_get_game_action(KEY_STAR) == 0,
          "getGameAction: soft/star have no action");

    input_pump();   /* drains the script: LEFT tap, 5 held             */
    CHECK(fpj_key_events >= 2, "vitals: key events counted in the queue");
    int ks = input_get_key_states();
    CHECK(ks == (KS_LEFT | KS_FIRE),
          "getKeyStates: tap latched + held digit-5 acts as FIRE");
    CHECK(input_get_key_states() == KS_FIRE,
          "getKeyStates: latch cleared, held key still reported");
}

/* ---- fpj2me fatal-screen + diag vitals (target issue 3) ------------ */
static int fatal_hook_hits;
void fpj_on_fatal(void)                    /* strong: overrides the     */
{ fatal_hook_hits++; }                     /* exit(3) default in natives */

static int any_pixel(u16 c, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < FP_LCD_W; x++)
            if (fp_screen[y * FP_LCD_W + x] == c) return 1;
    return 0;
}

static void test_fatal_diag(void)
{
    /* diag overlay: throttle uses fp_time_ms (host: now_ms), vitals
     * counters must all be live and the strip must actually paint    */
    fpj_diag = 1;
    now_ms = 1000;                   /* > 500 ms since a fresh static 0  */
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0x07E0);   /* green field   */
    fpj_diag_overlay();
    CHECK(fp_screen[0] == 0x0000, "diag: vitals strip cleared to black");
    CHECK(any_pixel(0x07FF, 0, 5 * FP_FONT_H) || any_pixel(0xFFE0, 0, 5 * FP_FONT_H),
          "diag: vitals text drawn (cyan/yellow)");

    /* M: no longer clips at the screen edge: a long missing-native
     * name wraps onto a 6th row. The old 14-char clip showed only
     * "com/nokia/mid/" and hid the method — the exact detail that
     * names the stall (Sound.getState waited out the audio timeout) */
    now_ms += 1000;                       /* beat the 500 ms throttle  */
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0x07E0);
    strcpy(fpj_miss_native, "com/nokia/mid/sound/Sound.getState");
    fp_lcd_flush();                  /* target contract: overlay on flush */
    CHECK(any_pixel(0xF800, 4 * FP_FONT_H, 5 * FP_FONT_H),
          "diag: M: row 4 drawn");
    CHECK(any_pixel(0xF800, 5 * FP_FONT_H, 6 * FP_FONT_H),
          "diag: M: wraps onto row 5 (full Class.method)");
    CHECK(fp_screen[(6 * FP_FONT_H) * FP_LCD_W + 64] == 0x07E0,
          "diag: 6th row only used when M: wraps");
    fpj_miss_native[0] = 0;

    /* short M: stays on row 4, strip stays 5 rows                    */
    now_ms += 1000;
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0x07E0);
    strcpy(fpj_miss_native, "a.b");
    fp_lcd_flush();
    CHECK(any_pixel(0xF800, 4 * FP_FONT_H, 5 * FP_FONT_H) &&
          !any_pixel(0xF800, 5 * FP_FONT_H, 6 * FP_FONT_H),
          "diag: short M: single row");
    fpj_miss_native[0] = 0;
    fpj_diag = 0;

    /* fatal screen: full class name + message + vitals on red, the
     * pending exception consumed, the hook fired (host override)     */
    fatal_hook_hits = 0;
    fpj_pending_exc = fpj_new_exception("java/lang/IllegalStateException",
                                        "boom 42");
    strcpy(fpj_miss_native, "java/lang/Object.wait");
    fpj_fatal("in unit test");
    CHECK(fatal_hook_hits == 1, "fatal: fpj_on_fatal hook fired");
    CHECK(fp_screen[80 * FP_LCD_W + 64] == 0xF800, "fatal: red screen");
    CHECK(any_pixel(0xFFFF, 3 * FP_FONT_H, 7 * FP_FONT_H),
          "fatal: exception class + message drawn");
    CHECK(fpj_pending_exc == 0, "fatal: pending exception consumed");
    fpj_miss_native[0] = 0;

    /* OOME singleton is armed at boot (fpj2me_boot ran in test_vm)  */
    CHECK(fpj_oome != 0 && fpj_vcpu > 0,
          "vitals: OOME singleton armed + VM dispatch counter live");
}

/* ---- Nokia Sound / DeviceControl (the splash-stall fix) -------------- */
static void test_nokia_sound(void)
{
    const NativeEntry *ne;
    Slot a[2], r[2];
    memset(a, 0, sizeof a);
    memset(r, 0, sizeof r);

    /* THE fix: getState() must report 1 (SOUND_STOPPED). The no-op
     * fallback returned 0 == SOUND_PLAYING, and GLLib's splash state
     * machine waited out a full audio timeout per transition — the
     * 2.1 s/frame / 0.47 FPS reading on the it2163.                 */
    ne = native_lookup("com/nokia/mid/sound/Sound", "getState", "()I");
    CHECK(ne != 0, "sound: Sound.getState()I registered");
    if (ne) {
        ne->fn(a, 0, r);
        CHECK(r[0].w == 1,
              "sound: getState -> 1 SOUND_STOPPED (never 0 PLAYING)");
    }

    CHECK(native_lookup("com/nokia/mid/sound/Sound", "<init>", "([BI)V") &&
          native_lookup("com/nokia/mid/sound/Sound", "init", "(I)V") &&
          native_lookup("com/nokia/mid/sound/Sound", "play", "()V") &&
          native_lookup("com/nokia/mid/sound/Sound", "stop", "()V") &&
          native_lookup("com/nokia/mid/sound/Sound", "release", "()V") &&
          native_lookup("com/nokia/mid/sound/Sound", "resume", "()V") &&
          native_lookup("com/nokia/mid/sound/Sound", "setGain", "(I)V") &&
          native_lookup("com/nokia/mid/sound/Sound", "getGain", "()I") &&
          native_lookup("com/nokia/mid/sound/Sound", "setSoundListener",
                        "(Lcom/nokia/mid/sound/SoundListener;)V"),
          "sound: full Sound surface stubbed (no M: no-op fallback)");

    CHECK(native_lookup("com/nokia/mid/ui/DeviceControl", "setLights", "(II)V") &&
          native_lookup("com/nokia/mid/ui/DeviceControl", "startVibra", "(IJ)V") &&
          native_lookup("com/nokia/mid/ui/DeviceControl", "stopVibra", "()V"),
          "device: DeviceControl lights/vibra stubbed");

    CHECK(native_lookup("com/nokia/mid/sound/Sound", "getState", "()Z") == 0,
          "sound: exact-signature lookup intact (no over-stubbing)");
}

/* ---- 128x160 native-portrait LCD scanout (fpj_lcd.h) ------------------
 * The on-phone bug: fpdoom's shared 160x128 landscape scan config
 * (fpmenu's stock "$@ = $@ --rotate 3" -> MADCTL XOR 0x60 -> MV set,
 * CASET 0..159 / RASET 0..127) receiving our 128x160 portrait pitch-128
 * framebuffer: 128-vs-160 stride shear, a wandering split black bar
 * with wrap-around, and a sideways picture. The plan must rebuild the
 * panel's NATIVE portrait window from whatever state fpdoom left.    */
static void test_lcd_portrait(void)
{
    FpjLcdPlan p, p2;

    /* 1. THE it2163 state: --rotate 3 (rtab[3]=0x60), itel ST7735S
     *    table mac 0xd0 -> programmed mac 0xB0 (MY|MV|ML), w1/h1
     *    swapped to 160x128 landscape                                    */
    memset(&p, 0, sizeof p);
    p.mac = 0xB0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
    CHECK(fpj_lcd_plan(&p) == 1, "lcd: it2163 rotate-3 landscape reprogrammed");
    CHECK(p.madctl == 0xD0,
          "lcd: portrait MADCTL = panel base 0xD0 (MV cleared, MY/MX/ML kept)");
    CHECK(p.caset[0] == 0 && p.caset[1] == 0 && p.caset[2] == 0 && p.caset[3] == 127,
          "lcd: CASET 0x2A -> 0..127 (128 columns)");
    CHECK(p.raset[0] == 0 && p.raset[1] == 0 && p.raset[2] == 0 && p.raset[3] == 159,
          "lcd: RASET 0x2B -> 0..159 (160 rows)");
    CHECK(p.pw == 128 && p.ph == 160 && p.fw == 128 && p.fh == 160,
          "lcd: sys_data patched to 128x160 (window=fb=DMA 1:1)");
    CHECK((p.caset[3] - p.caset[1] + 1) * (p.raset[3] - p.raset[1] + 1)
              == (unsigned)FP_LCD_W * FP_LCD_H,
          "lcd: GRAM window area == 20480 == 128*160*2-byte transfer");

    /* 2. --rotate 1 (rtab[1]=0xa0): mac 0xd0^0xa0=0x70 (MX|MV|ML)      */
    memset(&p, 0, sizeof p);
    p.mac = 0x70; p.rotate = 1; p.w1 = 160; p.h1 = 128;
    CHECK(fpj_lcd_plan(&p) == 1 && p.madctl == 0xD0 &&
          p.caset[3] == 127 && p.raset[3] == 159,
          "lcd: rotate-1 landscape reprogrammed identically");

    /* 3. --rotate 2 (rtab[2]=0xc0): 180-degree portrait, no MV swap    */
    memset(&p, 0, sizeof p);
    p.mac = 0x10; p.rotate = 2; p.w1 = 128; p.h1 = 160;
    CHECK(fpj_lcd_plan(&p) == 1 && p.madctl == 0xD0,
          "lcd: rotate-2 (180) flipped back to native portrait");

    /* 4. rotate-0 native portrait (mac 0xd0 as the itel table programs
     *    it): plan is IDEMPOTENT — same commands, no behavior change   */
    memset(&p, 0, sizeof p);
    p.mac = 0xD0; p.rotate = 0; p.w1 = 128; p.h1 = 160;
    CHECK(fpj_lcd_plan(&p) == 1 && p.madctl == 0xD0 &&
          p.caset[3] == 127 && p.raset[3] == 159 && p.pw == 128 && p.ph == 160,
          "lcd: native portrait passes through unchanged (idempotent)");

    /* 5. meanIT-style GC9106 (table mac 0x08, RGB-order bit only) under
     *    --rotate 3: current mac = 0x08 ^ 0x60 = 0x68 (MX|MV|RGB)      */
    memset(&p, 0, sizeof p);
    p.mac = 0x68; p.rotate = 3; p.w1 = 160; p.h1 = 128;
    CHECK(fpj_lcd_plan(&p) == 1 && p.madctl == 0x08,
          "lcd: GC9106 RGB-order bit preserved through the switch");

    /* 6. panel GRAM offsets survive (132x162 glass modules: +2/+1)     */
    memset(&p, 0, sizeof p);
    p.mac = 0xB0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
    p.xoff = 2; p.yoff = 1;
    CHECK(fpj_lcd_plan(&p) == 1 &&
          p.caset[1] == 2 && p.caset[3] == 129 &&
          p.raset[1] == 1 && p.raset[3] == 160,
          "lcd: portrait GRAM offsets applied (CASET 2..129, RASET 1..160)");

    /* 7. monochrome / --mac override (0x100 flag): never touched       */
    memset(&p, 0, sizeof p);
    p.mac = 0x1B0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
    CHECK(fpj_lcd_plan(&p) == 0,
          "lcd: mono/--mac panel left to fpdoom (letterbox fallback)");

    /* 8. a 240x320 panel is not our 128x160 phone: untouched           */
    memset(&p, 0, sizeof p);
    p.mac = 0x48; p.rotate = 0; p.w1 = 240; p.h1 = 320;
    CHECK(fpj_lcd_plan(&p) == 0,
          "lcd: 240x320 panel left to fpdoom (letterbox fallback)");

    /* 9. the plan is STATELESS: re-running it (the per-flush window
     *    re-pin in fp_lcd_flush) yields the identical window, so the
     *    re-issue itself can never drift                              */
    memset(&p, 0, sizeof p);
    p.mac = 0xB0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
    fpj_lcd_plan(&p);
    p2 = p;
    p.mac = 0xB0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
    fpj_lcd_plan(&p2);
    CHECK(p.reprogram == 1 && p2.reprogram == 1 &&
          p.madctl == p2.madctl &&
          !memcmp(p.caset, p2.caset, sizeof p.caset) &&
          !memcmp(p.raset, p2.raset, sizeof p.raset) &&
          p.pw == p2.pw && p.ph == p2.ph && p.fw == p2.fw && p.fh == p2.fh,
          "lcd: flush-time re-pin is stable (clean flush state)");

    /* 10. framebuffer layout 1:1 with the portrait scanout:
     *     fp_screen is EXACTLY 128*160 RGB565 = 40,960 bytes, row
     *     stride 128 px = 256 bytes, pixel (x,y) at fp_screen[y*128+x].
     *     These are the constants nat_Canvas_getWidth/getHeight and
     *     every blitter use.                                            */
    CHECK(FP_LCD_W == 128 && FP_LCD_H == 160,
          "lcd: Canvas.getWidth()/getHeight() == 128x160");
    CHECK(sizeof fp_screen == (size_t)FP_LCD_W * FP_LCD_H * 2,
          "lcd: fp_screen is 40,960 bytes (exact 128*160*2 DMA length)");
    CHECK(sizeof(fp_screen[0]) == 2 && (size_t)FP_LCD_W * 2 == 256,
          "lcd: RGB565 row stride 256 bytes");
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0x0000);
    fp_fill_rect(10, 20, 3, 2, 0xF800);            /* (10..12, 20..21) */
    CHECK(fp_screen[20 * FP_LCD_W + 10] == 0xF800 &&
          fp_screen[20 * FP_LCD_W + 12] == 0xF800 &&
          fp_screen[21 * FP_LCD_W + 11] == 0xF800,
          "lcd: fill lands row-major at fp_screen[y*128+x]");
    CHECK(fp_screen[19 * FP_LCD_W + 10] == 0x0000 &&
          fp_screen[22 * FP_LCD_W + 10] == 0x0000 &&
          fp_screen[20 * FP_LCD_W + 13] == 0x0000,
          "lcd: fill bounds exact (no wrap onto neighbor rows)");

    /* 11. flush path leaves the portrait layout intact: the host stub
     *     mirrors the target fp_lcd_flush() order (overlay, then the
     *     window re-pin which is a no-op on host). A marker outside
     *     the 6-row diag band must survive a flush untouched.         */
    fp_lcd_flush();
    CHECK(fp_screen[21 * FP_LCD_W + 11] == 0xF800,
          "lcd: flush (overlay+re-pin) does not disturb the frame");
}

/* ---- per-flush PROGRAM (fpj_lcd.h fpj_lcd_flush_seq) --------------------
 * 2026-11-25 (7). THE ROUND-6 LESSON (real itel i2163): rounds (5)/(6)
 * kept the per-frame CASET/RASET/RAMWR re-pin and only re-ordered its
 * gates — (5)'s FIFO/BUSY gate left a constant 544-word wrap
 * ("MISSION FAILE|D"), (6)'s TX_END count-closed gate SHIFTED it
 * ("YOU WANT TO|DO"). The gates were never the problem: the per-frame
 * register banging ITSELF desynchronizes the pixel count (the burst
 * toggles chnl_len/CD/ctl8/ctl9 on the SPI the LCDC capture stream
 * uses, while sys_wait_refresh() early-returns because lcdc_init()
 * masked LCDC irq.en). Stock fpdoom/fpmenu drive this exact panel
 * wrap-free by programming the window ONCE at boot (page_reset==0,
 * forced on SPI by lcm_init()) and never touching a panel register
 * per frame: the 20480-word stream == the window area, so the GRAM
 * write pointer wraps modulo 20480 back to (0,0) on its own.
 * (7) pins that program: the DEFAULT is LCDC_WAIT -> SUBMIT (the one
 * gate that stays is the REAL frame-completion wait: poll the RAW
 * irq bit that latches regardless of the irq.en mask). The (6) full
 * re-pin survives only behind gram_repin=1 (config.txt
 * "-- --gram-repin"), for a parallel-LCM GC9106-class panel.       */
static void test_lcd_flush_gate(void)
{
    int seq[FPJ_LCD_SEQ_MAX];
    int n;

    /* 1. THE it2163 case (SPI panel, portrait applied, DEFAULT flags):
     *    exactly LCDC_WAIT -> SUBMIT. NO commands, NO TX_END, NO bus
     *    toggles — the stock page_reset==0 flow, byte for byte the
     *    sequence that keeps fpmenu wrap-free on this same panel.   */
    n = fpj_lcd_flush_seq(1, 1, 0, 0, seq);
    CHECK(n == 2 && seq[0] == FPJ_FS_LCDC_WAIT && seq[1] == FPJ_FS_SUBMIT,
          "lcd-gate: DEFAULT SPI portrait = LCDC_WAIT,SUBMIT (stock flow)");

    /* 2. THE (4)/(5)/(6) BUG SHAPE, pinned as the regression guard:
     *    any FPJ_FS_CMD_* / TX_END / BUS_IDLE step in the DEFAULT
     *    program is the per-frame register banging that desynchronized
     *    the pixel count on the real panel. The fixed default (above)
     *    contains NONE of them — the 20480-word stream wraps the GRAM
     *    window modulo 20480 back to (0,0) without any help.         */
    {
        int i, banging = 0;
        for (i = 0; i < n; i++)
            if (seq[i] == FPJ_FS_CMD_CASET || seq[i] == FPJ_FS_CMD_RASET ||
                seq[i] == FPJ_FS_CMD_RAMWR || seq[i] == FPJ_FS_TX_END ||
                seq[i] == FPJ_FS_BUS_IDLE)
                banging = 1;
        CHECK(banging == 0,
              "lcd-gate: DEFAULT program has zero per-frame register steps");
    }

    /* 3. THE MATH that makes the no-re-pin flow correct: the DMA word
     *    count (w1*h1, what spi_refresh_next() arms) == the GRAM
     *    window area (what CASET/RASET set once at boot) — so the
     *    write pointer returns to (0,0) exactly when the stream ends.
     *    (fpj_lcd_plan test 1 pins the values; this pins the identity
     *    the whole (7) fix rests on, on a fresh plan.)               */
    {
        FpjLcdPlan p;
        memset(&p, 0, sizeof p);
        p.mac = 0xB0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
        CHECK(fpj_lcd_plan(&p) == 1 &&
              (unsigned)((p.caset[3] - p.caset[1] + 1) *
                         (p.raset[3] - p.raset[1] + 1)) ==
              p.pw * p.ph && p.pw * p.ph == 128u * 160u,
              "lcd-gate: stream length == window area == 20480 "
              "(pointer wraps to (0,0) with no re-pin)");
    }

    /* 4. DEFAULT LCM (parallel DBI) portrait: the SAME stock program —
     *    the re-pin is off on BOTH buses by default (fpdoom's own
     *    page_reset workaround class is opt-in via gram_repin).     */
    n = fpj_lcd_flush_seq(1, 0, 0, 0, seq);
    CHECK(n == 2 && seq[0] == FPJ_FS_LCDC_WAIT && seq[1] == FPJ_FS_SUBMIT,
          "lcd-gate: DEFAULT LCM portrait = LCDC_WAIT,SUBMIT too");

    /* 5. plan not applied (letterbox fallback, e.g. 240x320 / --mac):
     *    still LCDC_WAIT,SUBMIT — no commands (fpdoom's own lcd_init
     *    sized that window), but the frame-completion wait still
     *    guards the redraw + the LCDC restart. mono: single SKIP.   */
    n = fpj_lcd_flush_seq(0, 1, 0, 0, seq);
    CHECK(n == 2 && seq[0] == FPJ_FS_LCDC_WAIT && seq[1] == FPJ_FS_SUBMIT,
          "lcd-gate: no portrait plan -> LCDC_WAIT,SUBMIT (wait stays)");
    n = fpj_lcd_flush_seq(1, 1, 1, 0, seq);
    CHECK(n == 1 && seq[0] == FPJ_FS_SKIP,
          "lcd-gate: mono panel -> single SKIP step");

    /* 6. OPT-IN gram_repin=1, SPI: the (6) full-gate re-pin — LCDC
     *    frame wait (now the REAL raw-bit wait) FIRST, TX_END before
     *    the commands, RAMWR last before SUBMIT. Kept for the
     *    parallel-LCM GC9106 page-counter class / experiments.      */
    n = fpj_lcd_flush_seq(1, 1, 0, 1, seq);
    CHECK(n == 7, "lcd-gate: opt-in gram_repin SPI sequence is 7 steps");
    CHECK(seq[0] == FPJ_FS_LCDC_WAIT &&
          seq[1] == FPJ_FS_TX_END &&
          seq[2] == FPJ_FS_BUS_IDLE &&
          seq[3] == FPJ_FS_CMD_CASET &&
          seq[4] == FPJ_FS_CMD_RASET &&
          seq[5] == FPJ_FS_CMD_RAMWR &&
          seq[6] == FPJ_FS_SUBMIT,
          "lcd-gate: opt-in SPI order LCDC_WAIT,TX_END,BUS_IDLE,CASET,RASET,RAMWR,SUBMIT");
    {
        int i, tx_i = -1, cmd_i = -1, ramwr_i = -1, wait_i = -1;
        for (i = 0; i < n; i++) {
            if (seq[i] == FPJ_FS_TX_END && tx_i < 0) tx_i = i;
            if (seq[i] == FPJ_FS_CMD_CASET && cmd_i < 0) cmd_i = i;
            if (seq[i] == FPJ_FS_CMD_RAMWR) ramwr_i = i;
            if (seq[i] == FPJ_FS_LCDC_WAIT) wait_i = i;
        }
        CHECK(wait_i == 0 && tx_i >= 0 && cmd_i > tx_i && ramwr_i == n - 2,
              "lcd-gate: opt-in: frame retired (LCDC raw wait, then TX_END) "
              "before commands; RAMWR last before submit");
    }

    /* 7. OPT-IN gram_repin=1, LCM: same without TX_END (the DBI busy
     *    bit covers its FIFO+shifter)                                  */
    n = fpj_lcd_flush_seq(1, 0, 0, 1, seq);
    CHECK(n == 6 &&
          seq[0] == FPJ_FS_LCDC_WAIT &&
          seq[1] == FPJ_FS_BUS_IDLE &&
          seq[2] == FPJ_FS_CMD_CASET &&
          seq[3] == FPJ_FS_CMD_RASET &&
          seq[4] == FPJ_FS_CMD_RAMWR &&
          seq[5] == FPJ_FS_SUBMIT,
          "lcd-gate: opt-in LCM drops TX_END (busy bit covers the tail)");

    /* 8. the program is a PURE function of the flag (main() parses
     *    argv and flips it BEFORE the first flush): toggling
     *    0 -> 1 -> 0 returns to the exact stock program, and repeated
     *    runs at either setting are identical (stateless).           */
    {
        int a[FPJ_LCD_SEQ_MAX], b[FPJ_LCD_SEQ_MAX], c[FPJ_LCD_SEQ_MAX];
        int na = fpj_lcd_flush_seq(1, 1, 0, 0, a);
        int nb = fpj_lcd_flush_seq(1, 1, 0, 1, b);
        int nc = fpj_lcd_flush_seq(1, 1, 0, 0, c);
        CHECK(na != nb &&
              na == nc && !memcmp(a, c, (size_t)na * sizeof a[0]),
              "lcd-gate: gram_repin toggles cleanly off->on->off");
    }

    /* 9. the (5)/(6) regression's SIGNATURE, pinned so it stays
     *    diagnosed: 4 rows x 128 px + 32 px = 544 words = 1088 bytes
     *    — the constant toroidal phase per-frame register banging
     *    produced (round (5): exactly this; round (6): a different
     *    constant — ANY per-frame command step can produce one).    */
    CHECK(4 * 128 + 32 == 544 && 544 * 2 == 1088,
          "lcd-gate: (5)/(6) signature: 4x128+32 = 544 words = 1088 bytes "
          "(the dx=32,dy=4 wrap)");

    /* 10. the window itself is zero-origin: a (32,4) phase would need
     *     CASET x_start=32 / RASET y_start=4 — the plan never emits
     *     them (offsets are 0/0 for the itel family, or the <=2/1
     *     132x162-glass override), so a wrap can ONLY be stream
     *     phase — which the (7) default program now has zero of.    */
    {
        FpjLcdPlan p;
        memset(&p, 0, sizeof p);
        p.mac = 0xB0; p.rotate = 3; p.w1 = 160; p.h1 = 128;
        CHECK(fpj_lcd_plan(&p) == 1 &&
              p.caset[1] == 0 && p.raset[1] == 0 &&
              p.caset[2] == 0 && p.raset[2] == 0,
              "lcd-gate: window is zero-origin (no 32/4 CASET/RASET start)");
    }
}

/* ---- portrait keypad: sys_data.rotate keypad bits + the MIDP chain ---
 * 2026-11-25 (5). The on-phone bug: fpmenu's "$@ = $@ --rotate 3"
 * set sys_data.rotate = 0x33 (BOTH the screen XOR and the KEYPAD
 * rotation — entry.c: one --rotate value fills key AND scr), and
 * syscomm.c sys_getkeymap() remapped the raw pad codes through
 * num_turn[3] before fpj2me's rawmap saw them: physical UP arrived
 * as J2ME LEFT, DOWN as RIGHT, LEFT as DOWN, RIGHT as UP (CENTER
 * and the soft keys are never rotated — exactly what the hardware
 * showed). fpj_key_rotate() clears bits 4..5 when the portrait
 * plan applied; these units pin the decision AND the full chain
 * (raw -> num_turn -> FPJ_RAWMAP -> KP_* -> MIDP -1..-7) that
 * fp_platform.c keytrn_init() builds on the target.                */
static void test_portrait_keypad(void)
{
    /* 1. the decision: portrait clears ONLY the keypad bits         */
    CHECK(fpj_key_rotate(0x33, 1) == 0x03,
          "key: --rotate 3 -> keypad bits cleared, screen bits kept");
    CHECK(fpj_key_rotate(0x33, 0) == 0x33,
          "key: fallback (no portrait) keeps fpdoom's rotation");
    CHECK(fpj_key_rotate(0x11, 1) == 0x01,
          "key: --rotate 1 -> keypad bits cleared");
    CHECK(fpj_key_rotate(0x22, 1) == 0x02,
          "key: --rotate 2 -> keypad bits cleared");
    CHECK(fpj_key_rotate(0x02, 1) == 0x02,
          "key: screen-only rotate passes through untouched");
    CHECK(fpj_key_rotate(0x3C, 1) == 0x0C,
          "key: unused bits 2..3 preserved through the clear");
    CHECK(fpj_key_rotate(0x00, 1) == 0x00,
          "key: zero stays zero (no --rotate at all)");

    /* 2. THE BUG, pinned: rotate 3 WITHOUT the clear (the fallback
     *    path) rotates the D-pad 90 degrees — sys_getkeymap's exact
     *    num_turn[3] tables, replicated 1:1 in fpj_key.h            */
    CHECK(fpj_key_xlate(0x04, 0x33, 0) == KP_LEFT,
          "key: [bug demo] rotate-3 uncleared: raw UP -> KP_LEFT");
    CHECK(fpj_key_xlate(0x05, 0x33, 0) == KP_RIGHT,
          "key: [bug demo] rotate-3 uncleared: raw DOWN -> KP_RIGHT");
    CHECK(fpj_key_xlate(0x06, 0x33, 0) == KP_DOWN,
          "key: [bug demo] rotate-3 uncleared: raw LEFT -> KP_DOWN");
    CHECK(fpj_key_xlate(0x07, 0x33, 0) == KP_UP,
          "key: [bug demo] rotate-3 uncleared: raw RIGHT -> KP_UP");

    /* 3. THE FIX: same 0x33, portrait plan applied -> identity      */
    CHECK(fpj_key_xlate(0x04, 0x33, 1) == KP_UP,
          "key: portrait raw UP -> KP_UP");
    CHECK(fpj_key_xlate(0x05, 0x33, 1) == KP_DOWN,
          "key: portrait raw DOWN -> KP_DOWN");
    CHECK(fpj_key_xlate(0x06, 0x33, 1) == KP_LEFT,
          "key: portrait raw LEFT -> KP_LEFT");
    CHECK(fpj_key_xlate(0x07, 0x33, 1) == KP_RIGHT,
          "key: portrait raw RIGHT -> KP_RIGHT");

    /* 4. the never-rotated keys: CENTER/DIAL and the soft keys are
     *    identical with or without the clear (matches the hardware
     *    report: soft keys were always correct)                     */
    CHECK(fpj_key_xlate(0x0d, 0x33, 1) == KP_CENTER &&
          fpj_key_xlate(0x0d, 0x33, 0) == KP_CENTER,
          "key: CENTER never rotated (portrait or not)");
    CHECK(fpj_key_xlate(0x01, 0x33, 1) == KP_CENTER,
          "key: DIAL = CENTER/FIRE on 4-way pads");
    CHECK(fpj_key_xlate(0x08, 0x33, 1) == KP_SOFT1 &&
          fpj_key_xlate(0x08, 0x33, 0) == KP_SOFT1,
          "key: LSOFT never rotated -> SOFT1");
    CHECK(fpj_key_xlate(0x09, 0x33, 1) == KP_SOFT2 &&
          fpj_key_xlate(0x09, 0x33, 0) == KP_SOFT2,
          "key: RSOFT never rotated -> SOFT2");

    /* 5. digits rotate with the pad grid in the fallback; upright in
     *    portrait (Canvas '2' doubles as UP inside the game).
     *    num_turn[3] digit grid: "741852963" — '2' lands on key '4'. */
    CHECK(fpj_key_xlate(0x32, 0x33, 0) == KP_4,
          "key: [bug demo] rotate-3 uncleared: digit 2 -> KP_4");
    CHECK(fpj_key_xlate(0x32, 0x33, 1) == KP_2,
          "key: portrait digit 2 -> KP_2");
    CHECK(fpj_key_xlate(0x35, 0x33, 1) == KP_5,
          "key: portrait digit 5 -> KP_5 (rotation fixed point)");

    /* 6. the FULL MIDP contract (midp_input.c keymap): KP_* -> the
     *    exact Canvas key codes the user's D-pad spec requires.     */
    CHECK(fpj_key_xlate(0x04, 0x33, 1) == KP_UP    && ACT_UP    == -1 &&
          fpj_key_xlate(0x05, 0x33, 1) == KP_DOWN  && ACT_DOWN  == -2 &&
          fpj_key_xlate(0x06, 0x33, 1) == KP_LEFT  && ACT_LEFT  == -3 &&
          fpj_key_xlate(0x07, 0x33, 1) == KP_RIGHT && ACT_RIGHT == -4,
          "key: D-pad UP/DOWN/LEFT/RIGHT = Canvas -1/-2/-3/-4");
    CHECK(fpj_key_xlate(0x0d, 0x33, 1) == KP_CENTER && ACT_FIRE  == -5 &&
          fpj_key_xlate(0x08, 0x33, 1) == KP_SOFT1  && SOFT1     == -6 &&
          fpj_key_xlate(0x09, 0x33, 1) == KP_SOFT2  && SOFT2     == -7,
          "key: OK=FIRE(-5) LSOFT=SOFT1(-6) RSOFT=SOFT2(-7)");

    /* 7. unmapped raw codes return KP_NONE (sys_event drops them;
     *    the diag overlay's dead-key audit names them)              */
    CHECK(fpj_key_xlate(0x0e, 0x33, 1) == KP_NONE,
          "key: unmapped raw (CAMERA 0x0e) -> KP_NONE");
}

/* --------------------------------------------------------------------
 * AC1 round 2 (2026-11-25 (3)): the natives the in-game flow needs.
 * ------------------------------------------------------------------ */
static void test_ac1_natives(void)
{
    /* This suite runs before any fpj2me_boot(): the object heap needs
     * its one-free-block arena (heap_init) before ref_alloc can walk
     * it. Class metadata + the registry live in .bss/meta and work
     * pre-boot.                                                     */
    heap_init();

    /* fillTriangle: rasterizes (AC1's IGP panel backgrounds)         */
    JImage *dst = image_create(16, 16);
    MGraphics g; gfx_reset(&g, dst);
    gfx_color_rgb(&g, 0, 255, 0);
    gfx_fill_triangle(&g, 0, 0, 15, 0, 0, 15);      /* half the square */
    int tri = 0;
    for (int i = 0; i < 256; i++) if (dst->px[i] == 0x07E0) tri++;
    CHECK(tri > 80 && tri < 200,
          "gfx: fillTriangle rasterizes a half-square");
    gfx_fill_triangle(&g, 8, 8, 8, 8, 8, 8);        /* degenerate     */
    image_free(dst);

    CHECK(native_lookup("javax/microedition/lcdui/Graphics", "fillTriangle",
                        "(IIIIII)V") != 0,
          "gfx: fillTriangle native registered");

    /* RecordStore.getRecord(id, buf, off): copy-in + return length   */
    remove("/tmp/fpj2me/rms/tests/Copy.rms");
    int rs = rms_open("tests", "Copy", 1);
    CHECK(rs >= 0, "rms: getRecord-into open");
    int id = rms_add(rs, "AC1SAVE!", 8);
    JRef buf = ref_alloc(T_ARR_B, 4 + 16, 0);
    { JObj *b = obj(buf); ARR_LEN(b) = 16; memset(ARR_DATA(b), 0, 16); }
    Slot a[5]; Slot r[2];
    /* hand-build the RecordStore receiver: slot 1 carries the handle
     * (same layout nat_RS_open produces via native_new)             */
    JRef store = ref_alloc(T_OBJECT, 4 * 4, 0);
    { JObj *o = obj(store);
      o->clazz = class_load("javax/microedition/rms/RecordStore");
      OBJ_PAYLOAD(o)[1] = (u32)rs; }
    a[0].ref = store; a[1].w = (u32)id; a[2].ref = buf; a[3].w = 2;
    const NativeEntry *ne = native_lookup("javax/microedition/rms/RecordStore",
                                          "getRecord", "(I[BI)I");
    CHECK(ne != 0, "rms: getRecord(I[BI)I registered");
    if (ne) {
        ne->fn(a, 4, r);
        CHECK((s32)r[0].w == 8, "rms: getRecord-into returns length");
        JObj *b = obj(buf);
        CHECK(memcmp(ARR_DATA(b) + 2, "AC1SAVE!", 8) == 0,
              "rms: getRecord-into copies at offset");
    }
    rms_close(rs);

    /* System.out is a real object: println on the error path cannot
     * NPE a glyph-sheet build (GLLib font engine)                    */
    JRef so = fpj_std_stream("out");
    CHECK(so != 0 && obj(so) != 0,
          "System.out: real PrintStream stub (font error path)");
    JRef se = fpj_std_stream("err");
    CHECK(se != 0 && se != so, "System.err: distinct stub object");
    CHECK(native_lookup("java/io/PrintStream", "println",
                        "(Ljava/lang/String;)V") != 0,
          "System.out: println(String) registered (silent)");

    /* Sound.init([BI)V: the last AC1 com/nokia/mid native            */
    CHECK(native_lookup("com/nokia/mid/sound/Sound", "init", "([BI)V") != 0,
          "sound: init([BI)V registered (f.b(I)V re-arm before play)");

    /* Thread.yield pumps: GLLib's frame limiter spins on yield; the
     * 2^18-dispatch hook alone fires ~1.2x/s at phone speed          */
    const NativeEntry *ty = native_lookup("java/lang/Thread", "yield", "()V");
    CHECK(ty != 0, "Thread.yield registered");
    /* (behavioral: midp_paint_pump is called from it — covered by the
     * repaint-delivery tests; here we assert the registry entry)     */
}

/* ---- interpreter hot-path: cp resolution cache ----------------------- */
static void test_cp_cache(void)
{
    /* run a static method TWICE: the second execution goes through
     * the per-class cp_cache path (JField* / InvokeSite*). Identical
     * results + the app_property re-boot below re-exercise caching
     * after a full VM reset (the cache must die with the classes).  */
    Slot ret[2] = {{0}, {0}};
    CHECK(fpj_call_static("Tst", "run", "()I", ret) && (s32)ret[0].w == 5,
          "cache: 1st exec resolves + caches");
    CHECK(fpj_call_static("Tst", "run", "()I", ret) && (s32)ret[0].w == 5,
          "cache: 2nd exec via cp_cache (same result)");
    CHECK(fpj_call_static("Tst", "run2", "()I", ret) && (s32)ret[0].w == 5,
          "cache: native dispatch via cached InvokeSite");
    CHECK(fpj_call_static("Tst", "run4", "()I", ret) && (s32)ret[0].w == 41,
          "cache: getstatic/putstatic via cached JField");
}

int main(void)
{
    test_jar();
    test_transforms();
    test_clip_fill_string();
    test_setclip_replace_black();
    test_ac1_natives();
    test_rms();
    test_png();
    test_input();
    test_font();
    test_vm();
    test_gc();
    test_fatal_diag();
    test_cp_cache();
    test_nokia_sound();
    test_app_property();
    test_lcd_portrait();
    test_lcd_flush_gate();
    test_portrait_keypad();
    printf("\n%s (%d failure%s)\n", fails ? "RESULT: FAIL" : "RESULT: ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
