/*
 * fpj2me — fp_menu.h : in-app game selection menu (target only).
 *
 * fpj2me.bin launched from fpmenu shows this menu instead of booting a
 * fixed "game.jad": it lists every *.jar in the working directory that
 * fpmenu's --dir option selected (games/fpj2me/), lets the user pick
 * one with up/down + OK, and returns the jar's file name.
 */
#ifndef FPJ2ME_FP_MENU_H
#define FPJ2ME_FP_MENU_H

/* Run the menu. Returns:
 *   1  a jar was selected; its file name (relative to the working
 *      directory, e.g. "ac.jar") is copied into sel_out (NUL-terminated)
 *   0  user picked <exit> / pressed SOFT2 — caller should exit(0)
 * Never returns on a fatal draw error (there are none — drawing is
 * writing into fp_screen).                                                */
int fp_menu_run(char *sel_out, int sel_sz);

/* number of *.jar files found by the last fp_menu_run() scan (exposed
 * for status screens / tests) */
int fp_menu_last_count(void);

#endif /* FPJ2ME_FP_MENU_H */
