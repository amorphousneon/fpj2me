/*
 * fpj2me — fpj_lcd.h : 128x160 NATIVE-PORTRAIT LCD scanout plan.
 *
 * WHY THIS EXISTS (the on-phone bug, itel i2163 / SC6531E):
 *   fpdoom boots the panel through its own table + config.txt options
 *   and can leave the LCD controller in a LANDSCAPE scan state -- most
 *   commonly fpmenu's stock config.txt line "$@ = $@ --rotate 3"
 *   (XORs MADCTL with rtab[3]=0x60 => MV set => fpdoom swaps w/h and
 *   programs CASET 0..159 / RASET 0..127 = the "160x128 landscape
 *   configuration" every fpdoom app shares). fpj2me pushes a 128x160
 *   PORTRAIT RGB565 framebuffer (pitch 128) into that grid:
 *     - the panel's RAMWR fill stride (160) != our row stride (128)
 *       => the image is sheared and wraps around a wandering black bar
 *       ("S" + "DO YO" | gap | "OUND SET" + "U WANT SOUND?");
 *     - the DMA window (w2 x h2, mis-clamped to 128x128) != the GRAM
 *       window (160x128) => the write pointer never lands back on the
 *       window origin => the split drifts boot to boot, frame to frame;
 *     - MADCTL MV renders the whole picture sideways.
 *   THE FIX: at lcd_appinit() (fpdoom calls it BETWEEN lcd_init() and
 *   lcdc_init()) the app re-programs the panel into its NATIVE
 *   128x160 PORTRAIT orientation -- MADCTL(0x36) with MV cleared and
 *   the panel's own MY/MX/RGB bits preserved, CASET(0x2A) = 0..127,
 *   RASET(0x2B) = 0..159, RAMWR(0x2C) -- then patches sys_data.display
 *   (w1/h1/w2/h2 = 128x160) so lcdc_init() programs the LCDC DMA and
 *   spi_refresh_next()'s pixel count to EXACTLY 128*160 = 20480 pixels
 *   (40,960 bytes) row-major, pitch 128.
 *   2026-11-25 (7) — THE REFRESH FLOW IS NOW STOCK FPDOOM'S: rounds
 *   (4)/(5)/(6) re-issued CASET/RASET/RAMWR before every DMA submit
 *   (with increasingly careful gates) and the real panel STILL
 *   wrapped — every change to the per-frame register banging only
 *   shifted the wrap's phase. Stock fpdoom/fpmenu drive this exact
 *   SPI panel with ZERO wrap precisely because they do NOT touch
 *   the panel per frame: lcm_init() forces page_reset = 0 on SPI
 *   panels, the window is programmed once at boot, and every
 *   20480-pixel stream exactly fills the 128x160 window so the GRAM
 *   write pointer wraps back to (0,0) on its own. fp_lcd_flush()
 *   therefore now only (a) waits for the previous frame's LCDC
 *   end-of-capture by polling the RAW irq status (sys_wait_refresh()
 *   is a no-op after lcdc_init() masks irq.en) and (b) submits —
 *   fpj_lcd_flush_seq() below pins that program. The per-frame
 *   re-pin survives only as the OPT-IN gram_repin flag (config.txt
 *   "-- --gram-repin", default 0) for a parallel-LCM GC9106-class
 *   panel whose page counter genuinely needs it.
 *
 * This header is the DECISION half (pure math, no registers): it is
 * compiled into BOTH the target (fp_platform.c / target_main.c) and
 * the host checks (tests/hostcheck.c), so the on-phone behavior is
 * unit-tested 1:1 on the PC. The register half (SC6531E LCM FIFO /
 * SPI primitives, verified against fpdoom's syscode.c + lcd_spi.h)
 * lives in fp_platform.c.
 *
 * fpdoom state we consume (all read AFTER its lcd_init(), none of it
 * modified by us before the plan runs):
 *   sys_data.mac     MADCTL as actually programmed (base ^ rtab[rotate])
 *                    [bit 0x100 = fpdoom's monochrome flag]
 *   sys_data.rotate  bits 0..1 = screen rotate from config.txt
 *   sys_data.display.w1/h1 panel size AFTER lcd_init's MV swap
 *
 * MADCTL bits (ST7735/GC9106 family, "0x36"):
 *   0x80 MY  row/page address order   (vertical mirror)
 *   0x40 MX  column address order     (horizontal mirror)
 *   0x20 MV  row/column EXCHANGE      (90-degree scan = landscape)
 *   0x10 ML  vertical refresh order   (cosmetic, preserved verbatim)
 *   0x08 RGB RGB/BGR order            (preserved verbatim)
 *   0x02 MH  horizontal refresh order (cosmetic, preserved verbatim)
 */
