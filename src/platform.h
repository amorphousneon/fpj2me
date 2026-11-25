/*
 * fpj2me — platform.h : the ONLY file that touches the fpdoom HAL.
 * Point each declaration at your working fpdoom primitives (or wrap
 * them in fp_platform.c — 20 lines). Everything else in fpj2me is
 * HAL-agnostic and unit-testable on a PC.
 */
#ifndef FPJ2ME_PLATFORM_H
#define FPJ2ME_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/* ----- LCD ---------------------------------------------------------- */
/* itel it2163 panel: 128x160 RGB565. Override only when porting a jar
 * built for a different screen (e.g. 176x220).                       */
#ifndef FP_LCD_W
#define FP_LCD_W 128
#define FP_LCD_H 160
#endif

/* RGB565, row-major. In fpdoom this is Doom's front buffer reused
 * between frames; alias it here. */
extern uint16_t fp_screen[FP_LCD_W * FP_LCD_H];

/* Full-screen flush wraps fpdoom's sys_start_refresh(); fp_lcd_wait()
 * wraps sys_wait_refresh() (call BEFORE drawing each frame). On the
 * host build fp_lcd_wait is not used (tests draw synchronously).     */
void fp_lcd_flush(void);
void fp_lcd_wait(void);

/* ----- Time (sys_timer_ms / sys_wait_ms) ----------------------------- */
uint32_t fp_time_ms(void);
void     fp_sleep_ms(uint32_t ms);

/* ----- Debug log (DBG_LOG) ------------------------------------------- */
void fp_dbg(const char *s);

/* ----- Keypad (sys_event) --------------------------------------------
 * fpdoom delivers raw up/down events. Fill the table below with the
 * raw codes your keypad produces (dump them once with fp_dbg).
 * KEYPAD_* names follow the it2163 T9/D-pad physical layout.          */
typedef enum {
    KP_NONE = 0,
    KP_UP, KP_DOWN, KP_LEFT, KP_RIGHT, KP_CENTER,   /* D-pad + FIRE    */
    KP_SOFT1, KP_SOFT2,                             /* left/right key  */
    KP_0, KP_1, KP_2, KP_3, KP_4,
    KP_5, KP_6, KP_7, KP_8, KP_9,
    KP_STAR, KP_POUND,
    KP_VOLUP, KP_VOLDN, KP_SIDE,                    /* it2163 side keys*/
    KP_MAX
} fp_key_t;

/* fill in main():  fp_rawmap[KP_UP] = <fpdoom code for D-pad up>; ... */
extern int fp_rawmap[KP_MAX];

/* Poll once per frame; returns bitset of currently held keys. */
uint32_t fp_keys_held(void);
/* Consume one key event this frame; returns key + pressed flag. */
int fp_key_event(fp_key_t *key, int *pressed); /* 1 = event, 0 = none */

/* ----- SD file I/O (libc_embed) --------------------------------------
 * fpdoom's libc_embed gives fopen/fread/fwrite/fseek/fclose over FAT32.
 * If your libc_embed has no ftell(), it is just fseek(cur,0) + tell.  */
typedef void fp_file;

fp_file *fp_fopen(const char *path, const char *mode);
int      fp_fread(void *buf, int size, int n, fp_file *f);
int      fp_fwrite(const void *buf, int size, int n, fp_file *f);
int      fp_fseek(fp_file *f, long off, int whence); /* SEEK_SET/CUR/END */
long     fp_ftell(fp_file *f);
int      fp_fclose(fp_file *f);

/* App dir on SD. Under sdboot the app's working directory IS its game
 * folder (fpdoom finds doom1.wad the same way), so paths are RELATIVE:
 *   boot.jar    - compiled java.lang + java.util subset (M2+)
 *   game.jar    - the Gameloft jar under test
 *   game.jad    - its JAD (MIDlet-1 line is read for the MIDlet class)
 *   rms/<suite>/ - RecordStore files
 * FPJ_SD_ROOT must be "" (relative) or end with a slash (host tests use
 * "/tmp/fpj2me/").                                          */
#ifndef FPJ_SD_ROOT
#define FPJ_SD_ROOT ""
#endif

#endif /* FPJ2ME_PLATFORM_H */
