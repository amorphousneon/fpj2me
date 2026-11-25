/*
 * fpj2me — fp_platform.c : the ONLY file that talks to the fpdoom HAL.
 * Compiled for the target app only (tests/run.sh builds the PC check
 * with its own stubs instead of this file).
 *
 * fpdoom primitives used (all verified against the fpdoom tree):
 *   syscode.h : sys_timer_ms, sys_wait_ms, sys_event, sys_framebuffer,
 *               sys_start, sys_start_refresh, sys_wait_refresh, sys_data
 *   libc_embed: fopen/fread/fwrite/fseek/ftell/fclose, malloc family,
 *               printf (via fpdoom/include)
 * Mapping strategy:
 *   - LCD: fp_screen is our back buffer; sys_framebuffer() registers it
 *     once at boot; fp_lcd_flush() = GRAM window re-pin + sys_start_
 *     refresh(); the pump waits with sys_wait_refresh() before each new
 *     frame (fp_lcd_wait).
 *   - Keys: keytrn_init() translates RAW pad codes -> our KP_* enum, so
 *     sys_event() then hands us KP_* directly.
 *
 * 128x160 NATIVE PORTRAIT SCANOUT (see src/fpj_lcd.h for the whole
 * story). fpdoom's syscode.c keeps its LCM/SPI helpers static, but the
 * app is statically linked with the SAME fpdoom system objects, so the
 * register recipe below is a faithful 1:1 replication of:
 *   syscode.c lcm_set_mode()/lcm_send()  (SC6531E parallel DBI bus)
 *   lcd_spi.h spi_send()                 (SC6531E SPI LCD bus)
 * No fpdoom file is modified; everything is driven from lcd_appinit()
 * (which fpdoom calls BETWEEN lcd_init() and lcdc_init()) and from
 * fp_lcd_flush() (before every DMA submit).
 *
 * 2026-11-25 (5) TOROIDAL WRAP + LANDSCAPE D-PAD (real-hardware round):
 *   (a) the frame's (0,0) landed ~32px/~16rows inside the GRAM window
 *       with toroidal wrap: command bytes must NEVER be clocked into a
 *       bus that is still retiring the previous DMA — sys_wait_refresh()
 *       only tracks the LCDC frame IRQ (and early-returns while the IRQ
 *       is masked), which can fire before the SPI shifter / LCM FIFO
 *       drained. Every command burst is now gated on fpj_lcd_bus_idle()
 *       (hardware-level drain of BOTH buses), and the SPI burst runs in
 *       the lcd_init command state (HLD_EN/RGB565_EN parked, 8-bit words)
 *       — the state in which the MADCTL/CASET/RASET switch provably
 *       executed at boot — restoring the pixel-stream state after, so
 *       the LCDC DMA path stays intact.
 *   (b) the D-pad arrived 90-degrees rotated: fpdoom packs KEYPAD
 *       rotation in sys_data.rotate bits 4..5 (fpmenu's --rotate 3 sets
 *       BOTH screen and key), and sys_getkeymap() remaps the raw codes
 *       before our rawmap sees them. keytrn_init() clears the keypad
 *       bits (fpj_key.h fpj_key_rotate) when the portrait plan applied:
 *       physical UP = Canvas UP (-1) ... SOFT2 = -7. See fpj_key.h.
 *
 * 2026-11-25 (6) CONSTANT (dx=32,dy=4) TOROIDAL WRAP (real-hardware
 *   round, on top of (5)): the D-pad was fixed and the image upright,
 *   but every frame wrapped exactly 544 words (4 rows x 128 + 32 --
 *   "MISSION FAILE | D", the "NO/YES" bar split over the top 4 rows),
 *   CONSTANT within a boot. Root cause: the (5) gate (TXF_REAL_EMPTY
 *   + !BUSY) reads TRUE while the 20480-word frame transfer is still
 *   ARMED and STARVED -- the LCDC capture pipeline (its ~512-word
 *   internal FIFO) plus the SPI TX FIFO (~32 words) still owe the
 *   frame TAIL to the shifter, and with the shifter idle-waiting both
 *   flags pass ("idle" != "done": the SPI FIFO is real-empty because
 *   it is STARVED, not because the frame finished). The command burst
 *   then re-wrote tx_len=1 per byte on a transfer whose word count
 *   was still open, and the 544 tail words clocked out around the
 *   freshly latched RAMWR as uncounted DATA -- pixel (0,0) landed 544
 *   words deep in the window, wrapping toroidally with zero pixels
 *   lost, identically every frame (RAMWR re-parks the pointer each
 *   frame, so the phase is exactly the stranded tail, not growing).
 *   FIX: fpj_spi_retire_frame() -- the per-flush gate is now the SPI
 *   TX_END transfer-complete IRQ (int_raw bit 8, the same IRQ fpdoom's
 *   spi_wait_tx trusts per command byte), which CANNOT latch until
 *   the armed count has fully shifted, i.e. until the LCDC has pushed
 *   AND the SPI has shifted every word of the frame, FIFOs included.
 *   FIFO/BUSY waits follow as a formality, the flag is retired, and
 *   only then do 0x2A/0x2B/0x2C go out: between RAMWR and pixel (0,0)
 *   there is provably nothing. The boot-time apply_portrait keeps the
 *   plain bus-idle gate (no frame transfer has been submitted yet at
 *   that point), and the LCM path keeps its busy-bit drain (the
 *   parallel-DBI busy bit covers its FIFO+shifter; the count/IRQ gate
 *   is the SC6531E SPI-panel equivalent). See fpj_lcd.h
 *   fpj_lcd_flush_seq() -- the gate ORDER is host-unit-tested.
 *
 * 2026-11-25 (7) HANDLE THE SCREEN LIKE STOCK FPDOOM (real-hardware
 *   round, on top of (6)): the D-pad was fixed, the image upright, but
 *   the wrap SURVIVED every gate order — (5) FIFO/BUSY gave phase
 *   544, (6) TX_END shifted it again — because the gates were never
 *   the problem: the per-frame register banging ITSELF desynchronizes
 *   the pixel count (a command burst toggles chnl_len/CD/ctl8/ctl9 on
 *   the SPI the LCDC capture stream uses, and sys_wait_refresh()
 *   early-returns because lcdc_init() masks LCDC_BASE->irq.en &= ~1,
 *   so the toggle can land mid-capture). Stock fpdoom/fpmenu run this
 *   exact panel with ZERO wrap because they set MADCTL/CASET/RASET/
 *   RAMWR ONCE at boot (lcm_init() forces page_reset = 0 on SPI) and
 *   never touch a panel register per frame — the 20480-word stream
 *   exactly fills the 128x160 window and the GRAM write pointer wraps
 *   back to (0,0) modulo 20480 on its own. (7) copies that contract:
 *   (a) fp_lcd_flush() = fpj_lcdc_wait_frame() (poll the RAW
 *       end-of-frame bit LCDC_BASE->irq.raw & 1 — it latches
 *       regardless of the irq.en mask — clear it via irq.clr, ~50 ms
 *       cap) + sys_start_refresh(). NOTHING else, by default.
 *   (b) the (5)/(6) re-pin machinery is now OPT-IN: gram_repin=1
 *       (config.txt "-- --gram-repin"; fpj_lcd_set_gram_repin()),
 *       default 0 — kept for a parallel-LCM GC9106-class panel whose
 *       page counter genuinely needs the per-frame re-pin.
 *   See fpj_lcd.h fpj_lcd_flush_seq() — the program is host-unit-
 *   tested.                                                            */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "syscode.h"
