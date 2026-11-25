/*
 * fpj2me — midp_graphics.c : the 2D core. All Gameloft 2D traffic funnels
 * through drawRegion (sprite sheets + TRANS_*) and fillRect, so those two
 * are written for speed (row-copies for axis-aligned cases, per-pixel
 * inverse mapping for rotations). Replace the two hot loops with ARMv5
 * asm (ldmia/stmia) in M4 if profiling demands it.
 *
 * Transform math — iterate dst (dx,dy) and map back to src (u,v):
 *   NONE          : u=dx       v=dy        dims (w,h)
 *   MIRROR        : u=w-1-dx   v=dy        dims (w,h)
 *   ROT180        : u=w-1-dx   v=h-1-dy    dims (w,h)
 *   MIRROR_ROT180 : u=dx       v=h-1-dy    dims (w,h)   (flip vertical)
 *   ROT90 (cw)    : u=dy       v=h-1-dx    dims (h,w)
 *   ROT270        : u=w-1-dy   v=dx        dims (h,w)
 *   MIRROR_ROT90  : u=w-1-dy   v=h-1-dx    dims (h,w)
 *   MIRROR_ROT270 : u=dy       v=dx        dims (h,w)   (transpose)
 */
#include "midp.h"
#include "libc_shims.h"

/* Public-domain 8x8 glyph table, ASCII 32..127 (font8x8_basic,
 * vendored as vendor/font8x8.h). Swap in fpdoom's own font if it
 * ships one — keep the same 1-byte-per-row layout.                    */
#include "font8x8.h"

void gfx_reset(MGraphics *g, JImage *target)
{
    g->target = target;
    g->cx = 0; g->cy = 0; g->cw = target->w; g->ch = target->h;
    g->tx = 0; g->ty = 0;
    g->color = 0xFFFF;
}

/* MIDP Graphics.setClip(x,y,w,h): the new clip REPLACES the old one
 * (JSR-118 — "sets the current clip to the rectangle specified"),
 * expressed in translated coordinates and clamped to the destination
 * bounds. It must NOT intersect with the previous clip: GLLib's font
 * engine draws every glyph as setClip(cell) + drawImage(sheet,offset)
 * — with intersect semantics the 2nd..Nth glyph's clip intersected
 * the 1st glyph's cell to EMPTY, so only the first glyph of every
 * drawString ever rasterized (AC1: "DO YOU WANT SOUND?" showed one
 * 'Y', menus showed single stray glyphs, the title looked frozen).
 * clipRect() would be the intersecting API; MIDP games rarely use it. */
void gfx_clip(MGraphics *g, int x, int y, int w, int h)
{
    int x0 = x + g->tx, y0 = y + g->ty;
    int x1 = x0 + w,     y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->target->w) x1 = g->target->w;
    if (y1 > g->target->h) y1 = g->target->h;
    g->cw = (x1 > x0) ? x1 - x0 : 0;
    g->ch = (y1 > y0) ? y1 - y0 : 0;
    g->cx = (short)x0; g->cy = (short)y0;
}

void gfx_translate(MGraphics *g, int dx, int dy)
{
    g->tx += (short)dx; g->ty += (short)dy;
}

void gfx_color_rgb(MGraphics *g, int r, int gg, int b)
{
    u16 px = (u16)(((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3));
    /* pure black must be OPAQUE: 0x0000 is the PX_TRANS color-key, and
     * setColor(0)+fillRect is exactly how GLLib clears its off-screen
     * before blitting it over the screen — with a transparent "black"
     * the blit composites instead of replacing, and every previously
     * drawn frame bleeds through (AC1: the sound-prompt screen kept
     * the copyright text underneath). Same nudge the PNG decoder's
     * pack565() applies; imperceptible on a 65k panel.              */
    g->color = (px == PX_TRANS) ? PX_BLACK : px;
}

void gfx_color_int(MGraphics *g, u32 argb)   /* 0x00RRGGBB */
{
    gfx_color_rgb(g, (int)((argb >> 16) & 255),
                     (int)((argb >> 8) & 255), (int)(argb & 255));
}

void gfx_fill_rect(MGraphics *g, int x, int y, int w, int h)
{
    int x0 = x + g->tx, y0 = y + g->ty;
    int x1 = x0 + w,    y1 = y0 + h;
    if (x0 < g->cx) x0 = g->cx;
    if (y0 < g->cy) y0 = g->cy;
    if (x1 > g->cx + g->cw) x1 = g->cx + g->cw;
    if (y1 > g->cy + g->ch) y1 = g->cy + g->ch;
    if (x1 <= x0 || y1 <= y0) return;

    JImage *t = g->target;
    for (int yy = y0; yy < y1; yy++) {
        u16 *row = t->px + (size_t)yy * t->w + x0;
        u16  c   = g->color;
        /* ARM926 favors few branches over cleverness */
        for (int xx = x1 - x0; xx; xx--) *row++ = c;
    }
}

void gfx_draw_rect(MGraphics *g, int x, int y, int w, int h)
{
    gfx_draw_line(g, x, y, x + w - 1, y);
    gfx_draw_line(g, x, y + h - 1, x + w - 1, y + h - 1);
    gfx_draw_line(g, x, y, x, y + h - 1);
    gfx_draw_line(g, x + w - 1, y, x + w - 1, y + h - 1);
}

void gfx_draw_line(MGraphics *g, int x1, int y1, int x2, int y2)
{
    x1 += g->tx; y1 += g->ty; x2 += g->tx; y2 += g->ty;
    int dx = x2 > x1 ? x2 - x1 : x1 - x2;
    int dy = y2 > y1 ? y2 - y1 : y1 - y2;
    int sx = x2 >= x1 ? 1 : -1, sy = y2 >= y1 ? 1 : -1;
    int err = dx - dy;
    JImage *t = g->target;
    for (;;) {
        if (x1 >= g->cx && y1 >= g->cy &&
            x1 < g->cx + g->cw && y1 < g->cy + g->ch)
            t->px[(size_t)y1 * t->w + x1] = g->color;
        if (x1 == x2 && y1 == y2) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 <  dx) { err += dx; y1 += sy; }
    }
}


