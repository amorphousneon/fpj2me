/*
 * fpj2me — target_main.c : the sdboot/fpmenu app entry.
 *
 * Boot flow on the phone (identical to every fpdoom app):
 *   sdboot -> fpbin/fpmain.bin (fpmenu) -> reads fpbin/config.txt
 *     -> menu line "|Java games| fpbin/fpj2me.bin $@ --dir games/fpj2me"
 *     -> fpdoom startup (entry.c) parses --dir, points fatdata curdir
 *        at games/fpj2me, hands the rest to main()
 *
 * main(): paints BLUE "scanning" IMMEDIATELY (before anything that
 *   can fail — fpmenu switches the backlight OFF just before starting
 *   us, and only our sys_start() switches it back on; a phone that
 *   stays BLACK with the backlight off never reached this paint, i.e.
 *   the .bin itself did not load/run — see README "screen colors"),
 *   then the jar menu (fp_menu.c) -> GREEN "loading" -> fpj2me_boot(jar)
 *   -> frame pump until the MIDlet calls notifyDestroyed() -> VM reset
 *   -> back to the menu. Boot failures show a RED screen with a short
 *   text; every stage is one flat color so the phone alone is enough
 *   to triage (see README "screen colors").
 *
 * All game files are RELATIVE paths resolved inside the --dir folder:
 * <anything>.jar + rms/<jar-name>/ (per-suite RecordStores).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "syscode.h"
#include "platform.h"
#include "fpj_lcd.h"
#include "fpj2me.h"
#include "midp.h"
#include "fp_font.h"
#include "fp_menu.h"
#include "libc_shims.h"

void lcd_appinit(void)
{
    /* fpdoom contract (syscode.c sys_init -> lcd_appinit -> lcdc_init):
     * the system fills w1/h1 (panel size) but NEVER w2/h2 — the app
     * must size its framebuffer window here. lcdc_init() then programs
     * LCDC img.size_xy = w2 x h2 and img.pitch = w2.
     * Leaving w2/h2 at 0 makes the LCDC scan out a 0x0 image:
     * BLACK SCREEN WITH BACKLIGHT ON. Assign unconditionally.
     *
     * THE 128x160 PORTRAIT SWITCH (fpj_lcd.h + fp_platform.c): if
     * fpdoom left the panel in its shared 160x128 LANDSCAPE scan
     * (fpmenu's stock config.txt "$@ = $@ --rotate 3", or any panel
     * table with MV), re-program MADCTL/CASET/RASET/RAMWR to the
     * panel's NATIVE 128x160 portrait orientation right here — this
     * hook runs BETWEEN lcd_init() and lcdc_init(), while the bus is
     * still in 8-bit command mode — and patch w1/h1/w2/h2 to 128x160
     * so lcdc_init() + spi_refresh_next() drive a 128*160-pixel DMA
     * that matches the GRAM window 1:1 (no split-screen wrap, no
     * sideways image, fp_screen pitch 128 = 256 bytes/row).       */
    if (fpj_lcd_apply_portrait())
        return;                    /* 128x160 portrait installed 1:1  */

    /* Fallback (mono flag / --mac override / non-128x160 panels such
     * as 240x320): the original letterbox clamp. fp_screen is a
     * packed 128x160 buffer, so the pitch must be FP_LCD_W; a taller
     * panel letterboxes (intended: MIDP 128x160 game builds); a wider
     * panel letterboxes horizontally.                                */
    struct sys_display *d = &sys_data.display;
    unsigned w = d->w1, h = d->h1;

    if (w > FP_LCD_W) w = FP_LCD_W;    /* letterbox wider panels      */
    if (h > FP_LCD_H) h = FP_LCD_H;
    w &= ~1u;                          /* lcdc_init: width must be even */

    d->w2 = (uint16_t)w;
    d->h2 = (uint16_t)h;
}

static void banner_msg(uint16_t c, const char *l1, const char *l2)
{
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, c);
    if (l1) fp_draw_text(2, 4 * FP_FONT_H, l1, 0xFFFF);
    if (l2) fp_draw_text(2, 6 * FP_FONT_H, l2, 0xFFFF);
    fp_lcd_flush();
}