#ifndef FPJ2ME_LCD_H
#define FPJ2ME_LCD_H

#include "platform.h"            /* FP_LCD_W / FP_LCD_H (128x160) */

#define FPJ_MAC_MY    0x80u
#define FPJ_MAC_MX    0x40u
#define FPJ_MAC_MV    0x20u
#define FPJ_MAC_MONO  0x100u      /* fpdoom: monochrome panel flag   */

/* Panel-specific GRAM offsets for 128x160 portrait windows. The itel
 * family tables in fpdoom's lcd_config.h (it2160 ST7735S 0x7c89f0,
 * it2161R GC9104 0x009104, meanIT GC9106 0x009109 ...) are PLAIN
 * 128x160 modules: their OEM init streams carry CASET 0..0x7f /
 * RASET 0..0x9f with NO +2/+1 offset, so 0/0 is correct for the
 * it2163. Some 132x162-glass ST7735 "tab" modules need (2,1) in
 * portrait: override with -DFPJ_LCD_GRAM_XOFF=2 -DFPJ_LCD_GRAM_YOFF=1
 * in the Makefile APP_CFLAGS if a module ever letterboxes by 2/1 px. */
#ifndef FPJ_LCD_GRAM_XOFF
#define FPJ_LCD_GRAM_XOFF 0
#endif
#ifndef FPJ_LCD_GRAM_YOFF
#define FPJ_LCD_GRAM_YOFF 0
#endif

typedef struct {
        /* ---- IN: state exactly as fpdoom's lcd_init() left it ------ */
        unsigned mac;      /* sys_data.mac                            */
        unsigned rotate;   /* sys_data.rotate & 3                     */
        unsigned w1, h1;   /* sys_data.display.w1 / h1                */
        unsigned xoff, yoff; /* portrait GRAM offsets (FPJ_LCD_GRAM_*) */

        /* ---- OUT ---------------------------------------------------- */
        int reprogram;     /* 1: apply portrait cmds + patch sys_data  */
        unsigned madctl;   /* MADCTL (0x36) value to write             */
        unsigned caset[4]; /* CASET (0x2A): XS>>8, XS, XE>>8, XE       */
        unsigned raset[4]; /* RASET (0x2B): YS>>8, YS, YE>>8, YE       */
        unsigned pw, ph;   /* portrait w1/h1 to patch into sys_data    */
        unsigned fw, fh;   /* framebuffer window w2/h2                 */
} FpjLcdPlan;

/* fpdoom syscode.c lcd_init(): static const uint8_t rtab[] =
 *      { 0, 0xa0, 0xc0, 0x60 };   rotate 0/1/2/3 -> MADCTL XOR mask  */
#define FPJ_LCD_RTAB0 0x000u
#define FPJ_LCD_RTAB1 0x0a0u
#define FPJ_LCD_RTAB2 0x0c0u
#define FPJ_LCD_RTAB3 0x060u