#include "platform.h"
#include "midp.h"
#include "fpj_lcd.h"
#include "fpj_key.h"
#include "libc_shims.h"

/* 32-byte aligned like fpmenu's buffer: LCDC DMA + cache-line clean
 * in sys_start_refresh() work on whole lines.                          */
uint16_t fp_screen[FP_LCD_W * FP_LCD_H] __attribute__((aligned(32)));
int fp_rawmap[KP_MAX];

/* dead-key audit (diag overlay "R:xx,xx dead"): the first two raw
 * pad codes the OEM firmware produces that fpj2me maps to nothing.
   strong definition overrides natives.c's weak default            */
int fpj_dead_raw[2] = { -1, -1 };

/* ------------------------------------------------------------------ *
 * ---- LCD: 128x160 native portrait switch ------------------------- *
 * ------------------------------------------------------------------ */

/* the applied portrait plan; reprogram==0 => every reset is a no-op
 * (letterbox fallback for non-128x160 / mono / --mac panels)          */
static FpjLcdPlan fpj_plan;

/* ---- SC6531E parallel-DBI (LCM) primitives ----------------------- *
 * syscode.c:  #define LCM_REG_BASE 0x20800000
 *             LCM_CR(x) = MEM4(LCM_REG_BASE + x)
 *             lcm_config_addr: addr = lcd_cs << 26 | 0x60000000
 *             lcm_send(idx,val): wait !(LCM_CR(0)&2);
 *                  MEM2(addr | idx<<17) = val   (idx 0=cmd, 1=data)
 *             lcm_set_mode(val): LCM_CR(0x10 + cs<<4) = val
 *               mode 1 = 8-bit (manual commands)
 *               mode 0x28 = "8x2 BE" (the LCDC refresh stream)        */
#define FPJ_LCM_REG_BASE 0x20800000u
#define FPJ_LCM_CR(x)    (*(volatile uint32_t *)(FPJ_LCM_REG_BASE + (x)))
#define FPJ_LCM_FIFO(cs, idx) \
        (*(volatile uint16_t *)(((uint32_t)(cs) << 26) | 0x60000000u \
                | ((uint32_t)(idx) << 17)))

static void fpj_lcm_set_mode(unsigned val)
{
        uint32_t cs = sys_data.lcd_cs;
        while (FPJ_LCM_CR(0) & 2u) { }               /* LCM busy     */
        FPJ_LCM_CR(0x10 + (cs << 4)) = val;
}

static void fpj_lcm_send(unsigned idx, unsigned val) /* 0=cmd, 1=data */
{
        while (FPJ_LCM_CR(0) & 2u) { }
        FPJ_LCM_FIFO(sys_data.lcd_cs, idx) = (uint16_t)val;
}

/* ---- SC6531E SPI-LCD primitives (lcd_spi.h replication) ---------- *
 * Register layout and semantics copied 1:1 from fpdoom's lcd_spi.h:
 *   ctl0[6:2]  channel word length (8 = command bytes, 17/16 = pixel)
 *   ctl8[15]   CD bit: 0 = command, 1 = data
 *   ctl8[9:0]/ctl9[15:0] TX length (in channel words)
 *   ctl12[1]   SW_TX_REQ
 *   int_raw[8] TX_END_IRQ, sts2[8] BUSY, sts2[7] TXF_REAL_EMPTY       */
typedef volatile struct {
        uint32_t txd, clkd, ctl0, ctl1;              /* 0x00 */
        uint32_t ctl2, ctl3, ctl4, ctl5;
        uint32_t int_en, int_clr, int_raw, int_mask; /* 0x20 */
        uint32_t sts1, sts2, dspwait, sts3;
        uint32_t ctl6, sts4, fifo_rst, ctl7;         /* 0x40 */
        uint32_t sts5, ctl8, ctl9, ctl10;
        uint32_t ctl11, ctl12, sts6, sts7;           /* 0x60 */
        uint32_t sts8, sts9;
} fpj_spi_t;

/* wait until the SPI has fully retired every queued word: the same
 * idle check spi_refresh_next() performs before each frame            */