static const char *boot_err_text(int rc)
{
    switch (rc) {
    case -1: return "cannot open jar";
    case -2: return "no MIDlet-1 in";
    case -3: return "MIDlet class";
    case -4: return "out of memory";
    case -5: return "no startApp()";
    case -6: return "exception in start";  /* fpj_boot_err has the class */
    default: return "boot failed";
    }
}

/* ---- boot telemetry (overrides interp.c's weak no-ops) ------------ *
 * Drawn live on the GREEN loading screen so a hang names its last
 * completed step without any USB log (LIBC_SDIO=3 has no log channel).
 * Row 9: milestone (yellow). Row 11: class being loaded (white).
 * Each call repaints its line + flushes — cheap at boot frequency.
 * BOTH hooks are gated on fpj_telemetry: it is turned OFF at startApp
 * ENTRY now (interp.c), so nothing here can ever paint over a running
 * game — the "run" milestone below included.                        */
void fpj_stage(const char *msg)
{
    if (!fpj_telemetry) return;       /* game started: LCD is the game's */
    fp_fill_rect(0, 9 * FP_FONT_H, FP_LCD_W, FP_FONT_H, 0x07E0);
    fp_draw_text(2, 9 * FP_FONT_H, msg, 0xFFE0);
    fp_lcd_flush();
}

void fpj_stage_class(const char *name)
{
    if (!fpj_telemetry) return;      /* runtime loads: stay silent    */
    fp_fill_rect(0, 11 * FP_FONT_H, FP_LCD_W, FP_FONT_H, 0x07E0);
    fp_draw_text(2, 11 * FP_FONT_H, name, 0xFFFF);
    fp_lcd_flush();
}

/* red error screen + code + press-any-key; returns after one keypress */
static void error_screen(int rc, const char *jar)
{
    char buf[18];
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0xF800);       /* red      */
    fp_draw_text(2, 1 * FP_FONT_H, "boot failed", 0xFFFF);
    fp_draw_text(2, 3 * FP_FONT_H, boot_err_text(rc), 0xFFFF);
    /* -6: the escaped exception class ("uncaught java.lang..."),       
       anything else: the old fixed "not found" line                   */
    fp_draw_text(2, 4 * FP_FONT_H, rc == -6 ? fpj_boot_err : "not found", 0xFFFF);
    fp_draw_text(2, 6 * FP_FONT_H, "E:", 0xFFFF);
    snprintf(buf, sizeof buf, "%d", rc);
    fp_draw_text(2 + 3 * FP_FONT_W, 6 * FP_FONT_H, buf, 0xFFFF);
    fp_draw_text(2, 7 * FP_FONT_H, jar, 0xFFFF);
    fp_draw_text(2, 17 * FP_FONT_H, "press any key", 0x07FF);
    fp_lcd_flush();

    fp_key_t k; int pressed;
    for (;;) {
        while (fp_key_event(&k, &pressed))
            if (pressed) return;
        fp_sleep_ms(20);
    }
}