/* Graphics.fillTriangle(x1,y1,x2,y2,x3,y3): scanline fill with the
 * vertices sorted by y, x interpolated down the long edge (v1->v3)
 * and the two short edges (v1->v2, v2->v3). AC1's IGP painter fills
 * its button/panel backgrounds with these. Edges are drawn inclusive
 * so adjacent triangles leave no pinholes.                          */
void gfx_fill_triangle(MGraphics *g, int x1, int y1, int x2, int y2,
                       int x3, int y3)
{
    x1 += g->tx; y1 += g->ty;
    x2 += g->tx; y2 += g->ty;
    x3 += g->tx; y3 += g->ty;
    /* sort the three vertices so y1 <= y2 <= y3 */
#define TSORT(A, B) do { if (y##A > y##B) { int t; \
        t = x##A; x##A = x##B; x##B = t; \
        t = y##A; y##A = y##B; y##B = t; } } while (0)
    TSORT(1, 2);
    TSORT(1, 3);
    TSORT(2, 3);
#undef TSORT
    if (y3 < g->cy || y1 >= g->cy + g->ch) return;
    int h13 = y3 - y1;
    if (h13 <= 0) return;          /* degenerate: zero height (line or
                                      point) — no interior to fill      */
    JImage *t = g->target;
    u16 col = g->color;
    for (int yy = y1; yy <= y3; yy++) {
        if (yy < g->cy) continue;
        if (yy >= g->cy + g->ch) break;
        int xa = x1 + (x3 - x1) * (yy - y1) / h13;
        int xb;
        if (yy < y2) {
            int seg = y2 - y1;
            xb = seg ? x1 + (x2 - x1) * (yy - y1) / seg : x1;
        } else {
            int seg = y3 - y2;
            xb = seg ? x2 + (x3 - x2) * (yy - y2) / seg : x2;
        }
        int xl = xa < xb ? xa : xb;
        int xr = xa < xb ? xb : xa;
        if (xr < g->cx || xl >= g->cx + g->cw) continue;
        if (xl < g->cx) xl = g->cx;
        if (xr >= g->cx + g->cw) xr = g->cx + g->cw - 1;
        u16 *row = t->px + (size_t)yy * t->w;
        for (int xx = xl; xx <= xr; xx++) row[xx] = col;
    }
}

/* resolve anchor against image size (MIDP semantics)                  */
static void anchor_xy(int *x, int *y, int w, int h, int anchor)
{
    if (anchor & GA_HCENTER) *x -= w / 2;
    if (anchor & GA_RIGHT)   *x -= w;
    if (anchor & GA_VCENTER) *y -= h / 2;
    if (anchor & GA_BOTTOM)  *y -= h;
    if (anchor & GA_BASELINE)*y -= h;      /* approx: font has no metrics */
}

void gfx_draw_image(MGraphics *g, const JImage *im,
                    int x, int y, int anchor)
{
    anchor_xy(&x, &y, im->w, im->h, anchor);
    gfx_draw_region(g, im, 0, 0, im->w, im->h,
                    TRANS_NONE, x, y, GA_TOP | GA_LEFT);
}