static void fpj_spi_wait_idle(fpj_spi_t *spi)
{
        while (!(spi->sts2 & 1u << 7)) { }   /* TXF_REAL_EMPTY */
        while (spi->sts2 & 1u << 8) { }      /* BUSY            */
}

/* ---- 2026-11-25 (6): the frame RETIRE gate ------------------------ *
 * THE BUG this closes: "FIFO empty + not busy" is NOT "frame done".
 * The pixel stream is a COUNT-ARMED transfer: spi_refresh_next() sets
 * tx_len = w1*h1 = 20480 words and triggers; the LCDC capture engine
 * feeds the TX FIFO at AHB speed while the SPI drains it at SPI-clock
 * speed, so the pipeline (LCDC capture FIFO ~512 words + SPI TX FIFO
 * ~32 words) is ~544 words deep mid-frame.  At frame end the shifter
 * goes idle-waiting for the last words and BOTH status flags pass
 * while the count is still open -- a command burst gated only on them
 * (the (5) gate) fires into a live transfer: each fpj_spi_send8
 * re-writes tx_len=1 on the still-counting hardware, and the ~544
 * stranded tail words shift out around the RAMWR that was just
 * latched, as DATA.  The write pointer starts 544 words deep every
 * frame: the exact constant (dx=32, dy=4) = 4*128+32 toroidal wrap
 * measured on the itel i2163 -- zero pixels lost (the tail is real
 * frame data, merely late) and zero drift (RAMWR re-parks the pointer
 * each frame, so the phase is exactly the stranded tail size).
 * THE GATE: TX_END (int_raw bit 8) is the transfer-COMPLETION latch
 * of this SPI IP -- the very IRQ fpdoom's spi_wait_tx polls after
 * every command byte.  It cannot assert until the armed word count
 * has fully SHIFTED, which cannot happen until the LCDC has pushed
 * every word it owes (the count includes whatever the FIFOs hold).
 * Waiting for it therefore drains BOTH chips to the last bit:
 * shifter, SPI TX FIFO, LCDC capture pipeline.  After it: FIFO
 * real-empty + not busy as a formality, then retire the flag so the
 * only thing that can set it again is the NEXT frame's completion
 * (self-cleaning: our own command bytes consume their TX_END inside
 * fpj_spi_send8).
 * TIMEOUT: bounded (FPJ_RETIRE_MS) so a never-closing armed count --
 * e.g. a boot refresh whose LCDC pushed zero words before the img
 * layer was configured -- cannot hang a flush; we fall through to
 * the plain FIFO/BUSY waits (a starved-but-idle bus is still safe for
 * commands: nothing is in flight to strand).  A real frame at these
 * SPI clocks retires in ~10-30 ms, far inside the budget.            */
#define FPJ_TX_END_IRQ   (1u << 8)   /* int_raw/int_clr: transfer done */
#define FPJ_RETIRE_MS    150u        /* >> worst-case frame shift time */

static void fpj_spi_retire_frame(fpj_spi_t *spi)
{
        uint32_t t0 = sys_timer_ms();

        while (!(spi->int_raw & FPJ_TX_END_IRQ)) {
                if ((uint32_t)(sys_timer_ms() - t0) >= FPJ_RETIRE_MS)
                        break;              /* count never closes: bail */
        }
        while (!(spi->sts2 & 1u << 7)) { }   /* TXF_REAL_EMPTY */
        while (spi->sts2 & 1u << 8) { }      /* BUSY            */
        spi->int_clr |= FPJ_TX_END_IRQ;      /* retire it       */
}

/* ---- 2026-11-25 (7): the REAL frame-completion gate ----------------- *
 * THE GAP this closes: sys_wait_refresh() is gated on LCDC_BASE->
 * irq.en — and lcdc_init() clears that enable right after the boot
 * frame ("lcdc->irq.en &= ~1") — so on every later call it returns
 * IMMEDIATELY while the LCDC capture of the previous refresh may
 * still be streaming fp_screen out. fp_lcd_flush() used to rely on
 * it; a submit then re-arms the LCDC mid-capture (and the (5)/(6)
 * command bursts landed in exactly that window).
 * THE GATE: poll the RAW end-of-frame status LCDC_BASE->irq.raw & 1
 * directly — the raw bit latches when the capture DMA finishes
 * regardless of the enable mask (irq.en only gates the CPU interrupt
 * line; sys_start_refresh() re-sets it per frame, so every frame we
 * submit latches the bit) — and clear it via irq.clr |= 1.
 * Bounded by FPJ_LCDC_WAIT_MS (~50 ms, >10x the worst-case 20480-
 * word shift at panel SPI clocks) so a wedged LCDC can never hang a
 * flush; on timeout we fall through (submitting over an idle LCDC
 * is the stock fpdoom behavior anyway).
 * fpj_frame_armed tracks "we submitted a refresh nobody retired yet"
 * so the FIRST flush (nothing in flight — lcdc_init already waited
 * its own boot frame out) does not stall the full timeout on a raw
 * bit that will never come. fp_lcd_wait() shares this: the game loop
 * waits for the previous frame BEFORE composing into fp_screen.  */
#define FPJ_LCDC_EOF_BIT  (1u << 0)   /* irq.raw/clr: frame end (osd0) */
#define FPJ_LCDC_WAIT_MS  50u         /* >> worst-case 20480-word shift */

static unsigned fpj_frame_armed;     /* 1: refresh submitted, not retired */

static void fpj_lcdc_wait_frame(void)
{
        uint32_t t0;

        if (!fpj_frame_armed) return;         /* nothing in flight */
        fpj_frame_armed = 0;

        t0 = sys_timer_ms();
        while (!(LCDC_BASE->irq.raw & FPJ_LCDC_EOF_BIT)) {
                if ((uint32_t)(sys_timer_ms() - t0) >= FPJ_LCDC_WAIT_MS)
                        break;                 /* wedged: bail out  */
        }
        LCDC_BASE->irq.clr |= FPJ_LCDC_EOF_BIT;
}

