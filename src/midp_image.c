/*
 * fpj2me — midp_image.c : PNG -> RGB565 via stb_image (STBI_ONLY_PNG,
 * STBI_NO_STDIO), plus mutable images and Image.createImage(src,x,y,
 * w,h,transform) slicing used constantly by Gameloft sprite code.
 *
 * stb is vendored in vendor/ and compiled in exactly one TU:
 */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS              /* bare metal, no TLS runtime: GCC16 arm-none-eabi defaults to emutls and libgcc's emutls.o wants malloc/abort */
#define STBI_FAILURE_STRINGS 0            /* keep strings out of .rodata */
#define STBI_MALLOC(s)     fp_alloc(s)
#define STBI_REALLOC(p,s)  fp_realloc(p,s)
#define STBI_FREE(p)       fp_free(p)
#define STBI_ASSERT(x)     ((void)0)
#include "stb_image.h"

#include "midp.h"
#include "libc_shims.h"

/* RGBA8888 -> RGB565 with hard transparency (alpha < 128 => 0x0000).
 * Opaque black (0x0000) is nudged to 0x0001 so blitters can use a
 * single compare for transparency. */
static u16 pack565(int r, int g, int b)
{
    u16 px = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return px == PX_TRANS ? PX_BLACK : px;
}

JImage *image_from_png(const u8 *data, u32 size)
{
    int w = 0, h = 0, n = 0;
    /* 4 => force RGBA so paletted+alpha and truecolor share one path */
    u8 *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!rgba) return 0;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { stbi_image_free(rgba); return 0; }

    JImage *im = image_from_argb((const u32 *)rgba, w, h, 1);
    stbi_image_free(rgba);
    return im;
}

/* Image.createRGBImage(int[] rgb, w, h, processAlpha) backing: one
 * in-place pass ARGB8888 -> RGB565 + PX_TRANS color-key (0x0000), no
 * intermediate buffers — the Java int[] becomes garbage the moment
 * the native returns, so the collector reclaims it on the next GC
 * (the 4MB-PSRAM memory-lean contract for GL sprite buffers).      */
JImage *image_from_argb(const u32 *argb, int w, int h, int process_alpha)
{
    JImage *im = image_create((u16)w, (u16)h);
    if (!im) return 0;
    im->immutable = 1;
    u32 n = (u32)w * (u32)h;
    for (u32 i = 0; i < n; i++) {
        u32 p = argb[i];
        im->px[i] = (process_alpha && (p >> 24) < 128)
                  ? PX_TRANS
                  : pack565((int)((p >> 16) & 255),
                            (int)((p >> 8) & 255), (int)(p & 255));
    }
    return im;
}

/* All native image buffers (JImage*) are C heap, invisible to the Java
 * GC — they are tracked here so fpj2me_shutdown() can free a game's
 * decoded sprites when the launcher returns to the menu. Beyond
 * IMG_TRACK_MAX live images nothing is tracked (they only leak at
 * reset, not during play). The screen buffer in natives.c is static
 * storage and never goes through image_create, so it is safe.       */
#define IMG_TRACK_MAX 128
static JImage *img_track[IMG_TRACK_MAX];
static int n_img_track;

static void img_track_add(JImage *im)
{
    if (n_img_track < IMG_TRACK_MAX) img_track[n_img_track++] = im;
}

static void img_track_remove(JImage *im)
{
    for (int i = 0; i < n_img_track; i++)
        if (img_track[i] == im) {
            img_track[i] = img_track[--n_img_track];
            return;
        }
}

void image_free_all(void)
{
    for (int i = 0; i < n_img_track; i++) fp_free(img_track[i]);
    n_img_track = 0;
}

JImage *image_create(u16 w, u16 h)
{
    if (w == 0 || h == 0) return 0;             /* MIDP throws here    */
    JImage *im = fp_alloc(sizeof *im + (size_t)w * h * 2);
    if (!im) return 0;
    im->w = w; im->h = h; im->immutable = 0;
    im->px = (u16 *)((u8 *)im + sizeof *im);
    /* JSR-118: "Mutable images are created as blank images containing
     * only white pixels" — NOT transparent zeros. 0x0000 would make the
     * whole buffer PX_TRANS, so the first blit of an uncleared mutable
     * image would composite over whatever was on screen (and any
     * half-initialized frame would bleed through).                 */
    for (u32 i = 0; i < (u32)w * h; i++) im->px[i] = 0xFFFF;
    img_track_add(im);                          /* freed at VM reset   */
    return im;
}

void image_free(JImage *im)
{
    if (!im) return;
    img_track_remove(im);
    fp_free(im);
}

/* createImage(src, x, y, w, h, transform): implemented as a drawRegion
 * into a fresh mutable image — one code path for all 8 transforms.   */
JImage *image_sub(JImage *src, u16 x, u16 y, u16 w, u16 h, int transform)
{
    if (!src || x + w > src->w || y + h > src->h) return 0;
    int dw = (transform == TRANS_ROT90 || transform == TRANS_ROT270 ||
              transform == TRANS_MIRROR_ROT90 ||
              transform == TRANS_MIRROR_ROT270) ? h : w;
    int dh = (dw == (int)h) ? w : h;

    JImage *dst = image_create((u16)dw, (u16)dh);
    if (!dst) return 0;
    dst->immutable = 1;

    MGraphics g; gfx_reset(&g, dst);
    gfx_draw_region(&g, src, x, y, w, h, transform, 0, 0, GA_TOP | GA_LEFT);
    return dst;
}
