/*
 * fpj2me — fpj_key.h : the PORTRAIT keypad plan (pure, host-testable).
 *
 * THE BUG (itel i2163, real hardware): the D-pad arrived 90-degrees
 * rotated — physical UP sent J2ME LEFT, DOWN sent RIGHT, LEFT sent
 * DOWN, RIGHT sent UP — while the soft keys and CENTER were correct.
 *
 * THE ROOT CAUSE (fpdoom, verified in the tree):
 *   syscode.h      "rotate = key << 4 | screen"
 *   entry.c        --rotate S[,K]: ONE value sets BOTH (key = scr),
 *                    so fpmenu's stock config.txt line
 *                    "$@ = $@ --rotate 3" leaves sys_data.rotate=0x33
 *   syscomm.c      sys_getkeymap() takes rotate = sys_data.rotate>>4&3
 *                    and remaps the raw pad codes through
 *                    num_turn[rotate] BEFORE fpj2me's rawmap ever
 *                    sees them:
 *                      num_turn[3]: UP(0x04)->LEFT(0x06), DOWN->RIGHT,
 *                                   LEFT->DOWN, RIGHT->UP (+ digits)
 *                    num_turn[1]: the +90 table, num_turn[2]: 180.
 *   fp_platform.c  keytrn_init() calls sys_getkeymap() and builds
 *                    sys_data.keytrn[0] from its rawmap — inheriting
 *                    the rotation. KEYPAD_UP..RIGHT (0x04..0x07) and
 *                    the digit grid are the ONLY codes sys_getkeymap
 *                    rotates; LSOFT/RSOFT/CENTER pass through, which
 *                    is exactly the observed symptom.
 *
 * THE FIX: fpj2me always presents an UPRIGHT 128x160 portrait Canvas
 * (fpj_lcd.h re-programs the panel to native portrait), so the pad is
 * physically upright too — the keypad rotation bits must be cleared
 * before sys_getkeymap() runs. The SCREEN bits (0..1) are consumed
 * earlier (lcd_init's MADCTL XOR + fpj_lcd_plan) and pass through
 * untouched, as do the unused bits 2..3/6..7. When the portrait plan
 * did NOT apply (mono/--mac/non-128x160 letterbox fallback), fpdoom's
 * rotation is kept so the pad still matches the rotated letterbox.
 *
 * Like fpj_lcd.h, this header is the DECISION half: pure data and
 * inline functions compiled into BOTH the target (fp_platform.c) and
 * the host checks (tests/hostcheck.c), so the on-phone key contract —
 * physical UP = Canvas UP (-1), DOWN (-2), LEFT (-3), RIGHT (-4),
 * CENTER/OK = FIRE (-5), LSOFT = SOFT1 (-6), RSOFT = SOFT2 (-7) —
 * is unit-tested 1:1 on the PC.
 */
#ifndef FPJ2ME_KEY_H
#define FPJ2ME_KEY_H

#include "platform.h"            /* fp_key_t / KP_*                   */

/* sys_data.rotate layout (fpdoom syscode.h: "rotate = key << 4 | screen") */
#define FPJ_ROT_KEY_MASK 0x30u   /* bits 4..5: keypad rotation        */
#define FPJ_ROT_SCR_MASK 0x03u   /* bits 0..1: screen rotation        */

/*
 * The keypad-rotation decision. Pure function of its inputs:
 *   portrait != 0 -> keypad bits cleared (upright native pad)
 *   portrait == 0 -> fpdoom's value verbatim (letterbox fallback)
 * Screen bits and unused bits always pass through untouched.
 */
static inline unsigned fpj_key_rotate(unsigned rotate, int portrait)
{
        return portrait ? rotate & ~FPJ_ROT_KEY_MASK : rotate;
}

