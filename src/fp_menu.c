/*
 * fpj2me — fp_menu.c : minimal on-phone game picker (TARGET ONLY —
 * pulls in fpdoom's microfat; not compiled by tests/run.sh).
 *
 * Layout on the 128x160 vertical window (8x8 font, 16 cols x 20 rows):
 *   row 0      title "fpj2me" (left) + "<sel>/<total>" (right)
 *   row 1      thin horizontal divider
 *   rows 2..18 jar list, 17 visible, vertical scroll + right scrollbar
 *   row 19     footer: "<N> games" left, panel w1xh1 right (diagnostic)
 *
 * Keys:  UP/2 = up   DOWN/8 = down (auto-repeat, wraps at ends)
 *        OK/5/LSOFT = launch   RSOFT = exit
 *        last entry is "< exit >"
 */
#include <stdio.h>
#include <string.h>
#include "syscode.h"        /* fpdoom HAL: sys_event, sys_data          */
#include "platform.h"       /* fp_key_event / keys                      */
#include "fp_font.h"
#include "fp_menu.h"

#include "fatfile.h"        /* microfat.h + fatdata_glob                */

#define MENU_MAX      64
#define MENU_NAME_MAX 28

/* ---- colors (RGB565) ------------------------------------------------- */
#define C_BG      0x0000      /* black background                         */
#define C_TITLE   0xFFFF      /* white                                    */
#define C_TEXT    0xFFFF      /* white (all unselected text)              */
#define C_SEL_BG  0x29F5      /* selection bar: deep blue-teal            */
#define C_SEL_FG  0xFFFF      /* white (selected text)                    */
#define C_FOOT    0xFFFF      /* white (footer)                           */
#define C_TRACK   0x3186      /* dark gray scrollbar track / divider      */

#define LIST_ROW0 2                                   /* first list row   */
#define FOOT_ROW  ((FP_LCD_H / FP_FONT_H) - 1)        /* row 19 on 160px  */
#define LIST_ROWS (FOOT_ROW - LIST_ROW0)              /* 17 rows (2..18)  */

typedef struct {
    char names[MENU_MAX][MENU_NAME_MAX];
    int  n;
} MenuList;

static MenuList ml;
static int menu_last_count;

int fp_menu_last_count(void) { return menu_last_count; }

/* ---- scan: every non-dir *.jar in the current directory ------------- */
static int ends_with_jar(const char *name, int len)
{
    if (len < 5) return 0;                       /* "a.jar" is 5 chars  */
        const char *e = name + len - 4;
    return (e[0] | 32) == '.' && (e[1] | 32) == 'j'
    && (e[2] | 32) == 'a' && (e[3] | 32) == 'r';
}

static int scan_cb(void *ud, fat_entry_t *ent, const char *name)
{
    MenuList *m = (MenuList *)ud;
    if (m->n >= MENU_MAX) return 1;              /* stop enumeration    */
        if (ent->entry.attr & FAT_ATTR_DIR) return 0;
        int len = (int)strlen(name);
    if (len >= MENU_NAME_MAX || !ends_with_jar(name, len)) return 0;
    strcpy(m->names[m->n++], name);
    return 0;
}