/* opt-in per-frame window re-pin (default 0): see the (7) note in
 * the file header. main() sets it from the "-- --gram-repin" argv
 * flag (config.txt) BEFORE the first flush.                           */
static int fpj_gram_repin;

void fpj_lcd_set_gram_repin(int on) { fpj_gram_repin = on ? 1 : 0; }

/* one 8-bit transaction, byte-for-byte fpdoom's spi_send() plus the
 * channel-length save/restore (after lcdc_init the SPI sits in pixel
 * mode with chnl_len 17/16; commands must go out as 8-bit words and
 * the pixel mode must be intact for the next refresh).
 * TX_END is cleared BEFORE the trigger so the wait below can only be
 * satisfied by THIS byte's completion — the 20480-word frame transfer
 * leaves TX_END latched with nobody to clear it, and a stale flag
 * would turn the sync wait into a no-op (command bytes fired back to
 * back into a live shifter = the wrap bug).                             */
static void fpj_spi_send8(fpj_spi_t *spi, unsigned cd, unsigned data)
{
        unsigned chnl = spi->ctl0 & (0x1fu << 2);

        spi->ctl0 = (spi->ctl0 & ~(0x1fu << 2)) | (8u << 2);
        if (!cd) spi->ctl8 = (spi->ctl8 & ~(1u << 15));      /* cmd  */
        spi->ctl8 = (spi->ctl8 & ~0x3ffu);                   /* len hi*/
        spi->ctl9 = (spi->ctl9 & ~0xffffu) | 1u;             /* len=1*/
        spi->int_clr |= 1u << 8;                 /* retire stale END */
        spi->ctl12 |= 2u;                                    /* TX   */
        spi->txd = data;
        while (!(spi->int_raw & 1u << 8)) { }                /* END  */
        spi->int_clr |= 1u << 8;
        while (spi->sts2 & 1u << 8) { }                      /* BUSY */
        while (!(spi->sts2 & 1u << 7)) { }                   /* EMPTY*/
        spi->ctl0 = (spi->ctl0 & ~(0x1fu << 2)) | chnl;      /* restore */
        if (!cd) spi->ctl8 |= 1u << 15;                      /* data */
}

/* fpdoom lcd_spi.h ctl7 bits that spi_refresh_init() sets for the DMA
 * pixel stream — and that must be PARKED while commands go out:
 *   bit 7    SPI_TX_HLD_EN: holds CS asserted across the whole frame
 *            burst; a manual command byte in that state is not a
 *            framed transfer the panel can latch as a command
 *   bit 14   RGB565_EN: 16->17-bit word packing (spi_mode < 3)
 * The init-time command path (lcd_spi_init ends in 8-bit + no HLD/RGB)
 * is the state that provably executed MADCTL/CASET/RASET/RAMWR at
 * boot; fpj_lcd_cmd() reproduces it exactly and restores the refresh
 * state afterwards so the coming DMA is untouched.                     */
#define FPJ_SPI_HLD_EN    (1u << 7)
#define FPJ_SPI_RGB565_EN (1u << 14)

/* command + n data bytes (n may be 0), both buses                    */
static void fpj_lcd_cmd(unsigned cmd, const unsigned *args, int n)
{
        int i;
        if (sys_data.spi) {
                fpj_spi_t *spi = (fpj_spi_t *)sys_data.spi;
                unsigned ctl7;

                fpj_spi_wait_idle(spi);
                ctl7 = spi->ctl7;
                spi->ctl7 = ctl7 & ~(FPJ_SPI_HLD_EN | FPJ_SPI_RGB565_EN);

                fpj_spi_send8(spi, 0, cmd);
                for (i = 0; i < n; i++) fpj_spi_send8(spi, 1, args[i]);

                /* back to the exact spi_refresh_init() post-state:
                 * HLD_EN/RGB565_EN as they were, CD parked at 1 (data)
                 * by the last send8, chnl_len never left pixel mode   */
                spi->ctl7 = ctl7;
        } else {
                while (FPJ_LCM_CR(0) & 2u) { }  /* bus idle first      */
                fpj_lcm_set_mode(1);                         /* 8-bit */
                fpj_lcm_send(0, cmd);
                for (i = 0; i < n; i++) fpj_lcm_send(1, args[i]);
                fpj_lcm_set_mode(0x28);                     /* 8x2 BE*/
        }
}

/*
 * HARDWARE-LEVEL bus drain — sufficient at BOOT, NOT sufficient
 * per-frame. Covers the shifter/FIFO surface: SPI = FIFO real-empty
 * + not busy (then retire any latched TX_END), LCM = busy bit clear
 * (the DBI busy bit tracks its FIFO + shifter, so it stays set until
 * the frame tail has gone out on the wire). sys_wait_refresh() only
 * tracks the LCDC frame IRQ and early-returns while lcdc_init keeps
 * it masked, so it cannot be the gate either.  On the SPI bus this
 * plain drain has one blind spot — with the frame COUNT still open
 * the shifter sits idle-STARVED and both flags pass while the LCDC
 * capture pipeline still owes the tail.  Used ONLY by
 * fpj_lcd_apply_portrait() now ((7): the per-flush path no longer
 * sends commands at all by default; the opt-in gram_repin path
 * fences on fpj_lcdc_wait_frame() + fpj_spi_retire_frame()):
 * lcd_appinit runs BEFORE lcdc_init, so no frame transfer has been
 * armed yet and the plain drain is exact.
 */
static void fpj_lcd_bus_idle(void)
{
        if (sys_data.spi) {
                fpj_spi_t *spi = (fpj_spi_t *)sys_data.spi;
                fpj_spi_wait_idle(spi);
                spi->int_clr |= 1u << 8;      /* retire stale TX_END  */
        } else {
                while (FPJ_LCM_CR(0) & 2u) { }
        }
}

