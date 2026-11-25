/*
 * fpj2me — fp_font.c : 8x8 glyph blitter into fp_screen.
 * Host- and target-safe: only touches fp_screen (platform.h).
 */
#include "fp_font.h"
#include "font8x8.h"          /* vendor/ — static const font8x8[96][8] */

void fp_draw_char(int x, int y, int ch, uint16_t fg)
{
    if (ch < 32 || ch > 127) ch = '?';
    const unsigned char *g = font8x8[ch - 32];
    for (int r = 0; r < FP_FONT_H; r++, y++) {
        if (y < 0 || y >= FP_LCD_H) continue;
        uint8_t bits = g[r];
        uint16_t *px = fp_screen + y * FP_LCD_W + x;
        for (int c = 0; c < FP_FONT_W; c++, bits <<= 1, px++) {
            int sx = x + c;
            if (sx < 0 || sx >= FP_LCD_W) continue;
            if (bits & 0x80) *px = fg;
        }
    }
}

void fp_draw_text(int x, int y, const char *s, uint16_t fg)
{
    if (!s) return;
    for (; *s; s++, x += FP_FONT_W) fp_draw_char(x, y, *(const uint8_t *)s, fg);
}

void fp_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h;
    if (x1 > FP_LCD_W) x1 = FP_LCD_W;
    if (y1 > FP_LCD_H) y1 = FP_LCD_H;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *px = fp_screen + yy * FP_LCD_W;
        for (int xx = x0; xx < x1; xx++) px[xx] = c;
    }
}

void fp_draw_text_row(int x, int row, const char *s, uint16_t fg, uint16_t bg)
{
    int y = row * FP_FONT_H;
    fp_fill_rect(0, y, FP_LCD_W, FP_FONT_H, bg);
    fp_draw_text(x, y, s, fg);
}

void fp_draw_text_r(int x_right, int y, const char *s, uint16_t fg)
{
    int n = 0;
    while (s[n]) n++;
    fp_draw_text(x_right - n * FP_FONT_W, y, s, fg);
}