static int cmp_names(const void *pa, const void *pb)
{
    const char *a = (const char *)pa, *b = (const char *)pb;
    while (*a && *b) {
        int d = (*a | 32) - (*b | 32);           /* case-insensitive    */
        if (d) return d;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void menu_scan(void)
{
    memset(&ml, 0, sizeof ml);
    unsigned clust = fat_dir_clust(&fatdata_glob, ".");
    if (clust) fat_enum_name(&fatdata_glob, clust, scan_cb, &ml);

    if (ml.n > 1) {                              /* insertion sort      */
        char tmp[MENU_NAME_MAX];
        for (int i = 1; i < ml.n; i++) {
            strcpy(tmp, ml.names[i]);
            int j = i;
            while (j > 0 && cmp_names(ml.names[j - 1], tmp) > 0) {
                strcpy(ml.names[j], ml.names[j - 1]);
                j--;
            }
            strcpy(ml.names[j], tmp);
        }
    }
    menu_last_count = ml.n;
}

/* ---- drawing ---------------------------------------------------------- */
/* display name: basename without ".jar"; SFN-only-uppercase names are
 * lowercased for looks ("AC.JAR" -> "ac", "Gangstar.jar" -> "Gangstar") */
static void display_name(const char *jar, char *out, int outsz)
{
    int len = (int)strlen(jar);
    if (len >= 4 && (jar[len - 4] | 32) == '.') len -= 4;  /* strip .jar */
        if (len >= outsz) len = outsz - 1;

        int any_lower = 0;
    for (int i = 0; i < len; i++)
        if (jar[i] >= 'a' && jar[i] <= 'z') { any_lower = 1; break; }

        for (int i = 0; i < len; i++) {
            char c = jar[i];
            if (!any_lower && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[i] = c;
        }
        if (len == 0) { out[0] = '?'; len = 1; }     /* e.g. ".jar"          */
            out[len] = 0;
}

static void draw_menu(int sel, int top)
{
    char buf[20];
    int total = ml.n + 1;                        /* jars + <exit>        */
    int has_scroll = (total > LIST_ROWS);
    int list_w = has_scroll ? (FP_LCD_W - 4) : FP_LCD_W;
    int max_chars = (list_w - 4) / FP_FONT_W;
    if (max_chars > 14) max_chars = 14;
    if (max_chars < 1)  max_chars = 1;

    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, C_BG);

    /* header row 0: title left, selection position right                */
    fp_draw_text(2, 0, "fpj2me", C_TITLE);
    snprintf(buf, sizeof buf, "%d/%d", sel + 1, total);
    fp_draw_text_r(FP_LCD_W - 2, 0, buf, C_TITLE);

    /* subtle divider line in row 1                                      */
    fp_fill_rect(0, FP_FONT_H + 3, FP_LCD_W, 1, C_TRACK);

    /* vertical scrollable list: rows LIST_ROW0 .. FOOT_ROW - 1          */
    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = top + i;
        int y   = (LIST_ROW0 + i) * FP_FONT_H;
        const char *label;

        if (idx >= total) break;

        if (idx == ml.n) {
            label = "< exit >";
        } else {
            display_name(ml.names[idx], buf, max_chars + 1);
            label = buf;
        }

        if (idx == sel) {
            fp_fill_rect(0, y, list_w, FP_FONT_H, C_SEL_BG);
            fp_draw_text(2, y, label, C_SEL_FG);
        } else {
            fp_draw_text(2, y, label, C_TEXT);
        }
    }

    /* right-edge vertical scrollbar when list overflows screen          */
    if (has_scroll) {
        int track_y = LIST_ROW0 * FP_FONT_H;
        int track_h = LIST_ROWS * FP_FONT_H;
        int max_top = total - LIST_ROWS;
        int thumb_h = (track_h * LIST_ROWS) / total;
        if (thumb_h < 6) thumb_h = 6;
        int thumb_y = track_y + ((track_h - thumb_h) * top) / max_top;

        fp_fill_rect(FP_LCD_W - 3, track_y, 2, track_h, C_TRACK);
        fp_fill_rect(FP_LCD_W - 3, thumb_y, 2, thumb_h, C_TEXT);
    }

    /* footer row: game count left + panel geometry right                */
    snprintf(buf, sizeof buf, "%d game%s", ml.n, ml.n == 1 ? "" : "s");
    fp_draw_text(2, FOOT_ROW * FP_FONT_H, buf, C_FOOT);
    snprintf(buf, sizeof buf, "%dx%d",
             (int)sys_data.display.w1, (int)sys_data.display.h1);
    fp_draw_text_r(FP_LCD_W - 2, FOOT_ROW * FP_FONT_H, buf, C_FOOT);
}

/* ---- input with auto-repeat ------------------------------------------ */
static int menu_move(int sel, int d)
{
    int last = ml.n;                              /* + <exit> entry       */
    if (last <= 0) return 0;
    if (d == -1 && sel == 0)    return last;      /* wrap top -> bottom   */
        if (d == +1 && sel == last) return 0;         /* wrap bottom -> top   */
            sel += d;
    if (sel < 0)    sel = 0;
    if (sel > last) sel = last;
    return sel;
}

/* ---- no-jar warning: shown when the scan finds nothing ---------------- */
static void no_jars_screen(void)
{
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0xF800);        /* red       */
    fp_draw_text(2, 1 * FP_FONT_H, "no .jar files", 0xFFFF);
    fp_draw_text(2, 2 * FP_FONT_H, "in the game dir", 0xFFFF);
    fp_draw_text(2, 4 * FP_FONT_H, "check:", 0x07FF);
    fp_draw_text(2, 5 * FP_FONT_H, "1 .jar in", 0xFFFF);
    fp_draw_text(2, 6 * FP_FONT_H, "  games/fpj2me", 0xFFFF);
    fp_draw_text(2, 7 * FP_FONT_H, "2 config.txt:", 0xFFFF);
    fp_draw_text(2, 8 * FP_FONT_H, "  line 1 must be", 0xFFFF);
    fp_draw_text(2, 9 * FP_FONT_H, "  --bright 50 ...", 0xFFFF);
    fp_draw_text(2, 10 * FP_FONT_H, "  NOT $fpj2me=...", 0xFFFF);
    fp_draw_text(2, 12 * FP_FONT_H, "panel:", 0xFFFF);
    {
        char buf[16];
        snprintf(buf, sizeof buf, "%dx%d",
                 (int)sys_data.display.w1, (int)sys_data.display.h1);
        fp_draw_text(2 + 7 * FP_FONT_W, 12 * FP_FONT_H, buf, 0xFFFF);
    }
    fp_draw_text(2, 17 * FP_FONT_H, "press any key", 0x07FF);
    fp_lcd_flush();

    fp_key_t k; int pressed;
    for (;;) {
        while (fp_key_event(&k, &pressed))
            if (pressed) return;
            fp_sleep_ms(20);
    }
}

