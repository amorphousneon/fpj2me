/*
 * fpj2me — heap.c : object heap, mark&sweep GC, meta arena, class registry,
 * string interning.
 *
 * Heap layout (fpj_heap, 1 MiB):
 *   [BlkHdr | JObj | payload] [BlkHdr | JObj | payload] ...
 *   BlkHdr { u32 size; u16 free; u16 pad; } — implicit list, first-fit
 *   alloc, sweep coalesces adjacent free blocks. JObj is 8-aligned so
 *   long/double fields and T_ARR_J/T_ARR_D payloads stay ldrd-safe on
 *   the ARM926.
 *
 * Handles: every Java ref is an index into fpj_objects, so GC marks are
 * precise and no C-stack scanning is ever needed. Roots:
 *   1. fpj_cur_frame chain (locals + operand stacks — all Slots)
 *   2. static fields of registered classes (JField.static_val when the
 *      descriptor is a ref type)
 *   3. interned string literals
 *   4. gc_pin() stack — natives holding a ref across an allocation
 */
#include "fpj2me.h"
#include "libc_shims.h"

ObjSlot fpj_objects[FPJ_MAX_OBJECTS];
u8      fpj_heap[FPJ_HEAP_SIZE] __attribute__((aligned(8)));
Slot    fpj_stack[FPJ_STACK_WORDS];
Frame  *fpj_cur_frame;
u32     fpj_gc_count;                 /* vitals: collections run           */

/* ------------------------------------------------------------------ */
/* meta arena                                                          */
/* ------------------------------------------------------------------ */
static u8  meta_pool[FPJ_META_SIZE] __attribute__((aligned(8)));
static u32 meta_off;

void *meta_alloc(u32 n)
{
    n = (n + 7) & ~7u;                    /* keep 8-alignment for code ptrs */
    if (meta_off + n > FPJ_META_SIZE) return 0;
    void *p = meta_pool + meta_off;
    meta_off += n;
    memset(p, 0, n);
    return p;
}