/*
 * Called from lcd_appinit() (target_main.c), i.e. between fpdoom's
 * lcd_init() (panel init table + its MADCTL/CASET/RASET/RAMWR are
 * done, the bus still sits in 8-bit command mode) and lcdc_init()
 * (which reads sys_data.display to program the LCDC DMA + the SPI
 * pixel count). Returns 1 if the portrait switch was applied.
 */
int fpj_lcd_apply_portrait(void)
{
        FpjLcdPlan *p = &fpj_plan;
        struct sys_display *d = &sys_data.display;

        memset(p, 0, sizeof *p);
        p->mac    = sys_data.mac;
        p->rotate = sys_data.rotate;
        p->w1     = d->w1;
        p->h1     = d->h1;
        p->xoff   = FPJ_LCD_GRAM_XOFF;
        p->yoff   = FPJ_LCD_GRAM_YOFF;
        if (!fpj_lcd_plan(p)) return 0;

        /* 0. nothing else may own the bus: fpdoom's lcd_init() only
         *    sends commands here, but the OEM fdl/fpmenu stage can
         *    leave residue — the same gate as every later burst    */
        fpj_lcd_bus_idle();

        /* 1. panel -> native 128x160 portrait:
         *    MADCTL(0x36): MV cleared, the panel's own MY/MX/RGB/ML
         *    polarity preserved -> upright, unmirrored, right colors
         *    CASET(0x2A) 0..127, RASET(0x2B) 0..159 (panel offsets),
         *    RAMWR(0x2C) -> write pointer parked at the origin       */
        fpj_lcd_cmd(0x36, &p->madctl, 1);
        fpj_lcd_cmd(0x2A, p->caset, 4);
        fpj_lcd_cmd(0x2B, p->raset, 4);
        fpj_lcd_cmd(0x2C, 0, 0);

        /* 2. patch sys_data BEFORE lcdc_init() consumes it:
         *    w1/h1 128x160 -> LCDC disp_size/lcm_size portrait and
         *    spi_refresh_next() streams exactly 128*160 pixels
         *    w2/h2 128x160 -> img.size_xy == our framebuffer 1:1,
         *    pitch FP_LCD_W (256 bytes/row), no letterboxing         */
        sys_data.mac = (uint16_t)p->madctl;
        d->w1 = (uint16_t)p->pw;
        d->h1 = (uint16_t)p->ph;
        d->w2 = (uint16_t)p->fw;
        d->h2 = (uint16_t)p->fh;
        return 1;
}

/*
 * Re-pin the GRAM window before a DMA submit: CASET + RASET + RAMWR,
 * exactly the 128x160 window the plan installed — the OPT-IN variant
 * of what (4)-(6) did unconditionally. A no-op unless both
 * fpj_lcd_apply_portrait() succeeded AND fpj_gram_repin is set.
 *
 * 2026-11-25 (7): DEFAULT NO-OP — the stock-fpdoom refresh flow.
 * fpdoom's lcm_init() forces sys_data.page_reset = 0 on SPI panels
 * exactly because re-pinning per frame there causes more harm than
 * it cures (the (5)/(6) rounds measured it: a constant, then
 * shifting, toroidal wrap). The 128x160 window installed once at
 * boot by fpj_lcd_apply_portrait() plus a clean 20480-word DMA per
 * frame keeps the GRAM write pointer at (0,0) without any per-frame
 * commands: the stream length == the window area, so the pointer
 * wraps modulo 20480 back to the origin.
 * OPT-IN (fpj_gram_repin, "-- --gram-repin" in config.txt): the
 * (6) gated re-pin for parallel-LCM GC9106-class panels whose page
 * counter genuinely drifts (fpdoom's page_reset==1 class). The
 * command burst runs only AFTER fpj_lcdc_wait_frame() proved the
 * capture finished (raw end-of-frame bit), so it can no longer cut
 * a live capture stream; SPI adds the TX_END count-closed retire,
 * LCM drains on its DBI busy bit, and RAMWR stays the last command
 * before the caller's submit.
 */
void fpj_lcd_gram_reset(void)
{
        if (!fpj_plan.reprogram) return;
        if (sys_data.mac & FPJ_MAC_MONO) return;   /* mono: untouched */
        if (!fpj_gram_repin) return;    /* DEFAULT: stock page_reset==0 */

        fpj_lcdc_wait_frame();          /* capture provably finished    */
        if (sys_data.spi) {
                fpj_spi_t *spi = (fpj_spi_t *)sys_data.spi;
                fpj_spi_retire_frame(spi);   /* count closed + drained */
        } else {
                while (FPJ_LCM_CR(0) & 2u) { }     /* DBI busy bit     */
        }
        fpj_lcd_cmd(0x2A, fpj_plan.caset, 4);
        fpj_lcd_cmd(0x2B, fpj_plan.raset, 4);
        fpj_lcd_cmd(0x2C, 0, 0);
}

/* ---- LCD flush ----------------------------------------------------- *
 * THE flush is the one place the diag overlay renders: it stamps the
 * vitals strip into fp_screen right before the DMA submission, so a
 * refresh is submitted exactly once per real frame, with fully
 * composed pixels. The overlay used to draw + flush ITSELF from
 * inside fpj_io_pump() (mid-SD-read): a second LCD transfer per
 * 500 ms tick AND a refresh of a half-composed game frame.
 *
 * 2026-11-25 (7) — THE PROGRAM IS NOW EXACTLY STOCK FPDOOM'S
 * (fpj_lcd.h fpj_lcd_flush_seq pins it, host-unit-tested):
 *   1. fpj_diag_overlay()          — compose the final pixels
 *   2. fpj_lcdc_wait_frame()       — REAL end-of-capture: poll the
 *      RAW irq bit (sys_wait_refresh()'s en-gate made it a no-op),
 *      so fp_screen is free and the LCDC can never restart
 *      mid-capture
 *   3. fpj_lcd_gram_reset()        — DEFAULT no-op (gram_repin=0);
 *      the opt-in re-pin also runs after the frame wait
 *   4. sys_start_refresh()         — clean_dcache + spi_refresh_next
 *      + LCDC start; 20480 words == the window area, so the GRAM
 *      write pointer wraps back to (0,0) on its own
 */