/*
 * Compute the portrait plan. Returns p->reprogram:
 *   1 = 128x160 color panel detected -> reprogram MADCTL/CASET/RASET/
 *       RAMWR to native portrait, patch sys_data to 128x160 (see
 *       fp_platform.c fpj_lcd_apply_portrait()).
 *   0 = leave fpdoom's configuration alone (monochrome flag, --mac
 *       override, or a panel whose portrait geometry is not exactly
 *       FP_LCD_W x FP_LCD_H, e.g. 240x320) -> the caller falls back
 *       to the old letterbox clamp.
 *
 * Pure function of its inputs: running it twice yields the identical
 * plan, so the per-frame window re-issue in fp_lcd_flush() is
 * stateless and can never itself introduce drift.
 */
static inline int fpj_lcd_plan(FpjLcdPlan *p)
{
        static const unsigned rtab[4] = {
                FPJ_LCD_RTAB0, FPJ_LCD_RTAB1, FPJ_LCD_RTAB2, FPJ_LCD_RTAB3
        };
        unsigned base, pmac, mv_now, tw, th;

        p->reprogram = 0;
        p->pw = p->ph = p->fw = p->fh = 0;
        { unsigned i; for (i = 0; i < 4; i++) p->caset[i] = p->raset[i] = 0; }

        /* monochrome / --mac override: fpdoom runs those through its
         * mono refresh path (sys_data.mac & 0x100); do not touch     */
        if (p->mac & FPJ_MAC_MONO) return 0;

        /* undo fpdoom's rotate XOR to recover the panel's NATIVE
         * MADCTL -- the orientation the OEM firmware runs its own
         * portrait UI in (upright text, correct RGB order, correct
         * mirror polarity). rotate is invertible: it is an XOR.      */
        base = (p->mac ^ rtab[p->rotate & 3]) & ~FPJ_MAC_MONO;

        /* panel geometry in PORTRAIT terms: lcd_init swapped w/h iff
         * the FINAL mac (the one it programmed, = our input mac) had
         * MV set, so undo that swap to get the native w/h           */
        mv_now = p->mac & FPJ_MAC_MV;
        tw = mv_now ? p->h1 : p->w1;          /* portrait width  */
        th = mv_now ? p->w1 : p->h1;          /* portrait height */

        /* only a true 128x160 panel (in portrait terms) is fixed to
         * 1:1; anything else keeps fpdoom's letterbox behavior      */
        if (tw != (unsigned)FP_LCD_W || th != (unsigned)FP_LCD_H) return 0;

        /* portrait MADCTL: the panel's native setting. If the panel
         * table itself was landscape-native (MV in the base value --
         * none of the itel-family entries are), clear MV and swap
         * MY/MX so the physical scan directions are preserved.      */
        pmac = base;
        if (pmac & FPJ_MAC_MV) {
                pmac = (pmac & ~(FPJ_MAC_MY | FPJ_MAC_MX | FPJ_MAC_MV))
                     | ((pmac & FPJ_MAC_MY) >> 1)    /* MY -> MX */
                     | ((pmac & FPJ_MAC_MX) << 1);   /* MX -> MY */
        }
        p->madctl = pmac;

        /* the portrait GRAM window: CASET = physical X (128 cols),
         * RASET = physical Y (160 rows), panel offsets applied      */
        p->caset[0] = (p->xoff >> 8) & 0xff;
        p->caset[1] =  p->xoff        & 0xff;
        p->caset[2] = ((p->xoff + (unsigned)FP_LCD_W - 1) >> 8) & 0xff;
        p->caset[3] =  (p->xoff + (unsigned)FP_LCD_W - 1)        & 0xff;
        p->raset[0] = (p->yoff >> 8) & 0xff;
        p->raset[1] =  p->yoff        & 0xff;
        p->raset[2] = ((p->yoff + (unsigned)FP_LCD_H - 1) >> 8) & 0xff;
        p->raset[3] =  (p->yoff + (unsigned)FP_LCD_H - 1)        & 0xff;

        /* 1:1 portrait scanout: window == framebuffer == DMA == the
         * SPI pixel count (w1*h1): 128 * 160 * 2 = 40,960 bytes      */
        p->pw = (unsigned)FP_LCD_W;
        p->ph = (unsigned)FP_LCD_H;
        p->fw = (unsigned)FP_LCD_W;
        p->fh = (unsigned)FP_LCD_H;
        p->reprogram = 1;
        return 1;
}

