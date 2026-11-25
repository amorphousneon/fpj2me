/*
 * fpj2me — midp_input.c : keypad mapping + MIDlet/Canvas lifecycle pump.
 *
 * Event flow (one call to midp_frame_pump() per frame):
 *   sys_event -> raw codes -> midp keycodes -> Java keyPressed/
 *   keyRepeated/keyReleased dispatch (through the native bridge),
 *   synthesized repeats after 500 ms / 150 ms cadence.
 */
#include "midp.h"
#include "libc_shims.h"

/* ---- fpdoom raw key -> MIDP keycode ---------------------------------
 * One entry per physical key. Fill in the *raw* column from a one-time
 * dump of sys_event on your handset. Values here are the it2163 defaults
 * as fpdoom reports them — VERIFY before use (see README M2 step 3). */
typedef struct { fp_key_t hw; int midp; } KeyMap;
static const KeyMap keymap[] = {
    { KP_UP,     ACT_UP },    { KP_DOWN,   ACT_DOWN },
    { KP_LEFT,   ACT_LEFT },  { KP_RIGHT,  ACT_RIGHT },
    { KP_CENTER, ACT_FIRE },  { KP_SOFT1,  SOFT1 },
    { KP_SOFT2,  SOFT2 },    { KP_1,      KEY_NUM1 },
    { KP_2,      KEY_NUM2 }, { KP_3,      KEY_NUM3 },
    { KP_4,      KEY_NUM4 }, { KP_5,      KEY_NUM5 },
    { KP_6,      KEY_NUM6 }, { KP_7,      KEY_NUM7 },
    { KP_8,      KEY_NUM8 }, { KP_9,      KEY_NUM9 },
    { KP_0,      KEY_NUM0 }, { KP_STAR,   KEY_STAR },
    { KP_POUND,  KEY_POUND },{ KP_VOLUP,  ACT_GAME_A },
    { KP_VOLDN,  ACT_GAME_B },{ KP_SIDE,   ACT_GAME_C },
};
#define KEYMAP_N (int)(sizeof keymap / sizeof keymap[0])

/* Canvas.getGameAction(): MUST return the official JSR-118 action
 * codes (Canvas.UP==1, LEFT==2, RIGHT==5, DOWN==6, FIRE==8,
 * GAME_A..D==9..12). Gameloft code compares the result against those
 * Canvas constants, which javac inlined at build time. Digits 2/4/6/8/5
 * double as the nav pad; soft keys and star/pound have no game action. */
int input_get_game_action(int k)
{
    switch (k) {
    case KEY_NUM2: return GAME_UP;     case KEY_NUM8: return GAME_DOWN;
    case KEY_NUM4: return GAME_LEFT;   case KEY_NUM6: return GAME_RIGHT;
    case KEY_NUM5: return GAME_FIRE;
    case ACT_UP:   return GAME_UP;     case ACT_DOWN: return GAME_DOWN;
    case ACT_LEFT: return GAME_LEFT;   case ACT_RIGHT: return GAME_RIGHT;
    case ACT_FIRE: return GAME_FIRE;
    case ACT_GAME_A: return GAME_A; case ACT_GAME_B: return GAME_B;
    case ACT_GAME_C: return GAME_C; case ACT_GAME_D: return GAME_D;
    default: return 0;              /* 0 = IllegalArgumentException
                                       in MIDP; games avoid it */
    }
}

/* GameCanvas.getKeyStates(): MIDP semantics — a bit is set if the key
 * is currently down OR was down at any point since the previous call.
 * input_pump() ORs live state into ks_latch; reading clears it.      */
static u32 ks_latch;

static u32 ks_current(void)
{
    u32 s = 0;
    if (input_is_down(ACT_UP))     s |= KS_UP;
    if (input_is_down(ACT_DOWN))   s |= KS_DOWN;
    if (input_is_down(ACT_LEFT))   s |= KS_LEFT;
    if (input_is_down(ACT_RIGHT))  s |= KS_RIGHT;
    if (input_is_down(ACT_FIRE))   s |= KS_FIRE;
    if (input_is_down(ACT_GAME_A)) s |= KS_GAME_A;
    if (input_is_down(ACT_GAME_B)) s |= KS_GAME_B;
    if (input_is_down(ACT_GAME_C)) s |= KS_GAME_C;
    if (input_is_down(ACT_GAME_D)) s |= KS_GAME_D;
    /* digits 2/4/6/8/5 double as the nav pad (getGameAction says so,
     * so getKeyStates must too — one action, several physical keys)  */
    if (input_is_down(KEY_NUM2))   s |= KS_UP;
    if (input_is_down(KEY_NUM8))   s |= KS_DOWN;
    if (input_is_down(KEY_NUM4))   s |= KS_LEFT;
    if (input_is_down(KEY_NUM6))   s |= KS_RIGHT;
    if (input_is_down(KEY_NUM5))   s |= KS_FIRE;
    return s;
}

int input_get_key_states(void)
{
    u32 s = ks_latch | ks_current();
    ks_latch = 0;
    return (int)s;
}