void fp_lcd_flush(void)
{
    fpj_diag_overlay();          /* throttled to 500 ms; no-op if off */
    fpj_lcdc_wait_frame();       /* previous frame PROVABLY done      */
    fpj_lcd_gram_reset();        /* default: no-op (stock flow)       */
    sys_start_refresh();
    if (!(sys_data.mac & 0x100u)) fpj_frame_armed = 1;  /* retire next */
}
void fp_lcd_wait(void)  { fpj_lcdc_wait_frame(); }

/* ---- time / debug ---------------------------------------------------- */
uint32_t fp_time_ms(void)      { return sys_timer_ms(); }

/* Round 11: the USB idle pump runs from every sleep (see the end of
 * this file); forward-declared here because fp_sleep_ms sits above
 * the USB section.                                                     */
static void fpj_usb_pump(void);

void     fp_sleep_ms(uint32_t ms)
{
    fpj_usb_pump();            /* service EP0 while the UI idles     */
    sys_wait_ms(ms);
}

void fp_dbg(const char *s) { printf("%s\n", s); }

int fp_printf(const char *fmt, ...)
{ va_list a; va_start(a, fmt); int r = vprintf(fmt, a); va_end(a); return r; }

/* ---- keypad: raw -> KP_* via keytrn, then sys_event ------------------ */
/* RAW codes from fpdoom's KEYPAD_ENUM (syscode.h). The raw->KP table
 * itself is shared with the host checks: src/fpj_key.h FPJ_RAWMAP.
 * If a key is dead on your handset, dump sys_event codes once and
 * adjust THAT table only (hostcheck pins the MIDP -1..-7 contract). */
static const FpjRawKey rawmap[] = FPJ_RAWMAP;

void keytrn_init(void)
{
    uint8_t keymap[64], rkeymap[64];
    int i, flags;

    /* PORTRAIT KEYPAD (2026-11-25 (5), the D-pad fix): fpdoom packs
     * the KEYPAD rotation in sys_data.rotate bits 4..5 — fpmenu's
     * stock "$@ = $@ --rotate 3" sets key=scr=3 — and syscomm.c
     * sys_getkeymap() remaps KEYPAD_UP/DOWN/LEFT/RIGHT (+ the digit
     * grid) through num_turn[key] BEFORE this rawmap translates
     * them, so the D-pad arrived 90-degrees turned while CENTER and
     * the soft keys (never rotated by sys_getkeymap) were correct.
     * fpj_key_rotate() clears ONLY the keypad bits — screen bits
     * 0..1 were already consumed by lcd_init()/fpj_lcd_plan() — so
     * the upright portrait pad maps 1:1: UP=-1 DOWN=-2 LEFT=-3
     * RIGHT=-4 CENTER/FIRE=-5 SOFT1=-6 SOFT2=-7. Non-portrait
     * fallback panels keep fpdoom's rotation (keys match the
     * sideways letterbox). keytrn_init() runs from sys_init()
     * AFTER lcd_appinit(), so fpj_plan is already decided.        */
    sys_data.rotate = (uint8_t)fpj_key_rotate(sys_data.rotate,
                                              fpj_plan.reprogram);
    flags = sys_getkeymap(keymap);
    (void)flags;

    memset(rkeymap, 0, sizeof rkeymap);
    for (i = 0; i < (int)(sizeof rawmap / sizeof rawmap[0]); i++)
        if (rawmap[i].raw < 64)
            rkeymap[rawmap[i].raw] = (uint8_t)rawmap[i].kp;

    fpj_dead_raw[0] = fpj_dead_raw[1] = -1;
    for (i = 0; i < 64; i++) {
        unsigned a = keymap[i];
        sys_data.keytrn[0][i] = a < 64 ? rkeymap[a] : 0;
        sys_data.keytrn[1][i] = 0;       /* ignore power combinations */
        /* raw code the firmware can produce but we ignore (not in
         * rawmap): sys_event DROPS it silently — a dead key. Record
         * the first two so the diag overlay can name them.           */
        if (a < 64 && rkeymap[a] == 0) {
            if (fpj_dead_raw[0] < 0)      fpj_dead_raw[0] = (int)a;
            else if (fpj_dead_raw[1] < 0) fpj_dead_raw[1] = (int)a;
        }
    }
    /* expose the map for host debugging / remap overrides */
    for (i = 0; i < KP_MAX; i++) fp_rawmap[i] = 0;
}

/* one event per call; 1 = event ready (see *pressed), 0 = none        */
int fp_key_event(fp_key_t *key, int *pressed)
{
    int k;
    for (;;) {
        int ev = sys_event(&k);
        if (ev == EVENT_KEYDOWN) { *key = (fp_key_t)k; *pressed = 1; return 1; }
        if (ev == EVENT_KEYUP)   { *key = (fp_key_t)k; *pressed = 0; return 1; }
        return 0;                        /* EVENT_END / EVENT_QUIT     */
    }
}

uint32_t fp_keys_held(void) { return 0; }  /* held-state lives in midp_input */

/* ---- SD file I/O (libc_embed, LIBC_SDIO=3) --------------------------- */
/* fpj_io_pump: the ~35 s Gameloft->Ubisoft splash gap and the long
 * post-splash menu loads are mostly C TIME (SD reads + tinfl inflates
 * per class/resource). The interpreter's DISPATCH pump cannot fire
 * there, so keypad events and repaints used to starve for SECONDS per
 * read — on the phone this looks exactly like "keypad dead, screen
 * frozen". Pump from inside the I/O layer instead, throttled to
 * ~50 Hz: midp_paint_pump is re-entrancy-guarded, and interp_call
 * holds the outer frame chain for the GC, so dispatching keyPressed
 * / paint() from here is as safe as from Thread.sleep.            */