/* ---- per-flush program (pure, host-unit-tested) ---------------------
 * 2026-11-25 (7). THE ROUND-6 LESSON (real itel i2163): every gate
 * ORDER still left a wrap. (5)'s FIFO/BUSY gate gave phase 544
 * ("MISSION FAILE|D"); (6)'s TX_END count-closed gate gave a
 * different phase ("YOU WANT TO|DO") — because the gates were never
 * the problem: the per-frame CASET/RASET/RAMWR register banging
 * ITSELF desynchronizes the pixel count (each command burst toggles
 * chnl_len/CD/ctl8/ctl9 on the SPI the LCDC capture stream uses,
 * sys_wait_refresh() early-returns because lcdc_init() masked
 * LCDC_BASE->irq.en &= ~1, and the toggle lands mid-capture).
 * Stock fpdoom runs this exact panel with ZERO wrap: window commands
 * once at boot (page_reset == 0 on SPI — lcm_init() forces it), then
 * every frame is clean_dcache() + spi_refresh_next() + LCDC start,
 * and the 20480-word stream exactly fills the 128x160 window: the
 * GRAM write pointer wraps modulo 20480 back to (0,0) with no help.
 *
 * (7) adopts that flow. THE DEFAULT program:
 *     LCDC_WAIT -> SUBMIT
 *   — no command steps at all, on BOTH buses (LCM GC9106s that
 *     genuinely need a per-frame re-pin — fpdoom's page_reset class
 *     — can opt in, see gram_repin).
 *   LCDC_WAIT is the one gate that stays, and it is now REAL:
 *     sys_wait_refresh() cannot be it (lcdc_init() keeps irq.en
 *     masked, so it returns before checking anything — a new refresh
 *     can be started mid-capture). The flush polls the RAW
 *     end-of-frame bit LCDC_BASE->irq.raw & 1 (raw latches
 *     regardless of the enable mask) and clears it (irq.clr |= 1),
 *     bounded by ~50 ms, BEFORE sys_start_refresh() re-arms the
 *     capture — fp_screen is then free to redraw and LCDC can never
 *     restart mid-capture. See fpj_lcdc_wait_frame() (fp_platform.c).
 * THE OPT-IN program (gram_repin == 1, config.txt "-- --gram-repin"):
 *   SPI:  LCDC_WAIT, TX_END, BUS_IDLE, CASET, RASET, RAMWR, SUBMIT
 *   LCM:  LCDC_WAIT, BUS_IDLE, CASET, RASET, RAMWR, SUBMIT
 *   — the (6) full-gate re-pin, for parallel-LCM GC9106-class panels
 *     whose page counter genuinely wraps wrong (fpdoom's page_reset
 *     workaround class). TX_END stays before the commands and RAMWR
 *     stays last; the commands run only after the frame provably
 *     completed (raw IRQ), so the burst can no longer cut a capture
 *     mid-stream.
 * MONO: a single SKIP (sys_start_refresh() handles mono alone).
 *
 * This enum + fpj_lcd_flush_seq() pin the program on the host; the
 * register half (fpj_lcdc_wait_frame / fpj_spi_retire_frame) lives
 * in fp_platform.c.                                                    */