/* action bits a single MIDP keycode contributes (0 if none) — used to
 * latch taps that are pressed AND released between two polls        */
static u32 ks_key_bits(int midp)
{
    switch (midp) {
    case ACT_UP:    case KEY_NUM2: return KS_UP;
    case ACT_DOWN:  case KEY_NUM8: return KS_DOWN;
    case ACT_LEFT:  case KEY_NUM4: return KS_LEFT;
    case ACT_RIGHT: case KEY_NUM6: return KS_RIGHT;
    case ACT_FIRE:  case KEY_NUM5: return KS_FIRE;
    case ACT_GAME_A: return KS_GAME_A; case ACT_GAME_B: return KS_GAME_B;
    case ACT_GAME_C: return KS_GAME_C; case ACT_GAME_D: return KS_GAME_D;
    default: return 0;
    }
}

/* ---- held-key state + event queue ----------------------------------- */
u32 fpj_key_events;                    /* vitals: queued press/release/repeat */
#define EVQ_MASK 63
/* midp is a FULL int, NOT u8: MIDP device keycodes are NEGATIVE for the
 * nav pad (-1..-5), soft keys (-6/-7) and GAME_A..D (-9..-12); a u8
 * field turned SOFT1(-6) into 250, so keyPressed(250) reached the
 * game and f.w(250) mapped to bit 0 — soft keys and D-pad were dead
 * (digits survived, which is why keypad-spam still "worked").        */
static struct { int midp; u8 down; u32 t; } evq[EVQ_MASK + 1];
static int evq_head, evq_tail;
static u32 held_bits[4];              /* 128-bit held set, indexed by   */
                                      /* (midp & 0x7F)                  */
static u32 rep_next[128];             /* next repeat time per key       */
#define REPEAT_DELAY 500
#define REPEAT_RATE  150

static void held_set(int midp, int on)
{
    int b = midp & 127;
    held_bits[b >> 5] = on ? (held_bits[b >> 5] | (1u << (b & 31)))
                           : (held_bits[b >> 5] & ~(1u << (b & 31)));
}

int input_is_down(int midp)
{
    int b = midp & 127;
    return (held_bits[b >> 5] >> (b & 31)) & 1;
}

static void evq_push(int midp, int down)
{
    int nt = (evq_tail + 1) & EVQ_MASK;
    if (nt == evq_head) return;                      /* drop on overflow */
    fpj_key_events++;          /* vitals: keys actually reaching the queue */
    evq[evq_tail].midp = midp;
    evq[evq_tail].down = (u8)down;
    evq[evq_tail].t    = fp_time_ms();
    evq_tail = nt;
}

int input_get_key(int *keycode)
{
    if (evq_head == evq_tail) return 0;
    *keycode = evq[evq_head].midp;
    int down = evq[evq_head].down;
    evq_head = (evq_head + 1) & EVQ_MASK;
    return down == 0 ? 2 : down == 2 ? 3 : 1;  /* 1 press 2 release 3 repeat */
}

void input_pump(void)
{
    fp_key_t k; int pressed;
    while (fp_key_event(&k, &pressed)) {
        for (int i = 0; i < KEYMAP_N; i++) {
            if (keymap[i].hw != k) continue;
            int midp = keymap[i].midp;
            held_set(midp, pressed);
            evq_push(midp, pressed);
            ks_latch |= ks_key_bits(midp);   /* latch even instant taps  */
            if (pressed) rep_next[midp & 127] = fp_time_ms() + REPEAT_DELAY;
            break;
        }
    }
    /* synthesize keyRepeated while held                                */
    u32 now = fp_time_ms();
    ks_latch |= ks_current();          /* latch for getKeyStates()       */
    for (int b = 0; b < 128; b++) {
        if (!(input_is_down(b))) continue;
        if ((int)rep_next[b] >= 0 && (s32)(now - rep_next[b]) >= 0) {
            evq_push(b, 2);                          /* tag: repeat       */
            rep_next[b] = now + REPEAT_RATE;
        }
    }
}

void input_key_released_flush(void) {}

/* fpj2me_shutdown(): drop every held/queued key so the menu (and the
 * next suite) starts from a clean input state                           */
void input_reset(void)
{
    ks_latch = 0;
    evq_head = evq_tail = 0;
    memset(held_bits, 0, sizeof held_bits);
    memset(rep_next, 0, sizeof rep_next);
}

/* weak no-op bridges — natives.c overrides with real Java dispatch */
__attribute__((weak)) void bridge_key_pressed(int midp)  { (void)midp; }
__attribute__((weak)) void bridge_key_released(int midp) { (void)midp; }
__attribute__((weak)) void bridge_key_repeated(int midp) { (void)midp; }
__attribute__((weak)) void bridge_command(int softkey)   { (void)softkey; }

/* ---- main frame pump ------------------------------------------------ */
void midp_frame_pump(void)
{
    input_pump();
    media_pump();                       /* deliver queued media events   */
    int k, ev;
    while ((ev = input_get_key(&k))) {
        if (ev == 1)      bridge_key_pressed(k);
        else if (ev == 3) bridge_key_repeated(k);
        else              bridge_key_released(k);
    }
}