void fpj_io_pump(void)
{
    static u32 last;
    u32 now = fp_time_ms();
    if ((u32)(now - last) < 20) return;
    last = now;
    midp_paint_pump();
}

fp_file *fp_fopen(const char *p, const char *m) { return (fp_file *)fopen(p, m); }
int  fp_fread(void *b, int s, int n, fp_file *f)
{ int r = (int)fread(b, (size_t)s, (size_t)n, (FILE *)f); fpj_io_pump(); return r; }
int  fp_fwrite(const void *b, int s, int n, fp_file *f)
{ return (int)fwrite(b, (size_t)s, (size_t)n, (FILE *)f); }
int  fp_fseek(fp_file *f, long o, int w) { return fseek((FILE *)f, o, w); }
long fp_ftell(fp_file *f) { return ftell((FILE *)f); }
int  fp_fclose(fp_file *f) { return fclose((FILE *)f); }

/* ---- native heap (jar buffers, decoded pixels, class bytes) ---------- */
void *fp_alloc(size_t n)    { return malloc(n ? n : 1); }
void *fp_realloc(void *p, size_t n) { return realloc(p, n); }
void  fp_free(void *p)      { free(p); }

/* ---- libc string surface (fpdoom libc.c / common.c provide these) ---- */
/* Declared in libc_shims.h; if your fpdoom build lacks one of them,
 * add a 5-line fallback here.                                          */

/* ---- USB cast transport (Round 10) ------------------------------------
 * fpdoom's usbio.c is compiled into this very image (SYS_SRCS in
 * build_sc6531.make), so its raw-bulk primitives are linkable here:
 *   usb_read(dst, len, USB_NOWAIT)  pumps the 2 KB RX ring (services
 *                                    EP2 interrupts) and drains it;
 *   usb_write(src, len)             blocking TX on EP3, 64-byte
 *                                    chunks (returns len).
 * Device side (Round 11 cold-boot: fp_usb_init() below):
 *   VID 0x1782 / PID 0x4D00, vendor class 0xFF, 2 endpoints,
 *   EP OUT 0x02 (phone RX) / EP IN 0x83 (phone TX), max packet 64.
 * With LIBC_SDIO=3 nothing else in the image touches USB (the
 * libc_server handshake and _debug_msg are compiled out), so the
 * pipe is exclusively ours while a MIDlet holds it open.
 * Round 11: fp_usb_init() arms the controller from a cold sdboot
 * (see below); we never re-run it under a live link.
 *
 * Round 12 — TIGHT-LOOP BULK RX CONTRACT. fp_usb_read() is one full
 * DRAIN-TO-EMPTY pass: usb_read(dst, len, USB_NOWAIT) loops
 * "usb_check_int() while the ring has >= 64 bytes free, then copy
 * out what arrived" until the ring goes empty, so ONE call services
 * the UDC interrupts (EP0 enumeration included) AND moves every
 * queued byte. The blocking state machine that CALLS this in a
 * tight spin — no fp_sleep_ms while payload is streaming, ~16 ms
 * keypad/abort polling, sleep only before the first byte — is
 * usb_fill() in natives.c (shared verbatim with the host checks,
 * which is why it lives there and not here: the host mock pipe is
 * synchronous and cannot block inside a platform primitive).
 * The old fill loop's fp_sleep_ms(2) per partial drain quantized to
 * a 10-16 ms hardware-timer tick on the SC6531E — 641 packets per
 * frame, >5 s per frame, the 0.2 fps cast and the host ACK
 * timeouts. Never sleep on this path while data is pending.       */
#include "usbio.h"

/* usbio.c keeps its ring private (usbio.h has no struct); the layout
 * is fixed for the fpdoom tree this app builds against: 0x800-byte
 * power-of-two ring + USB_MAXREAD(64) overflow margin. Declared here
 * ONLY to give available() an honest byte count.                     */
#define FPJ_USB_BUFSIZE 0x800
typedef struct { uint32_t rpos, wpos; uint8_t buf[FPJ_USB_BUFSIZE + 64]; } fpj_usb_buf_t;
extern fpj_usb_buf_t usb_buf;

int fp_usb_read(void *dst, unsigned len)
{
    /* single drain-to-empty pass; also services USB interrupts
     * (usb_check_int: EP0/EP2/EP3) while the ring has room — the
     * tight-loop caller (usb_fill) spins this to keep the 2 KB ring
     * from ever wedging full or NAKing EP2 OUT longer than one pass */
    return usb_read(dst, len, USB_NOWAIT);
}

int fp_usb_write(const void *src, unsigned len)
{
    if (!len) return 0;
    return usb_write(src, len) == (int)len ? (int)len : -1;
}

unsigned fp_usb_available(void)
{
    return (unsigned)((usb_buf.wpos - usb_buf.rpos) & (FPJ_USB_BUFSIZE - 1));
}

void fp_usb_reset(void)
{
    /* ring + endpoints belong to the bootloader session; a MIDlet
     * always starts from a drained, consistent state. No-op.          */
}