int main(int argc, char **argv)
{
    /* App-private args come AFTER the "--" separator in the fpmenu
     * menu line (entry.c consumes its own --bright/--charge/--dir/...
     * tokens and ERR_EXITs on unknown "-" options, so "--" is the
     * only channel): "|Java Games| $fpj2me --dir games/fpj2me -- \
     *  --gram-repin" -> argv = ["--gram-repin"]. The one flag today:
     *   --gram-repin (or gram_repin=1 / --gram-repin=1): re-enable
     *   the (4)-(6) per-frame CASET/RASET/RAMWR window re-pin that
     *   (7) turned OFF by default (it desynchronized the pixel
     *   count on the real SPI panel; stock fpdoom's page_reset==0
     *   flow needs nothing). Intended for a parallel-LCM GC9106-
     *   class panel whose page counter genuinely drifts. Parsing
     *   runs before ANY flush; the boot-time apply_portrait() is
     *   not flag-dependent (the one-time window install always
     *   runs — it happens inside sys_init(), before main).       */
    {
        int i;
        for (i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "--gram-repin") ||
                !strcmp(argv[i], "--gram-repin=1") ||
                !strcmp(argv[i], "gram_repin=1"))
                fpj_lcd_set_gram_repin(1);
            else if (!strcmp(argv[i], "--gram-repin=0") ||
                     !strcmp(argv[i], "gram_repin=0"))
                fpj_lcd_set_gram_repin(0);
        }
    }

    /* stage 0: paint FIRST, ask questions later. The banner goes to the
     * framebuffer before sys_start() (backlight) and before the event
     * drain, so a crash anywhere later in boot still leaves the blue
     * screen + text visible. LCDC is already scanning (lcdc_init ran in
     * fpdoom's stage-1 sys_init); sys_start() re-enables the backlight
     * that fpmenu turned off and starts keypad IRQs.                    */
    sys_framebuffer(fp_screen);
    banner_msg(0x001F, "fpj2me", "scanning...");
    sys_start();
    fp_lcd_flush();                 /* refresh again, now lit            */
    { int key; while (sys_event(&key) != EVENT_END); }   /* drain */

    /* ---- Round 11: cold-boot USB device enumeration ------------------
     * Under sdboot (LIBC_SDIO=3) the SC6531E UDC is powered down when
     * we start: no AHB island power, no LDO_USB -> no D+ pull-up ->
     * plugging the phone into Linux showed NOTHING in lsusb/dmesg.
     * fp_usb_init() replays the OEM firmware's own UDC cold init
     * (AHB power bit 4 + GR 0x8B000220 bit 5 + 300 ms settle +
     * SELF_PWR/USB_EN + usb_init() endpoint arming + LDO_USB power
     * cycle) and is idempotent: under the LIBC_SDIO<3 FDL path it
     * detects the live controller and adopts it untouched. After it
     * returns, a plugged host sees 1782:4d00; fp_sleep_ms() services
     * EP0 while the UI idles, so enumeration completes without any
     * MIDlet holding the pipe.                                       */
    fp_usb_init();

    for (;;) {
        char jar[32];

        /* stage 2: the jar menu; 0 = <exit> -> reboot to fpmenu      */
        if (fp_menu_run(jar, sizeof jar) == 0)
            break;

        /* stage 3: green while the jar is opened, the manifest read,
         * classes parsed and startApp() entered — if this screen is
         * stuck, the hang is inside fpj2me_boot (SD or classfile).
         * interp.c clears it to BLACK the moment startApp() begins.  */
        banner_msg(0x07E0, "loading", jar);

        /* ---- fp_time_ms() wall-clock cross-check (target issue 2) --
         * sys_timer_ms() reads the SC6531E always-on tick counter at
         * 0x8100300c (OEM firmware configures it: 1 ms step). To prove
         * the divider is not off (e.g. ~10x slow => "35 s" splashes),
         * compare it against an INDEPENDENT clock: sys_wait_us() is a
         * pure CPU-cycle loop (208 MHz core clock, 26 MHz crystal).
         * 100 x 1 ms of CPU cycles must measure ~100-160 ms on the ms
         * tick. Result is painted on the loading screen (row 13):
         *   "timer ok"    -> the ms tick IS wall-clock milliseconds
         *   "timer NNNms!"-> off by the printed factor (red text)   */
        {
            uint32_t t0 = fp_time_ms();
            for (int i = 0; i < 100; i++) sys_wait_us(1000);
            uint32_t dt = fp_time_ms() - t0;
            char tbuf[20];
            if (dt >= 60 && dt <= 400) {
                fp_draw_text(2, 13 * FP_FONT_H, "timer ok", 0x07FF);
            } else {
                snprintf(tbuf, sizeof tbuf, "timer %ums!", (unsigned)dt);
                fp_draw_text(2, 13 * FP_FONT_H, tbuf, 0xF800);
            }
            fp_lcd_flush();
        }

        int rc = fpj2me_boot(jar);
        if (rc != 0) {
            error_screen(rc, jar);
            continue;                /* back to the menu               */
        }

        /* stage 4: the game runs. notifyDestroyed() sets the flag,
         * we flush RMS + wipe the VM and return to the menu.
         * (Gameloft midlets usually never get here: their whole frame
         * loop runs INSIDE startApp — liveness comes from the engine's
         * pump hook + the blocking natives; see natives.c.)          */
        while (!fpj_midlet_done) {
            fp_lcd_wait();           /* previous refresh done          */
            fpj2me_pump();           /* input + media + repaint        */
            fp_sleep_ms(1);          /* watchdog / SD breathing room  */
        }
        fpj2me_shutdown();
    }

    exit(0);                         /* sys_exit: wd reset -> fpmenu   */
    return 0;
}