int fp_menu_run(char *sel_out, int sel_sz)
{
    int sel = 0, top = 0, redraw = 1;
    int hold = 0;                                 /* -1 up, +1 down       */
    uint32_t hold_since = 0, rep_next = 0;

    menu_scan();
    sel_out[0] = 0;
    if (ml.n == 0) {
        no_jars_screen();
        return 0;
    }

    for (;;) {
        if (redraw) {
            int total = ml.n + 1;
            int max_top = total - LIST_ROWS;
            if (max_top < 0) max_top = 0;

            if (sel < top) top = sel;
            if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
            if (top > max_top) top = max_top;
            if (top < 0) top = 0;

            draw_menu(sel, top);
            redraw = 0;
        }

        fp_lcd_flush();                           /* show this frame      */
        fp_sleep_ms(10);
        fp_lcd_wait();                            /* previous flip done   */

        fp_key_t k; int pressed;
        while (fp_key_event(&k, &pressed)) {
            if (!pressed) {
                if ((k == KP_UP || k == KP_2) && hold < 0) hold = 0;
                if ((k == KP_DOWN || k == KP_8) && hold > 0) hold = 0;
                continue;
            }
            switch (k) {
                case KP_UP: case KP_2:
                    sel = menu_move(sel, -1);
                    hold = -1; hold_since = rep_next = fp_time_ms();
                    redraw = 1; break;
                case KP_DOWN: case KP_8:
                    sel = menu_move(sel, +1);
                    hold = +1; hold_since = rep_next = fp_time_ms();
                    redraw = 1; break;
                case KP_CENTER: case KP_5: case KP_SOFT1:
                    if (sel < ml.n) {
                        strncpy(sel_out, ml.names[sel], (size_t)sel_sz - 1);
                        sel_out[sel_sz - 1] = 0;
                        return 1;
                    }
                    return 0;                         /* <exit> selected      */
                case KP_SOFT2:
                    return 0;
                default: break;
            }
        }

        /* auto-repeat after 350 ms, then every 150 ms                  */
        if (hold) {
            uint32_t now = fp_time_ms();
            if ((int32_t)(now - rep_next) >= 0) {
                if (now - hold_since < 350) rep_next = hold_since + 350;
                else {
                    sel = menu_move(sel, hold);
                    rep_next = now + 150;
                    redraw = 1;
                }
            }
        }
    }
}
