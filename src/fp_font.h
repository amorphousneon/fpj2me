/*
 * fpj2me — fp_font.h : 8x8 text drawing into fp_screen (RGB565).
 *
 * Used by the target launcher menu / status screens (fp_menu.c,
 * target_main.c) and available to host tests. Same glyph table as
 * midp_graphics.c drawString (vendor/font8x8.h, one static const copy
 * per TU — 768 bytes each, fine for -gc-sections builds).
 */
#ifndef FPJ2ME_FP_FONT_H
#define FPJ2ME_FP_FONT_H

#include "platform.h"

#define FP_FONT_W 8
#define FP_FONT_H 8
#define FP_FONT_COLS (FP_LCD_W / FP_FONT_W)   /* 16 on a 128px panel  */
#define FP_FONT_ROWS (FP_LCD_H / FP_FONT_H)   /* 20 on a 160px panel  */

/* 8x8 cell, MSB-first rows — identical to midp_graphics.c. */
void fp_draw_char(int x, int y, int ch, uint16_t fg);

/* Transparent background. Clips to the screen. */
void fp_draw_text(int x, int y, const char *s, uint16_t fg);

/* Clipped solid fill. */
void fp_fill_rect(int x, int y, int w, int h, uint16_t c);

/* Selection bar: fill row with `bg` then draw `s` in `fg` (x in px). */
void fp_draw_text_row(int x, int row, const char *s, uint16_t fg, uint16_t bg);

/* Draw right-aligned in the 16-col grid: x is the RIGHT edge in px. */
void fp_draw_text_r(int x_right, int y, const char *s, uint16_t fg);

#endif /* FPJ2ME_FP_FONT_H */