/*
 * fpdoom's rotation tables, copied 1:1 from syscomm.c sys_getkeymap()
 * (index = raw & 3 of the KEYPAD_UP..RIGHT block, then the digit grid
 * 1..9; only those 13 codes are ever rotated). Kept here so the host
 * checks can prove the FULL chain — raw code -> num_turn -> rawmap ->
 * KP_* -> MIDP keycode — exactly as the target builds it.
 */
#define FPJ_NUM_TURN { /* 4 + 9 entries per row, like syscomm.c */      \
        "\004\005\006\007123456789",   /* 0: identity (portrait)  */     \
        "\007\006\004\005369258147",   /* 1: +90                  */     \
        "\005\004\007\006987654321",   /* 2: 180                  */     \
        "\006\007\005\004741852963"    /* 3: -90 (rotate 3!)      */     \
}

/* raw pad code -> KP_* (the it2163 defaults; KEYPAD_* values from
 * fpdoom syscode.h KEYPAD_ENUM). Shared by fp_platform.c
 * keytrn_init() and the host checks — one table, no drift.        */
#define FPJ_RAWMAP {                                                     \
        { 0x04, KP_UP },     { 0x05, KP_DOWN },                          \
        { 0x06, KP_LEFT },   { 0x07, KP_RIGHT },                         \
        { 0x0d, KP_CENTER }, { 0x01, KP_CENTER },  /* DIAL = OK on 4-way */ \
        { 0x08, KP_SOFT1 },  { 0x09, KP_SOFT2 },                         \
        { 0x2a, KP_STAR },   { 0x23, KP_POUND },                         \
        { 0x24, KP_VOLUP },  { 0x25, KP_VOLDN },                         \
        { 0x30, KP_0 }, { 0x31, KP_1 }, { 0x32, KP_2 }, { 0x33, KP_3 },  \
        { 0x34, KP_4 }, { 0x35, KP_5 }, { 0x36, KP_6 }, { 0x37, KP_7 },  \
        { 0x38, KP_8 }, { 0x39, KP_9 },                                  \
}

typedef struct { unsigned raw; fp_key_t kp; } FpjRawKey;

/*
 * One sys_getkeymap()-style translation step (the rotation fpdoom
 * applies on top of the raw code). `rot` is the KEYPAD rotation
 * (sys_data.rotate >> 4 & 3, AFTER fpj_key_rotate()). Codes outside
 * KEYPAD_UP..RIGHT and '1'..'9' are returned verbatim — exactly like
 * syscomm.c, which is why CENTER/LSOFT/RSOFT never rotate.
 */
static inline unsigned fpj_key_numturn(unsigned raw, unsigned rot)
{
        /* [4+9+1]: the 13 payload codes + the string NUL, so the
         * string-literal initializers below are never too long     */
        static const unsigned char num_turn[4][14] = FPJ_NUM_TURN;
        if (raw - 0x04u < 4u)  return num_turn[rot & 3][raw - 0x04u];
        if (raw - 0x31u < 9u)  return num_turn[rot & 3][raw - 0x31u + 4];
        return raw;
}

/*
 * The COMPLETE decision chain for one raw pad code, as the target
 * runs it: sys_data.rotate -> fpj_key_rotate (portrait clear) ->
 * fpdoom's num_turn (what sys_getkeymap returns) -> our rawmap ->
 * KP_*. Returns KP_NONE (0) for unmapped codes — sys_event drops
 * those (the "dead key" audit in fp_platform.c names them).
 */
static inline fp_key_t fpj_key_xlate(unsigned raw, unsigned sys_rotate,
                                     int portrait)
{
        static const FpjRawKey rawmap[] = FPJ_RAWMAP;
        unsigned rot = fpj_key_rotate(sys_rotate, portrait) >> 4 & 3;
        unsigned code = fpj_key_numturn(raw, rot);
        unsigned i;
        for (i = 0; i < sizeof rawmap / sizeof rawmap[0]; i++)
                if (rawmap[i].raw == code) return rawmap[i].kp;
        return KP_NONE;
}

#endif /* FPJ2ME_KEY_H */