enum {
        FPJ_FS_SKIP = 0,  /* mono: sys_start_refresh() does it all   */
        FPJ_FS_LCDC_WAIT, /* poll LCDC irq.raw&1, clear, ~50 ms cap  */
        FPJ_FS_TX_END,    /* [repin,SPI] TX_END: frame count CLOSED  */
        FPJ_FS_BUS_IDLE,  /* [repin] SPI FIFO+shifter / DBI drained  */
        FPJ_FS_CMD_CASET, /* [repin] 0x2A window columns             */
        FPJ_FS_CMD_RASET, /* [repin] 0x2B window rows                */
        FPJ_FS_CMD_RAMWR, /* [repin] 0x2C: park the write pointer    */
        FPJ_FS_SUBMIT,    /* sys_start_refresh(): 20480-pixel DMA    */
};

/* how many steps fpj_lcd_flush_seq() can emit (seq array size)        */
#define FPJ_LCD_SEQ_MAX 8

/*
 * The ordered program fp_lcd_flush() executes between composing the
 * frame and submitting the DMA. Pure function of (plan_applied,
 * spi_panel, mono, gram_repin) so the host check pins the exact
 * program:
 *   DEFAULT (gram_repin == 0, mono == 0): LCDC_WAIT, SUBMIT — the
 *     stock page_reset==0 flow: NO per-frame commands, the 20480-
 *     word stream wraps the GRAM window modulo 20480 back to (0,0)
 *     (the same math that keeps stock fpdoom/fpmenu wrap-free on
 *     this exact panel).
 *   gram_repin == 1: the full gated re-pin (SPI adds TX_END).
 *   mono: a single SKIP step.
 * The plan-not-applied letterbox fallback also runs LCDC_WAIT,
 * SUBMIT: no commands (fpdoom's own lcd_init sized that window),
 * but the frame-completion wait still guards the redraw.
 * Returns the number of steps written into seq[] (which must have
 * room for FPJ_LCD_SEQ_MAX entries).
 */
static inline int fpj_lcd_flush_seq(int plan_applied, int spi_panel,
                                    int mono, int gram_repin, int *seq)
{
        int k = 0;

        if (mono) {
                seq[k++] = FPJ_FS_SKIP;
                return k;
        }
        seq[k++] = FPJ_FS_LCDC_WAIT;      /* ALWAYS first: real gate  */
        if (!plan_applied || !gram_repin) {
                seq[k++] = FPJ_FS_SUBMIT; /* stock page_reset==0 flow */
                return k;
        }
        if (spi_panel)
                seq[k++] = FPJ_FS_TX_END;
        seq[k++] = FPJ_FS_BUS_IDLE;
        seq[k++] = FPJ_FS_CMD_CASET;
        seq[k++] = FPJ_FS_CMD_RASET;
        seq[k++] = FPJ_FS_CMD_RAMWR;
        seq[k++] = FPJ_FS_SUBMIT;
        return k;
}

/* ---- target-only API (implemented in fp_platform.c) -----------------
 * Host tests exercise fpj_lcd_plan() directly; these drive the
 * real SC6531E LCM/SPI registers (replicated from fpdoom syscode.c /
 * lcd_spi.h) and only exist in the target build:
 *   fpj_lcd_apply_portrait()  lcd_appinit() hook (between lcd_init()
 *                             and lcdc_init()): send MADCTL/CASET/
 *                             RASET/RAMWR ONCE at boot, patch
 *                             sys_data to 128x160
 *   fpj_lcd_gram_reset()      fp_lcd_flush() hook: the OPT-IN per-
 *                             frame CASET/RASET/RAMWR re-pin (only
 *                             when fpj_lcd_set_gram_repin(1) — the
 *                             config.txt "-- --gram-repin" flag;
 *                             DEFAULT: no-op on every panel)
 *   fpj_lcd_set_gram_repin()  main() calls this once with the parsed
 *                             argv flag before the first flush
 * fpj_lcd_apply_portrait() is a no-op when the plan did not apply;
 * fpj_lcd_gram_reset() additionally no-ops while the flag is 0.     */
int  fpj_lcd_apply_portrait(void);
void fpj_lcd_gram_reset(void);
void fpj_lcd_set_gram_repin(int on);

#endif /* FPJ2ME_LCD_H */
