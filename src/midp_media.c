/*
 * fpj2me — midp_media.c : javax.microedition.media silent stubs.
 *
 * Rules that keep Gameloft builds from hanging:
 *  1. createPlayer NEVER returns null/throws for supported types — it
 *     returns the shared silent player. Games null-check or catch
 *     MediaException on real handsets and continue silently.
 *  2. Every state transition succeeds immediately (state machine below).
 *  3. PlayerListener events (END_OF_MEDIA, STARTED) are QUEUED and
 *     delivered from media_pump() on the main loop — never fired
 *     synchronously inside start() (reentrancy into Java while the
 *     interpreter frame is mid-call corrupts the operand stack).
 *  4. Manager.playTone(): no-op.
 *  5. The bridge maps volume/setLoopCount/getDuration/getMediaTime to
 *     constants: duration -1 (unknown), media time 0, loop ignored.
 */
#include "midp.h"
#include "libc_shims.h"

void bridge_player_event(int ev);   /* interp.c installs the real one */
__attribute__((weak)) void bridge_player_event(int ev);

typedef struct {
    int   state;
    int   loop_count;        /* stored, ignored                        */
    JRef  listener;          /* PlayerListener ref, 0 = none           */
    char  ctype[24];
} SilentPlayer;

static SilentPlayer sp_singleton;
static int sp_used;

struct MediaEv { JRef player; int ev; };
#define MEDIA_Q 16
static struct MediaEv media_q[MEDIA_Q];
static int mq_head, mq_tail;

void *media_create_player(const char *content_type)
{
    (void)content_type;                       /* "audio/midi", "audio/x-wav" ... */
    memset(&sp_singleton, 0, sizeof sp_singleton);
    sp_singleton.state = PL_UNREALIZED;
    sp_used = 1;
    return &sp_singleton;
}

int media_player_state(void *p)
{
    SilentPlayer *sp = p;
    return sp ? sp->state : PL_CLOSED;
}

int media_player_op(void *p, int op)
{
    SilentPlayer *sp = p;
    if (!sp) return -1;
    switch (op) {
    case 0: if (sp->state == PL_UNREALIZED) sp->state = PL_REALIZED;  break;
    case 1: if (sp->state != PL_CLOSED)     sp->state = PL_PREFETCHED; break;
    case 2: /* start */
        if (sp->state == PL_CLOSED) break;
        sp->state = PL_STARTED;
        /* queue STARTED + END_OF_MEDIA with duration 0 — a silent
         * player "finishes" instantly so menu music loops proceed    */
        if (sp->listener) {
            int nt = (mq_tail + 1) % MEDIA_Q;
            if (nt != mq_head) {
                media_q[mq_tail].player = (JRef)0; /* bridge re-resolves `this` */
                media_q[mq_tail].ev = EV_STARTED;  mq_tail = nt;
                nt = (mq_tail + 1) % MEDIA_Q;
                if (nt != mq_head) {
                    media_q[mq_tail].player = (JRef)0;
                    media_q[mq_tail].ev = EV_END_OF_MEDIA; mq_tail = nt;
                }
            }
        }
        break;
    case 3: if (sp->state == PL_STARTED) sp->state = PL_PREFETCHED; break;
    case 4: sp->state = PL_CLOSED; break;
    }
    return 0;
}

void media_add_listener(void *p, void *listener_ref)
{
    SilentPlayer *sp = p;
    if (sp && sp_used) sp->listener = (JRef)(uintptr_t)listener_ref;
}

void media_pump(void)
{
    while (mq_head != mq_tail) {
        int ev = media_q[mq_head].ev;
        mq_head = (mq_head + 1) % MEDIA_Q;
        bridge_player_event(ev);   /* interp.c: calls listener.update()  */
    }
}

/* fpj2me_shutdown(): the singleton holds a JRef listener and the queue
 * holds JRefs — both dangle after heap_vm_reset(), so drop them first */
void media_reset(void)
{
    memset(&sp_singleton, 0, sizeof sp_singleton);
    sp_used = 0;
    mq_head = mq_tail = 0;
}

/* weak default so this file links standalone; natives.c overrides */
__attribute__((weak)) void bridge_player_event(int ev) { (void)ev; }