void gfx_draw_region(MGraphics *g, const JImage *src,
                     int sx, int sy, int sw, int sh, int transform,
                     int dx, int dy, int anchor)
{
    if (sw <= 0 || sh <= 0) return;           /* MIDP: IllegalArgumentException
                                                 territory — we no-op     */
    int rot = (transform == TRANS_ROT90  || transform == TRANS_ROT270 ||
               transform == TRANS_MIRROR_ROT90 ||
               transform == TRANS_MIRROR_ROT270);
    int dw = rot ? sh : sw, dh = rot ? sw : sh;
    anchor_xy(&dx, &dy, dw, dh, anchor);

    dx += g->tx; dy += g->ty;
    int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (x0 < g->cx) x0 = g->cx;
    if (y0 < g->cy) y0 = g->cy;
    if (x1 > g->cx + g->cw) x1 = g->cx + g->cw;
    if (y1 > g->cy + g->ch) y1 = g->cy + g->ch;
    if (x0 >= x1 || y0 >= y1) return;

    JImage *t = g->target;
    const u16 *sp = src->px;

    /* fast path: untransformed / mirrored => contiguous rows          */
    if (transform == TRANS_NONE || transform == TRANS_MIRROR) {
        int rev = (transform == TRANS_MIRROR);
        /* MIRROR: u = sw-1-(xx-dx); the first VISIBLE dst column (x0)
         * decides the first source column — right-edge clipping is
         * handled by the loop bound, left-edge clipping by this start
         * offset (fix: was x1-1-dx, which ignored a clipped left edge) */
        int srcx0 = sx + (rev ? (sw - 1 - (x0 - dx)) : (x0 - dx));
        for (int yy = y0; yy < y1; yy++) {
            const u16 *s = sp + (size_t)(sy + (yy - dy)) * src->w + srcx0;
            u16 *d = t->px + (size_t)yy * t->w + x0;
            if (!rev) {
                for (int xx = x0; xx < x1; xx++) {
                    u16 p = *s++;
                    if (p != PX_TRANS) *d = p;
                    d++;
                }
            } else {
                for (int xx = x0; xx < x1; xx++) {   /* read px[srcx0] then
                                                   step left with *s-- */
                    u16 p = *s--;
                    if (p != PX_TRANS) *d = p;
                    d++;
                }
            }
        }
        return;
    }

    /* general path: inverse-map each destination pixel                */
    int u, v;
    for (int yy = y0; yy < y1; yy++) {
        u16 *d = t->px + (size_t)yy * t->w + x0;
        int cy = yy - dy;                          /* dst-local coords  */
        for (int xx = x0; xx < x1; xx++, d++) {
            int cx = xx - dx;
            switch (transform) {
            case TRANS_ROT180:        u = sw-1-cx; v = sh-1-cy; break;
            case TRANS_MIRROR_ROT180: u = cx;      v = sh-1-cy; break;
            case TRANS_ROT90:         u = cy;      v = sh-1-cx; break;
            case TRANS_ROT270:        u = sw-1-cy; v = cx;      break;
            case TRANS_MIRROR_ROT90:  u = sw-1-cy; v = sh-1-cx; break;
            case TRANS_MIRROR_ROT270: u = cy;      v = cx;      break;
            default:                  u = cx;      v = cy;      break;
            }
            u16 p = sp[(size_t)(sy + v) * src->w + (sx + u)];
            if (p != PX_TRANS) *d = p;
        }
    }
}

void gfx_draw_rgb(MGraphics *g, const u32 *rgb, int off, int scanlen,
                  int x, int y, int w, int h, int process_alpha)
{
    x += g->tx; y += g->ty;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < g->cx) x0 = g->cx;
    if (y0 < g->cy) y0 = g->cy;
    if (x1 > g->cx + g->cw) x1 = g->cx + g->cw;
    if (y1 > g->cy + g->ch) y1 = g->cy + g->ch;
    if (x0 >= x1 || y0 >= y1) return;

    JImage *t = g->target;
    for (int yy = y0; yy < y1; yy++) {
        u16 *d = t->px + (size_t)yy * t->w + x0;
        const u32 *s = rgb + (size_t)(yy - y) * scanlen + off + (x0 - x);
        for (int xx = x0; xx < x1; xx++, d++, s++) {
            u32 p = *s;
            if (process_alpha && (p >> 24) < 128) continue;
            int r = (p >> 16) & 255, gg = (p >> 8) & 255, b = p & 255;
            u16 c = (u16)(((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3));
            *d = (c == PX_TRANS) ? PX_BLACK : c;   /* opaque black stays opaque */
        }
    }
}

int gfx_char_width(void)  { return 4; }   /* 8x8 font halved to 4x8:  32 */
int gfx_string_height(void){ return 8; }  /* chars per line on 128px LCD */

void gfx_draw_string(MGraphics *g, const char *s,
                     int x, int y, int anchor)
{
    /* NOTE: `s` must already be JSR-8859/UTF-8-decoded to 8859-1 by the
     * native bridge (see README, M2 checklist). ASCII == identical.    */
    int w = (int)strlen(s) * gfx_char_width();
    int h = gfx_string_height();
    anchor_xy(&x, &y, w, h, anchor);
    x += g->tx; y += g->ty;

    JImage *t = g->target;
    for (int i = 0; s[i]; i++) {
        const u8 *glyph = font8x8[(s[i] >= 32 && s[i] < 127) ? s[i] - 32 : 0];
        int gx = x + i * gfx_char_width();
        int gy = y;
        for (int r = 0; r < 8; r++) {              /* 8 rows           */
            u8 bits = glyph[r];
            for (int c = 0; c < 4; c++) {          /* 4 columns (halved) */
                if (!(bits & (0x80 >> c))) continue;
                int px = gx + c, py = gy + r;
                if (px >= g->cx && py >= g->cy &&
                    px < g->cx + g->cw && py < g->cy + g->ch)
                    t->px[(size_t)py * t->w + px] = g->color;
            }
        }
    }
}