char *meta_strdup(const char *s)
{
    u32 n = (u32)strlen(s) + 1;
    char *p = meta_alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

u32 meta_used(void) { return meta_off; }

/* ------------------------------------------------------------------ */
/* class registry                                                      */
/* ------------------------------------------------------------------ */
#define MAX_CLASSES 1024
static JClass *classes[MAX_CLASSES];
static u32      n_classes;

void class_register(JClass *c)
{
    if (n_classes < MAX_CLASSES) classes[n_classes++] = c;
}

JClass *class_find_loaded(const char *name)
{
    for (u32 i = 0; i < n_classes; i++)
        if (!strcmp(classes[i]->name, name)) return classes[i];
    return 0;
}

int class_count(void) { return (int)n_classes; }

/* heap_vm_reset() lives at the bottom of this file: it touches the
 * statics declared below (handle table, intern table, pin stack).   */
void heap_vm_reset(void);

/* ------------------------------------------------------------------ */
/* string interning                                                    */
/* ------------------------------------------------------------------ */
/* String = T_ARR_C object: payload [u32 len][u16 chars...] (Latin-1
 * widened — CLDC games are 99% ASCII; modified-UTF8 multibyte is M3). */
#define MAX_INTERN 512
static struct { const char *key; JRef ref; } intern_tab[MAX_INTERN];
static u32 n_intern;

JRef intern_str(const char *s)
{
    for (u32 i = 0; i < n_intern; i++)
        if (!strcmp(intern_tab[i].key, s)) return intern_tab[i].ref;

    JRef r = str_from_utf8(s, (u32)strlen(s));
    if (!r) return 0;
    if (n_intern < MAX_INTERN) {
        intern_tab[n_intern].key = s;      /* key lives in the meta arena */
        intern_tab[n_intern].ref = r;
        n_intern++;
    }
    return r;
}

/* ---- modified-UTF-8 -> UTF-16 (JVMS 4.4.7) -------------------------- */
/* 1..3 byte sequences (0x00 as C0 80, surrogates as two 3-byte halves).
 * Malformed bytes degrade to Latin-1 so a bad stream never walks out
 * of bounds — the caller gets SOMETHING drawable, like a real KVM.   */
static u32 utf8_dec(const u8 *p, u32 n, u16 *out)
{
    u32 i = 0, o = 0;
    while (i < n) {
        u8 c = p[i];
        if (c < 0x80) {
            out[o++] = c; i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n &&
                   (p[i + 1] & 0xC0) == 0x80) {
            out[o++] = (u16)(((c & 0x1F) << 6) | (p[i + 1] & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n &&
                   (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
            out[o++] = (u16)(((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) |
                            (p[i + 2] & 0x3F));
            i += 3;
        } else {
            out[o++] = c; i += 1;         /* malformed: Latin-1 fallback */
        }
    }
    return o;
}

JRef str_from_utf8(const char *s, u32 n)
{
    /* decode never grows past n chars: alloc n, fill fewer            */
    JRef r = ref_alloc(T_ARR_C, 4 + 2 * (n ? n : 1), 0);
    JObj *o = obj(r);
    if (!o) return 0;
    ARR_LEN(o) = n ? utf8_dec((const u8 *)s, n, (u16 *)ARR_DATA(o)) : 0;
    return r;
}

/* ------------------------------------------------------------------ */
/* handle table                                                        */
/* ------------------------------------------------------------------ */
static u32 handle_hi = 1;                  /* handle 0 is reserved:
                                              JRef 0 == null by design  */
static u16 handle_freelist[FPJ_MAX_OBJECTS];
static u32 n_handle_free;

static int handle_grab(void)
{
    if (n_handle_free) return handle_freelist[--n_handle_free];
    if (handle_hi < FPJ_MAX_OBJECTS) return (int)handle_hi++;
    return -1;
}

static void handle_drop(u16 h)
{
    fpj_objects[h].obj = 0;
    fpj_objects[h].gen++;
    if (n_handle_free < FPJ_MAX_OBJECTS)
        handle_freelist[n_handle_free++] = h;
}

/* ------------------------------------------------------------------ */
/* block heap                                                          */
/* ------------------------------------------------------------------ */
typedef struct { u32 size; u16 free; u16 pad; } BlkHdr;
#define BLK_HDR   sizeof(BlkHdr)            /* 8 */
#define BLK_MIN   (BLK_HDR + sizeof(JObj) + 4)

/* one free block spanning the arena (fpj_heap is .bss => all zero) */
void heap_init(void)
{
    BlkHdr *b = (BlkHdr *)fpj_heap;
    b->size = FPJ_HEAP_SIZE;
    b->free = 1;
}

void *heap_alloc(u32 payload)
{
    u32 need = BLK_HDR + sizeof(JObj) + ((payload + 3) & ~3u);
    if (need < BLK_MIN) need = BLK_MIN;

    BlkHdr *b = (BlkHdr *)fpj_heap, *best = 0;
    for (;;) {
        if (b->free && b->size >= need) { best = b; break; }  /* first-fit */
        if ((u8 *)b + b->size >= fpj_heap + FPJ_HEAP_SIZE) break;
        b = (BlkHdr *)((u8 *)b + b->size);
    }
    if (!best) {
        gc_run();                                       /* retry after GC */
        b = (BlkHdr *)fpj_heap;
        for (;;) {
            if (b->free && b->size >= need) { best = b; break; }
            if ((u8 *)b + b->size >= fpj_heap + FPJ_HEAP_SIZE) break;
            b = (BlkHdr *)((u8 *)b + b->size);
        }
        if (!best) return 0;                            /* true OOM       */
    }
    /* split if the remainder can hold another object */
    if (best->size >= need + BLK_MIN) {
        BlkHdr *rest = (BlkHdr *)((u8 *)best + need);
        rest->size = best->size - need;
        rest->free = 1; rest->pad = 0;
        best->size = need;
    }
    best->free = 0;
    return (u8 *)best + BLK_HDR;                        /* the JObj slot */
}

u32 heap_used_bytes(void)
{
    u32 used = 0;
    BlkHdr *b = (BlkHdr *)fpj_heap;
    for (;;) {
        if (!b->free) used += b->size;
        if ((u8 *)b + b->size >= fpj_heap + FPJ_HEAP_SIZE) break;
        b = (BlkHdr *)((u8 *)b + b->size);
    }
    return used;
}

u32 heap_free_bytes(void) { return FPJ_HEAP_SIZE - heap_used_bytes(); }

/* constructor rebind: 'new String(bytes,off,len)' hands its <init> a
 * placeholder T_OBJECT allocated by the 'new' opcode, but a real
 * String is a T_ARR_C built inside the constructor. Point the
 * CALLER's handle at the new object instead; the placeholder block
 * becomes ordinary garbage (its ->handle is aimed at the new
 * object's OLD number, so the sweep recycles that number exactly
 * once and never the live one). The receiver must be pinned across
 * the constructor's allocation — the invoke already popped it off
 * the operand stack into a C local the mark phase cannot see.     */
void handle_rebind(JRef h, JRef rep)
{
    if (!h || h == rep) return;
    JObj *o2 = obj(rep), *o1 = obj(h);
    if (!o2 || !o1) return;
    o1->handle = (u16)rep;          /* placeholder sweep recycles rep    */
    o2->handle = (u16)h;
    fpj_objects[h].obj = o2;
    fpj_objects[rep].obj = 0;
}


JRef ref_alloc(u8 type, u32 payload, void *clazz)
{
    JObj *o = heap_alloc(payload);
    if (!o) return 0;
    int h = handle_grab();
    if (h < 0) return 0;                    /* out of handles            */
    o->type   = type;
    o->flags  = 2;                          /* live                      */
    o->handle = (u16)h;
    o->size   = payload;
    o->clazz  = clazz;
    /* JVM zero-init guarantee for fields/arrays */
    memset(OBJ_PAYLOAD(o), 0, (payload + 3) & ~3u);
    fpj_objects[h].obj = o;
    return (JRef)h;
}

/* ------------------------------------------------------------------ */
/* mark & sweep                                                        */
/* ------------------------------------------------------------------ */
static JRef  pin_stack[64];
static u32   pin_sp;

void gc_pin(JRef r)   { if (pin_sp < 64) pin_stack[pin_sp++] = r; }
void gc_unpin(int n)  { if ((u32)n <= pin_sp) pin_sp -= (u32)n; }

/* Iterative marking (no C recursion: GL object graphs can chain deep on
 * a 36 KB app stack). Worklist overflow = conservative keep-everything
 * cycle instead of dropping a live-but-unscanned subtree.            */
#define MARK_STK_MAX 1024
static JRef mark_stk[MARK_STK_MAX];
static u32  mark_sp, mark_overflow;

static void mark_push(JRef r);                  /* fwd decl            */

/* scan a Slot array as candidate handles (ints that happen to look like
 * valid handles are tolerated: mark is monotonic, so a false positive
 * only retains garbage for one cycle, never frees live data)          */
static void mark_slots(const Slot *s, int n)
{
    for (int i = 0; i < n; i++) {
        JRef r = s[i].ref;
        if (r && r < FPJ_MAX_OBJECTS && fpj_objects[r].obj)
            mark_push(r);
    }
}

static void mark_push(JRef r)
{
    JObj *o = obj(r);
    if (!o || (o->flags & 1)) return;       /* not live / already marked */
    o->flags |= 1;
    if (mark_sp < MARK_STK_MAX) mark_stk[mark_sp++] = r;
    else mark_overflow = 1;                 /* keep-all fallback         */
}

/* trace one object's children: ref arrays exactly, instance fields with
 * the same tolerant handle scan mark_slots uses. Stub-class payloads
 * (Image/Stream raw C pointers, rms handles, counts) hold values that
 * are either out of handle range or harmless false positives — the
 * pre-fix code never traced instance fields at all, so every object
 * reachable ONLY through a field (f.bw, StringBuffer buffers, Vector
 * elements...) was swept mid-use once a collection ran.               */
static void mark_children(JRef r)
{
    JObj *o = obj(r);
    if (!o) return;
    if (o->type == T_ARR_A) {
        u32 n = ARR_LEN(o);
        JRef *a = (JRef *)ARR_DATA(o);
        for (u32 i = 0; i < n; i++)
            if (a[i]) mark_push(a[i]);
    } else if (o->type == T_OBJECT) {
        u32 words = o->size >> 2;
        u32 *p = OBJ_PAYLOAD(o);
        for (u32 i = 0; i < words; i++) {
            JRef q = p[i];
            if (q && q < FPJ_MAX_OBJECTS && fpj_objects[q].obj)
                mark_push(q);
        }
    }
}

/* ------------------------------------------------------------------ */
/* re-entrant interpreter calls                                         */
/* ------------------------------------------------------------------ */
/* natives that can block (Thread.sleep, serviceRepaints) dispatch
 * paint()/keyPressed() through interp_call() while an OUTER exec_run
 * is still live. interp_call replaces fpj_cur_frame with the nested
 * frame (prev = 0), so the outer chain would become invisible to the
 * mark phase and its locals/operand stacks could be swept mid-use.
 * The hold stack keeps those chains marked for the nested call's
 * lifetime. Depth is bounded: sleep -> paint -> (no more nesting). */
#define FRAME_HOLD_MAX 8
static Frame *frame_hold[FRAME_HOLD_MAX];
static u32   n_frame_hold;

void gc_frame_hold(Frame *f)
{
    if (f && n_frame_hold < FRAME_HOLD_MAX) frame_hold[n_frame_hold++] = f;
}

void gc_frame_release(int n)
{
    if ((u32)n <= n_frame_hold) n_frame_hold -= (u32)n;
}

void gc_run(void)
{
    fpj_gc_count++;             /* vitals: a racing count = GC pressure  */
    /* ---- mark ---- */
    mark_sp = 0; mark_overflow = 0;
    for (Frame *f = fpj_cur_frame; f; f = f->prev) {
        if (f->locals) mark_slots(f->locals, f->method ? f->method->max_locals : 0);
        if (f->stack)  mark_slots(f->stack, f->sp);
    }
    for (u32 i = 0; i < n_frame_hold; i++)
        for (Frame *f = frame_hold[i]; f; f = f->prev) {
            if (f->locals) mark_slots(f->locals, f->method ? f->method->max_locals : 0);
            if (f->stack)  mark_slots(f->stack, f->sp);
        }
    for (u32 i = 0; i < pin_sp; i++) mark_push(pin_stack[i]);
    if (fpj_pending_exc) mark_push(fpj_pending_exc);   /* in-flight throw */
    for (u32 i = 0; i < n_intern; i++) mark_push(intern_tab[i].ref);
    for (u32 c = 0; c < n_classes; c++) {
        JClass *cl = classes[c];
        if (cl->static_ref) mark_push(cl->static_ref);   /* Class mirrors */
        for (u32 f = 0; f < cl->n_fields; f++) {
            JField *fd = &cl->fields[f];
            if ((fd->access & 8) &&                        /* ACC_STATIC */
                fd->sig && (fd->sig[0] == 'L' || fd->sig[0] == '[')) {
                if (fd->static_val) mark_push(fd->static_val);
            }
        }
    }
    while (mark_sp) mark_children(mark_stk[--mark_sp]);

    /* ---- sweep (implicit walk, coalesce adjacent free) ---- */
    BlkHdr *b = (BlkHdr *)fpj_heap;
    u8 *lim = fpj_heap + FPJ_HEAP_SIZE;
    while ((u8 *)b < lim) {
        BlkHdr *next = (BlkHdr *)((u8 *)b + b->size);
        if (!b->free) {
            JObj *o = (JObj *)((u8 *)b + BLK_HDR);
            if (mark_overflow) {
                o->flags &= ~1;         /* worklist overflow: keep all  */
            } else if ((o->flags & 1)) {
                o->flags &= ~1;             /* survived: clear mark      */
            } else {
                handle_drop(o->handle);
                b->free = 1;
            }
        }
        if (b->free && (u8 *)next < lim && next->free) {
            b->size += next->size;          /* coalesce; re-visit merged */
            continue;
        }
        b = next;
    }
}

/* ------------------------------------------------------------------ */
/* full VM reset (fpj2me_shutdown: menu -> next jar)                    */
/* ------------------------------------------------------------------ */
/* Wipes everything that makes one suite's state: heap blocks, object
 * handles, meta arena (class metadata + interned strings), class
 * registry, GC pin stack. Safe only after natives_reset() dropped the
 * C-side refs (cur_canvas/cur_display/...).                            */
void heap_vm_reset(void)
{
    memset(fpj_objects, 0, sizeof fpj_objects);
    handle_hi = 1;                    /* handle 0 reserved (null)        */
    n_handle_free = 0;
    heap_init();                      /* one free block over the arena   */
    meta_off = 0;                     /* meta_alloc zeroes on handout    */
    n_classes = 0;
    n_intern = 0;
    pin_sp = 0;
    n_frame_hold = 0;
    fpj_cur_frame = 0;
    mark_sp = 0; mark_overflow = 0;
}