/* ---- Round 11: cold-boot USB enumeration (1782:4d00) -----------------
 *
 * THE BUG: fpj2me.bin booted from microSD via sdboot never showed up
 * in lsusb/dmesg on Linux. Two independent causes:
 *
 *   (1) usbio.c's comment says it all: "USB is already initialized
 *       by the bootloader" - true for the LIBC_SDIO<3 FDL path (the
 *       host loaded us over USB, so FDL1/ROM left the controller
 *       powered and armed), FALSE for sdboot. entry.c arms the UDC
 *       only inside `#if LIBC_SDIO < 3`; with LIBC_SDIO=3 nothing in
 *       the whole image ever touches USB.
 *
 *   (2) Even "just call usb_init()" is not enough: cold, the SC6531E
 *       USB island is POWERED DOWN. AHB power gate off, and the
 *       LDO_USB33 that biases the PHY and the 1.5 k D+ pull-up is
 *       down - no pull-up, no attach, and the host sees literally
 *       nothing (not even an unknown device).
 *
 * THE FIX: replicate the OEM firmware's own UDC cold init. Recovered
 * from the phone's flash.bin (XIP NOR dump, vaddr == file offset;
 * UDC init at 0x29FFA, LDO helper at 0x29B04) and cross-checked
 * against the leaked SC6531E/M Device Specification:
 *
 *   [0x20501080] = 0x10        AHB power-enable SET, bit 4 = USB
 *                               island (DS 5.2.1: USB space lives on
 *                               the AHB matrix at 0x90000000)
 *   [0x8B000220] |= 0x20       GR module-control bit 5, set once and
 *                               left set by the stock init
 *   ...300 ms settle delay...   stock fw waits before touching UDC
 *   [0x90000000] |= 2 then 1    SELF_PWR, then USB_EN (DS 6.1.4.2.1:
 *                               "software should write 1 [to SELF_PWR]
 *                               since our USB device is always
 *                               self-powered")
 *   arm EP0/EP2/EP3             max-packet 8/64/64, int enables,
 *                               buffer-ready, EP-enable, TIMEOUT_LMT
 *                               = IDENTICAL register writes to
 *                               fpdoom's usb_init() (and to stock:
 *                               0x29B2A/0x29B66/0x29BC6/0x29C0C/0x29C56)
 *   LDO_USB_REG0.0 = 1 then 0   power-cycle: keep the host blind
 *                               while re-arming, then power on. DS
 *                               13.1.1.32: LDO_USB_REG0 lives on the
 *                               analog die at Glb-reg offset 0x7C,
 *                               ARM-side ADI window 0x82001400+, so
 *                               its ARM address is 0x8200147C; bit 0
 *                               LDO_USB_PD is "1: power down, 0:
 *                               power on". Clearing it powers D+ ->
 *                               the host sees the attach.
 *
 * The stock fw does the LDO power-down FIRST (soft-disconnect while
 * reconfiguring) and powers on LAST - we keep that order but only
 * on the truly-cold path: if USB_CTRL.USB_EN already reads 1 we are
 * the FDL session's guest and must not disturb a live link (the
 * AHB-power/GR writes are idempotent set/RMW ops, safe to repeat).
 *
 * ENUMERATION SERVICE: usbio.c only pumps interrupts from
 * usb_read()/usb_getchar(). A host enumerating while the UI idles
 * (menu, MIDlet event wait) would time out reading descriptors - so
 * fpj_usb_pump() runs a 1-byte NON-blocking drain on every
 * fp_sleep_ms() while the ring is empty (an empty-ring read steals
 * nothing: usb_read() services EP0/EP2/EP3 interrupts, finds 0
 * queued bytes and returns 0; if the host sent anything mid-pump it
 * stays queued - never stolen, see the ring-guard).           */
#define FPJ_AHB_USB_PWR   0x20501080u /* AHB power-en SET (SC6531E)  */
#define FPJ_GR_USB_CTRL   0x8B000220u /* GR module control           */
#define FPJ_USB_CTRL_REG  0x90000000u /* UDC USB_CTRL                */
#define FPJ_ADI_LDO_USB   0x8200147Cu /* LDO_USB_REG0 (analog die)   */

static int fpj_usb_up;   /* 0 = cold, 1 = up (brought up or adopted) */

int fp_usb_init(void)
{
    if (fpj_usb_up) return 0;      /* idempotent                      */

    /* USB island power + module control. The SET register and the
     * RMW are idempotent, so these two are safe even when a live
     * FDL session already owns the controller.                     */
    MEM4(FPJ_AHB_USB_PWR) = 1u << 4;
    MEM4(FPJ_GR_USB_CTRL) = MEM4(FPJ_GR_USB_CTRL) | (1u << 5);

    /* island settle (the OEM UDC init waits 300 ms right here)     */
    sys_wait_ms(300);

    /* already armed? (LIBC_SDIO<3: entry.c ran usb_init() and the
     * FDL host session is live) - adopt, touch nothing else.       */
    if (MEM4(FPJ_USB_CTRL_REG) & 1u) {
        fpj_usb_up = 1;
        return 0;
    }

    /* cold: soft-disconnect (LDO off) while we (re)arm             */
    adi_write(FPJ_ADI_LDO_USB, adi_read(FPJ_ADI_LDO_USB) | 1u);

    /* controller: self-powered, then enable (stock order)          */
    MEM4(FPJ_USB_CTRL_REG) = MEM4(FPJ_USB_CTRL_REG) | (1u << 1);
    MEM4(FPJ_USB_CTRL_REG) = MEM4(FPJ_USB_CTRL_REG) | 1u;

    /* arm EP0 (8) / EP2 OUT 0x02 (64) / EP3 IN 0x83 (64) + timeouts:
     * fpdoom's own sequence, byte-for-byte the stock fw's, and the
     * one already proven on this phone over the FDL link. Also
     * zeroes the 2 KB RX ring.                                      */
    usb_init();

    /* LDO on: D+ pull-up powered -> the host sees 1782:4d00        */
    adi_write(FPJ_ADI_LDO_USB, adi_read(FPJ_ADI_LDO_USB) & ~1u);

    fpj_usb_up = 1;
    return 0;
}

static void fpj_usb_pump(void)
{
    if (!fpj_usb_up) return;
    if (fp_usb_available() != 0) return;  /* data pending: the reader
                                           * owns the ring (a 1-byte
                                           * drain could steal one) */
    {
        uint8_t t;
        usb_read(&t, 1, USB_NOWAIT);     /* services EP0/EP2/EP3    */
    }
}
