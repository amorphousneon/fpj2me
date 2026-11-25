/*
 * fpj2me — interp.c : classfile loader + interpreter + bootstrap.
 *
 *   1. CP helpers / classfile parser (JVM spec ch.4, CLDC subset)
 *   2. class_load with STUB fallback for native-only classes
 *      (java.lang.*, javax.microedition.* have no .class files until
 *      the M2 boot.jar; a stub class carries just the name, a super
 *      chain and the native registry)
 *   3. Frame engine: iterative — frames carved from fpj_stack, Java
 *      call depth never costs C stack
 *   4. Dispatch: computed goto, full 202-opcode CLDC set
 *   5. Boot: game.jad -> MIDlet class -> <init> -> startApp -> pump
 *
 * No verifier (jars are preverified/trusted). No exceptions yet:
 * athrow / bounds / cast failures call fpj_panic with a precise
 * message. Threads are M2 (cooperative scheduler).
 */
#include "fpj2me.h"
#include "midp.h"
#include "jar.h"
#include "fp_font.h"
#include "libc_shims.h"
#include <stdlib.h>   /* exit(): fatal paths reboot to fpmenu via sys_exit */

/* ---- boot telemetry defaults (weak; target_main.c overrides) ------ */
int fpj_telemetry = 0;
__attribute__((weak)) void fpj_stage(const char *m)      { (void)m; }
__attribute__((weak)) void fpj_stage_class(const char *m) { (void)m; }

/* ---- runtime vitals (diag overlay; see natives.c fpj_diag_overlay) -- */
int  fpj_diag;                     /* 1 = on-LCD vitals strip (diag file)  */
u32  fpj_vcpu;                     /* dispatches executed (VM "heartbeats")*/
char fpj_last_class[64];           /* last class parse_one touched         */
JRef fpj_oome;                     /* pre-allocated OutOfMemoryError       */

/* weak default: target fp_platform.c overrides with a throttled call to
 * midp_paint_pump() so SD reads / inflates keep delivering keys+paints */
__attribute__((weak)) void fpj_io_pump(void) {}

/* ---- catchable Java exceptions ------------------------------------- */
JRef fpj_pending_exc;                 /* in-flight exception ref, GC root */
char fpj_boot_err[64];                /* detail text for boot failures    */

void fpj_throw(JRef exc)
{
    if (!exc)                          /* OOM while building the exception */
        fpj_panic("OOM while throwing");
    fpj_pending_exc = exc;
}

/* boot helper: an exception escaped <init>/startApp — name it for the
 * red error screen (fpj_boot_err) and clear the pending flag          */
static void boot_exc_note(const char *where)
{
    JObj *eo = obj(fpj_pending_exc);
    const char *cn = (eo && eo->clazz) ? ((JClass *)eo->clazz)->name : "?";
    fp_dbg("uncaught exception in ");
    fp_dbg(where);
    fp_dbg(cn);
    int i = 0;
    const char *p = "uncaught ";
    while (*p && i < (int)sizeof fpj_boot_err - 1) fpj_boot_err[i++] = *p++;
    for (p = cn; *p && i < (int)sizeof fpj_boot_err - 1; p++)
        fpj_boot_err[i++] = (*p == '/') ? '.' : *p;
    /* append ": message" when the Throwable carries one (slot 1 String,
     * see fpj_new_exception) — the boot red screen then names e.g.
     * "uncaught java.lang.OutOfMemoryError: Java heap" in full       */
    if (eo && eo->type == T_OBJECT && eo->size >= 8) {
        JObj *mo = obj((JRef)OBJ_PAYLOAD(eo)[1]);
        if (mo && mo->type == T_ARR_C) {
            if (i < (int)sizeof fpj_boot_err - 2) {
                fpj_boot_err[i++] = ':';
                fpj_boot_err[i++] = ' ';
            }
            u32 n = ARR_LEN(mo);
            if (n > sizeof fpj_boot_err - 1 - (u32)i)
                n = sizeof fpj_boot_err - 1 - (u32)i;
            for (u32 k = 0; k < n; k++)
                fpj_boot_err[i++] = (char)(u8)((u16 *)ARR_DATA(mo))[k];
        }
    }
    fpj_boot_err[i] = 0;
    fpj_pending_exc = 0;
}

int exec_run(Frame *bottom, Slot *ret);    /* engine, defined below    */

/* ------------------------------------------------------------------ */
/* operand-stack macros (parametrized on the frame)                     */
/* ------------------------------------------------------------------ */
#define PUSHV(fv, v)     ((fv)->stack[(fv)->sp++].w = (u32)(v))
#define POPV(fv)         ((fv)->stack[--(fv)->sp].w)
#define PUSH64(fv, lo, hi) do { (fv)->stack[(fv)->sp].w = (u32)(lo); \
    (fv)->stack[(fv)->sp+1].w = (u32)(hi); (fv)->sp += 2; } while (0)
#define TAKE64(fv, lo, hi) do { (fv)->sp -= 2; \
    (lo) = (fv)->stack[(fv)->sp].w; \
    (hi) = (fv)->stack[(fv)->sp+1].w; } while (0)

static u32 f2w(float f)  { u32 w; memcpy(&w, &f, 4); return w; }
static float w2f(u32 w)  { float f; memcpy(&f, &w, 4); return f; }
static double w2d(u32 lo, u32 hi)
{ u64 u = (u64)lo | (u64)hi << 32; double d; memcpy(&d, &u, 8); return d; }
static void d2w(double d, u32 *lo, u32 *hi)
{ u64 u; memcpy(&u, &d, 8); *lo = (u32)u; *hi = (u32)(u >> 32); }
static s64 w2l(u32 lo, u32 hi) { return (s64)((u64)lo | (u64)hi << 32); }
static void l2w(s64 v, u32 *lo, u32 *hi)
{ *lo = (u32)(u64)v; *hi = (u32)((u64)v >> 32); }

/* ------------------------------------------------------------------ */
/* CP helpers                                                           */
/* ------------------------------------------------------------------ */
#define CP_UTF8   1
#define CP_INT    3
#define CP_FLOAT  4
#define CP_LONG   5
#define CP_DOUBLE 6
#define CP_CLASS  7
#define CP_STRING 8
#define CP_FIELD  9
#define CP_METHOD 10
#define CP_IFMETH 11
#define CP_NT     12

#define ACC_STATIC  0x0008
#define ACC_NATIVE  0x0100

static const char *cp_utf8(const CpEntry *cp, u16 idx)
{ return (const char *)(uintptr_t)cp[idx].v.l; }

static const char *cp_class_name(const CpEntry *cp, u16 idx)
{ return cp_utf8(cp, cp[idx].idx1); }

static int sig_ret_cells(const char *sig)
{
    /* return type is the descriptor AFTER the ')': "(...)RET".
     * Reading sig[1] (the first ARG) was wrong: "()V" and "(I)V"
     * reported a 1-cell return, so every void native call pushed a
     * garbage word onto the operand stack.                        */
    const char *p = sig;
    while (*p && *p != ')') p++;
    p++;
    char c = *p;
    return (c == 'J' || c == 'D') ? 2 : (c == 'V') ? 0 : 1;
}
static int sig_arg_cells(const char *sig)
{
    int n = 0; const char *p = sig + 1;
    while (*p && *p != ')') {
        if (*p == 'J' || *p == 'D') n += 2; else n += 1;
        while (*p == '[') p++;
        if (*p == 'L') while (*p && *p != ';') p++;
        p++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* native-only (stub) class super chain                                  */
/* ------------------------------------------------------------------ */
static const char *stub_super_of(const char *name)
{
    static const struct { const char *c, *s; } t[] = {
        { "javax/microedition/lcdui/game/GameCanvas",
          "javax/microedition/lcdui/Canvas" },
        { "javax/microedition/lcdui/game/TiledLayer",
          "javax/microedition/lcdui/game/Layer" },
        { "javax/microedition/lcdui/game/Sparkle",
          "javax/microedition/lcdui/game/Layer" },   /* gloft quirk    */
        { "javax/microedition/lcdui/game/Sprite",
          "javax/microedition/lcdui/game/Layer" },
        { "javax/microedition/lcdui/game/LayerManager",
          "java/lang/Object" },
        { "javax/microedition/lcdui/Canvas",
          "javax/microedition/lcdui/Displayable" },
        { "javax/microedition/lcdui/Screen",
          "javax/microedition/lcdui/Displayable" },
        { "javax/microedition/lcdui/Form",
          "javax/microedition/lcdui/Screen" },
        { "javax/microedition/lcdui/List",
          "javax/microedition/lcdui/Screen" },
        { "javax/microedition/lcdui/TextBox",
          "javax/microedition/lcdui/Screen" },
        { "javax/microedition/lcdui/Alert",
          "javax/microedition/lcdui/Screen" },
        { "javax/microedition/lcdui/Text",
          "javax/microedition/lcdui/Screen" },
        /* Nokia UI API classes GL jars subclass: FullCanvas IS a Canvas
         * (fullscreen repaint/key path) — the chain MUST pass through
         * javax Canvas or every inherited repaint()/getWidth() call
         * resolves to a no-op (AC1's frozen screen).                   */
        { "com/nokia/mid/ui/FullCanvas",
          "javax/microedition/lcdui/Canvas" },
        { "com/nokia/mid/ui/TextEditor",
          "javax/microedition/lcdui/Canvas" },
        { "com/nokia/mid/ui/DeviceControl",
          "java/lang/Object" },
        /* java.lang exception hierarchy: GLLib catch blocks match
         * against Exception / RuntimeException / Throwable, so the
         * stub super chain MUST model it for the unwinder            */
        { "java/lang/Exception",                "java/lang/Throwable" },
        { "java/lang/RuntimeException",         "java/lang/Exception" },
        { "java/lang/Error",                    "java/lang/Throwable" },
        { "java/lang/LinkageError",             "java/lang/Error" },
        { "java/lang/NoClassDefFoundError",     "java/lang/LinkageError" },
        { "java/lang/OutOfMemoryError",         "java/lang/Error" },
        { "java/lang/StackOverflowError",       "java/lang/Error" },
        { "java/lang/IllegalArgumentException", "java/lang/RuntimeException" },
        { "java/lang/IllegalStateException",    "java/lang/RuntimeException" },
        { "java/lang/NullPointerException",     "java/lang/RuntimeException" },
        { "java/lang/ArithmeticException",      "java/lang/RuntimeException" },
        { "java/lang/ClassCastException",       "java/lang/RuntimeException" },
        { "java/lang/ArrayStoreException",      "java/lang/RuntimeException" },
        { "java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException" },
        { "java/lang/ArrayIndexOutOfBoundsException",
          "java/lang/IndexOutOfBoundsException" },
        { "java/lang/StringIndexOutOfBoundsException",
          "java/lang/IndexOutOfBoundsException" },
        { "java/lang/NumberFormatException",    "java/lang/IllegalArgumentException" },
        { "java/lang/ClassNotFoundException",   "java/lang/Exception" },
        { "java/lang/InterruptedException",     "java/lang/Exception" },
        { "java/io/IOException",                "java/lang/Exception" },
        { "java/io/EOFException",               "java/io/IOException" },
        { "java/io/UTFDataFormatException",     "java/io/IOException" },
        { "java/io/InterruptedIOException",     "java/io/IOException" },
        { "javax/microedition/rms/RecordStoreException",
          "java/lang/Exception" },
        { "javax/microedition/rms/RecordStoreNotFoundException",
          "javax/microedition/rms/RecordStoreException" },
        { "javax/microedition/rms/RecordStoreFullException",
          "javax/microedition/rms/RecordStoreException" },
        { "javax/microedition/rms/InvalidRecordIDException",
          "javax/microedition/rms/RecordStoreException" },
        { "javax/microedition/media/MediaException",
          "java/lang/Exception" },
        { "javax/microedition/midlet/MIDletStateChangeException",
          "java/lang/Exception" },
    };
    for (u32 i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].c, name)) return t[i].s;
    return "java/lang/Object";
}

/* ------------------------------------------------------------------ */
/* classfile parser                                                     */
/*                                                                      */
/* Total-parser discipline: EVERY read and skip is bounds-checked       */
/* against the buffer end, and every constant-pool index is validated   */
/* before use. A malformed or divergent stream now fails CLEANLY        */
/* (red screen, named class) instead of walking a wild pointer into    */
/* an abort. class_load is ITERATIVE: the old recursive version put    */
/* one C frame + one 8.4 KB tinfl state per hierarchy level on the     */
/* 36 KB app stack (sc6531e_fdl.ld EXTRA_SIZE 0x9000) — real Gameloft  */
/* chains (10+ classes) overflowed it and froze the phone.             */
/* ------------------------------------------------------------------ */
typedef struct { const u8 *p; const u8 *end; int bad; } Rd;
static u8  r8(Rd *r)  { if (r->p + 1 > r->end) { r->bad = 1; return 0; } return *r->p++; }
static u16 r16(Rd *r) { if (r->p + 2 > r->end) { r->bad = 1; return 0; }
                         u16 v = (u16)(r->p[0] << 8 | r->p[1]); r->p += 2; return v; }
static u32 r32(Rd *r) { if (r->p + 4 > r->end) { r->bad = 1; return 0; }
                         u32 v = (u32)((u32)r->p[0] << 24 | (u32)r->p[1] << 16 |
                             (u32)r->p[2] << 8 | r->p[3]); r->p += 4; return v; }
/* advance n bytes; returns the OLD p, or 0 (poisoned) on overrun     */
static const u8 *rd_skip(Rd *r, u32 n)
{
    if (r->bad || n > (u32)(r->end - r->p)) { r->bad = 1; return 0; }
    const u8 *q = r->p; r->p += n; return q;
}

/* validated constant-pool access: NULL instead of a wild pointer     */
static const char *cp_utf8_b(const CpEntry *cp, u16 cpn, u16 idx)
{
    if (!idx || idx >= cpn || cp[idx].tag != CP_UTF8) return 0;
    return (const char *)(uintptr_t)cp[idx].v.l;
}
static const char *cp_class_name_b(const CpEntry *cp, u16 cpn, u16 idx)
{
    if (!idx || idx >= cpn || cp[idx].tag != CP_CLASS) return 0;
    u16 j = cp[idx].idx1;
    if (!j || j >= cpn || cp[j].tag != CP_UTF8) return 0;
    return (const char *)(uintptr_t)cp[j].v.l;
}

static u8 *read_class_bytes(const char *cls, u32 *len)
{
    char path[160];
    if (strlen(cls) + 7 >= sizeof path) return 0;
    strcpy(path, cls); strcat(path, ".class");
    if (game_jar_ok()) {
        const JarEntry *e = jar_find(jar_game(), path);
        if (e) return jar_read(jar_game(), e, len);
    }
    if (boot_jar_ok()) {
        const JarEntry *e = jar_find(jar_boot(), path);
        if (e) return jar_read(jar_boot(), e, len);
    }
    return 0;
}

static int parse_code(JMethod *m, Rd *r)
{
    m->max_stack  = r16(r);
    m->max_locals = r16(r);
    u32 clen = r32(r);                     /* code_length is u4 (spec)  */
    if (r->bad || clen > 0xFFFF) return -1;
    m->code_len  = (u16)clen;
    m->code      = meta_alloc(m->code_len);
    if (!m->code) return -1;
    if (r->p + m->code_len > r->end) { r->bad = 1; return -1; }
    memcpy(m->code, r->p, m->code_len);
    r->p += m->code_len;
    /* exception_table: kept (native-endian u16 x4 per entry) so the
     * interpreter can UNWIND a throw to the nearest catch handler —
     * GLLib wraps every optional-API probe in try/catch.             */
    u16 exn = r16(r);
    if (r->bad) return -1;
    m->n_exc = exn;
    m->exc   = 0;
    if (exn) {
        const u8 *tab = rd_skip(r, 8u * exn);
        if (!tab) return -1;
        m->exc = meta_alloc(8u * exn);
        if (!m->exc) return -1;
        for (u16 i = 0; i < exn; i++) {
            m->exc[i * 4 + 0] = (u16)(tab[i * 8 + 0] << 8 | tab[i * 8 + 1]);
            m->exc[i * 4 + 1] = (u16)(tab[i * 8 + 2] << 8 | tab[i * 8 + 3]);
            m->exc[i * 4 + 2] = (u16)(tab[i * 8 + 4] << 8 | tab[i * 8 + 5]);
            m->exc[i * 4 + 3] = (u16)(tab[i * 8 + 6] << 8 | tab[i * 8 + 7]);
        }
    }
    u16 atn = r16(r);
    if (r->bad) return -1;
    for (u16 i = 0; i < atn; i++) {
        r16(r); u32 n = r32(r);
        if (r->bad || !rd_skip(r, n)) return -1;
    }
    return 0;
}

/* parse ONE classfile (or native-only stub). NO super resolution —
 * super_name is stored for the iterative chain walk in class_load.
 * Registers state 2 (ready) or a state-3 tombstone on failure.     */
static JClass *parse_one(const char *name)
{
    fpj_stage_class(name);
    /* vitals: remember the class even at runtime (telemetry off) —
     * fpj_diag_overlay shows it, so a frozen name = a hang inside      
     * THIS class's load (SD read / inflate), not a logic loop         */
    {   int i; for (i = 0; i < 62 && name[i]; i++) fpj_last_class[i] = name[i];
        fpj_last_class[i] = 0; }
    JClass *c = class_find_loaded(name);
    if (c) return (c->state == 2) ? c : 0;

    u32 len; u8 *bytes = read_class_bytes(name, &len);
    if (!bytes) {                            /* native-only stub        */
        c = meta_alloc(sizeof *c);
        if (!c) return 0;
        c->name = meta_strdup(name);
        c->super_name = strcmp(name, "java/lang/Object")
                      ? meta_strdup(stub_super_of(name)) : 0;
        c->native_only = 1;
        c->state = 2;
        class_register(c);
        return c;
    }

    Rd rr = { bytes, bytes + len, 0 }, *r = &rr;
    if (r32(r) != 0xCAFEBABEu || r->bad) goto bad;
    r16(r); r16(r);

    u16 cpn = r16(r);
    if (r->bad || !cpn) goto bad;
    CpEntry *cp = meta_alloc(cpn * sizeof(CpEntry));
    if (!cp) goto bad;
    for (u16 i = 1; i < cpn; i++) {
        u8 tag = r8(r);
        cp[i].tag = tag; cp[i].idx1 = cp[i].idx2 = 0; cp[i].v.l = 0;
        switch (tag) {
        case CP_UTF8: {
            u16 n = r16(r);
            if (r->bad || n > (u32)(r->end - r->p)) goto bad;
            char *s = meta_alloc(n + 1);
            if (!s) goto bad;
            memcpy(s, r->p, n); s[n] = 0; r->p += n;
            cp[i].v.l = (uintptr_t)s; cp[i].idx1 = n;
            break; }
        case CP_INT: case CP_FLOAT: cp[i].v.u = r32(r); break;
        case CP_LONG: case CP_DOUBLE:
            cp[i].v.l = (u64)r32(r) << 32 | r32(r); i++; break;
        case CP_CLASS: case CP_STRING: cp[i].idx1 = r16(r); break;
        case CP_FIELD: case CP_METHOD: case CP_IFMETH:
            cp[i].idx1 = r16(r); cp[i].idx2 = r16(r); break;
        case CP_NT: cp[i].idx1 = r16(r); cp[i].idx2 = r16(r); break;
        default: goto bad;                   /* Java 7+ constants       */
        }
        if (r->bad) goto bad;
    }

    c = meta_alloc(sizeof *c);
    if (!c) goto bad;
    c->cp = cp; c->cp_count = cpn;
    c->access   = r16(r);
    u16 this_i  = r16(r);
    u16 super_i = r16(r);
    if (r->bad) goto bad;
    const char *tn = cp_class_name_b(cp, cpn, this_i);
    if (!tn) goto bad;
    c->name = meta_strdup(tn);
    if (!c->name) goto bad;

    const char *sn = super_i ? cp_class_name_b(cp, cpn, super_i) : 0;
    if (super_i && !sn) goto bad;
    c->super_name = sn ? meta_strdup(sn) : 0;

    c->n_ifaces = r16(r);
    c->iface_idx = meta_alloc(c->n_ifaces * 2);
    if (c->n_ifaces && !c->iface_idx) goto bad;
    for (u16 i = 0; i < c->n_ifaces; i++) c->iface_idx[i] = r16(r);
    if (r->bad) goto bad;

    u16 fn = r16(r);
    if (r->bad) goto bad;
    c->fields = meta_alloc(fn * sizeof(JField));
    c->n_fields = fn;
    for (u16 i = 0; i < fn; i++) {
        JField *f = &c->fields[i];
        f->access = r16(r);
        f->name = meta_strdup(cp_utf8_b(cp, cpn, r16(r)));
        f->sig  = meta_strdup(cp_utf8_b(cp, cpn, r16(r)));
        if (r->bad || !f->name || !f->sig) goto bad;
        f->static_val = f->static_hi = 0;
        f->offset = 0;                   /* assigned bottom-up in link */
        u16 an = r16(r);
        if (r->bad) goto bad;
        for (u16 a = 0; a < an; a++) {
            const char *anm = cp_utf8_b(cp, cpn, r16(r));
            u32 alen = r32(r);
            if (r->bad || !anm) goto bad;
            if (!strcmp(anm, "ConstantValue") && (f->access & ACC_STATIC)
                && alen == 2) {
                u16 ci = r16(r);
                if (r->bad || !ci || ci >= cpn) goto bad;
                if (cp[ci].tag == CP_STRING)
                    f->static_val = intern_str(cp_utf8_b(cp, cpn, cp[ci].idx1));
                else { f->static_val = (u32)cp[ci].v.l;
                       f->static_hi  = (u32)(cp[ci].v.l >> 32); }
            } else if (!rd_skip(r, alen)) goto bad;
        }
    }

    u16 mn = r16(r);
    if (r->bad) goto bad;
    c->methods = meta_alloc(mn * sizeof(JMethod));
    c->n_methods = mn;
    /* per-class constant-pool resolution cache: getfield/putfield/
     * getstatic/putstatic and the invokes resolve their cp ref ONCE
     * (first execution) and park the JField* / InvokeSite* in here —
     * see do_invoke + the field opcode handlers. meta_alloc zeroes. */
    void **cpc = meta_alloc(cpn * sizeof(void *));
    for (u16 i = 0; i < mn; i++) {
        JMethod *m = &c->methods[i];
        m->access = r16(r);
        m->name = meta_strdup(cp_utf8_b(cp, cpn, r16(r)));
        m->sig  = meta_strdup(cp_utf8_b(cp, cpn, r16(r)));
        m->code = 0; m->native = 0; m->cp = cp;
        m->cp_cache = cpc;      /* per-class resolution cache (below)  */
        m->max_stack = m->max_locals = m->code_len = 0;
        m->n_exc = 0; m->exc = 0;
        if (r->bad || !m->name || !m->sig) goto bad;
        u16 an = r16(r);
        if (r->bad) goto bad;
        for (u16 a = 0; a < an; a++) {
            const char *anm = cp_utf8_b(cp, cpn, r16(r));
            u32 alen = r32(r);
            if (r->bad || !anm) goto bad;
            if (!strcmp(anm, "Code")) {
                Rd cr = { r->p, r->end, 0 };
                if (parse_code(m, &cr)) goto bad;
            }
            if (!rd_skip(r, alen)) goto bad;
        }
        /* validate exception-table catch types NOW: the unwinder
         * resolves them at throw time and must not walk a bad index  */
        for (u16 x = 0; x < m->n_exc; x++) {
            u16 ci = m->exc[x * 4 + 3];
            if (!ci) continue;                    /* 0 = catch-all     */
            if (ci >= cpn || cp[ci].tag != CP_CLASS) goto bad;
            u16 j = cp[ci].idx1;
            if (!j || j >= cpn || cp[j].tag != CP_UTF8) goto bad;
        }
        if (m->access & ACC_NATIVE)
            m->native = native_lookup(c->name, m->name, m->sig);
    }

    fp_free(bytes);
    c->state = 2;
    class_register(c);
    return c;
bad:
    fp_free(bytes);
    if (c) {
        if (!c->name) c->name = meta_strdup(name);
        c->state = 3;
        class_register(c);          /* tombstone: caches the failure  */
    }
    fp_dbg("classfile parse failed: ");
    fp_dbg(name);
    return 0;
}

/* iterative super-chain loader: parse DOWN the hierarchy (no C
 * recursion), then link supers and assign instance-field offsets UP.
 * Stack use is one flat frame regardless of hierarchy depth.       */
#define FPJ_MAX_CHAIN 96

JClass *class_load(const char *name)
{
    JClass *c = class_find_loaded(name);
    if (c) return (c->state == 2) ? c : 0;

    JClass *chain[FPJ_MAX_CHAIN];
    int depth = 0;
    JClass *anchor = 0;               /* class below the chain, or 0   */
    const char *cur = name;

    for (;;) {
        if (depth >= FPJ_MAX_CHAIN) {
            fp_dbg("hierarchy too deep: "); fp_dbg(name);
            for (int i = 0; i < depth; i++) chain[i]->state = 3;
            return 0;
        }
        JClass *k = parse_one(cur);   /* registers state 2 or 3        */
        if (!k) return 0;
        chain[depth++] = k;
        if (!k->super_name) { anchor = 0; break; }   /* Object        */

        /* cycle inside the chain being built?                        */
        int cyc = 0;
        for (int i = 0; i < depth; i++)
            if (!strcmp(chain[i]->name, k->super_name)) { cyc = 1; break; }
        if (cyc) {
            fp_dbg("class hierarchy cycle at "); fp_dbg(k->name);
            for (int i = 0; i < depth; i++) chain[i]->state = 3;
            return 0;
        }

        JClass *s = class_find_loaded(k->super_name);
        if (s) {                        /* already loaded earlier       */
            anchor = (s->state == 2) ? s : 0;
            break;
        }
        cur = k->super_name;            /* parse the super next         */
    }

    for (int i = depth - 1; i >= 0; i--) {
        JClass *k = chain[i];
        k->super = (i == depth - 1) ? anchor : chain[i + 1];
        if (k->super_name && !k->super) {         /* anchor failed      */
            for (int j = 0; j < depth; j++) chain[j]->state = 3;
            return 0;
        }
        u32 base = k->super ? k->super->instance_words : 0;
        for (u16 fi = 0; fi < k->n_fields; fi++) {
            JField *fd = &k->fields[fi];
            if (!(fd->access & ACC_STATIC)) {
                fd->offset = base;
                base += (fd->sig[0] == 'J' || fd->sig[0] == 'D') ? 2 : 1;
            }
        }
        k->instance_words = base;
    }
    return chain[0];
}

/* ------------------------------------------------------------------ */
/* lookup / type tests                                                  */
/* ------------------------------------------------------------------ */
JMethod *class_find_method(JClass *c, const char *name, const char *sig)
{
    for (JClass *k = c; k; k = k->super)
        for (u16 i = 0; i < k->n_methods; i++)
            if (!strcmp(k->methods[i].name, name) &&
                !strcmp(k->methods[i].sig, sig))
                return &k->methods[i];
    return 0;
}

static JField *find_field(JClass *c, const char *name, const char *sig)
{
    for (JClass *k = c; k; k = k->super)
        for (u16 i = 0; i < k->n_fields; i++)
            if (!strcmp(k->fields[i].name, name) &&
                !strcmp(k->fields[i].sig, sig))
                return &k->fields[i];
    return 0;
}

int class_is_subclass(JClass *c, JClass *sup)
{
    for (JClass *k = c; k; k = k->super) {
        if (k == sup) return 1;
        for (u16 i = 0; i < k->n_ifaces; i++) {
            JClass *ifc = class_load(cp_class_name(k->cp, k->iface_idx[i]));
            if (ifc && class_is_subclass(ifc, sup)) return 1;
        }
    }
    return 0;
}

/* resolve walking the chain; stub classes consult the native registry  */

/* append a synthesized method to a STUB class's (initially empty)
 * method table: caches resolved natives AND the no-op fallbacks so
 * hot loops neither re-search the registry nor grow the meta arena
 * on every call. name/sig stay owned by the caller's constant pool
 * (meta arena, stable for the suite's lifetime).                      */
static JMethod *stub_add_method(JClass *k, const char *name,
                                const char *sig, const NativeEntry *n)
{
    JMethod *arr = meta_alloc((u32)(k->n_methods + 1) * sizeof *arr);
    if (!arr) return 0;
    if (k->n_methods)
        memcpy(arr, k->methods, (u32)k->n_methods * sizeof *arr);
    JMethod *m  = &arr[k->n_methods];
    m->access   = 0x0001 | ACC_NATIVE;     /* public native            */
    m->name     = name;
    m->sig      = sig;
    m->code     = 0;
    m->native   = n;
    m->cp       = 0;
    m->cp_cache = 0;          /* synthesized: no cp, no cache lookups */
    m->max_stack = m->max_locals = m->code_len = 0;
    m->n_exc = 0; m->exc = 0;
    k->methods  = arr;
    k->n_methods++;
    return m;
}

/* same growth trick for a synthesized STATIC field on a stub class:
 * games read/write library statics we have not modeled; a zero-valued
 * slot beats "getstatic: no field" -> panic -> frozen green screen.  */
static JField *stub_add_static(JClass *k, const char *name, const char *sig)
{
    JField *arr = meta_alloc((u32)(k->n_fields + 1) * sizeof *arr);
    if (!arr) return 0;
    if (k->n_fields)
        memcpy(arr, k->fields, (u32)k->n_fields * sizeof *arr);
    JField *fd = &arr[k->n_fields];
    fd->access = ACC_STATIC;
    fd->name   = name;
    fd->sig    = sig;
    fd->offset = 0;
    fd->static_val = fd->static_hi = 0;
    k->fields  = arr;
    k->n_fields++;
    return fd;
}

/* first stub (native-only) class in c's chain — every chain ends at
 * the java/lang/Object stub, so this always finds one                */
static JClass *stub_in_chain(JClass *c)
{
    for (JClass *k = c; k; k = k->super)
        if (k->native_only) return k;
    return 0;
}

static JMethod *resolve_virtual(JClass *cls, const char *name,
                                const char *sig)
{
    for (JClass *k = cls; k; k = k->super) {
        for (u16 i = 0; i < k->n_methods; i++)
            if (!strcmp(k->methods[i].name, name) &&
                !strcmp(k->methods[i].sig, sig))
                return &k->methods[i];
        if (k->native_only) {
            const NativeEntry *n = native_lookup(k->name, name, sig);
            if (n) return stub_add_method(k, name, sig, n);
            /* NO no-op fallback YET: keep walking the chain. GL jars
             * subclass unmodeled stubs (FullCanvas, Sparkle, ...) whose
             * PARENTS model the method (Canvas.repaint / Layer.paint):
             * stopping here made every inherited call a silent no-op
             * (AC1: FullCanvas.repaint() dropped -> frozen screen).   */
        }
    }
    /* whole chain exhausted and nothing modeled: degrade to a logged
     * no-op with a zeroed return and keep booting.                    */
    JClass *st = stub_in_chain(cls);
    if (st) {
        const NativeEntry *n = native_missing(st->name, name, sig);
        if (n) {
            JMethod *m = stub_add_method(st, name, sig, n);
            if (m) return m;
        }
    }
    return 0;
}

static JMethod *resolve_ref(CpEntry *cp, u16 idx)
{
    const char *cn = cp_class_name(cp, cp[idx].idx1);
    const char *nm = cp_utf8(cp, cp[cp[idx].idx2].idx1);
    const char *sg = cp_utf8(cp, cp[cp[idx].idx2].idx2);
    JClass *c = class_load(cn);
    if (!c) return 0;
    return resolve_virtual(c, nm, sg);
}

/* ------------------------------------------------------------------ */
/* arrays (multi-dim)                                                   */
/* ------------------------------------------------------------------ */
static JRef multi_new(const char *cname, u32 *dims, int ndims)
{
    if (ndims <= 0 || dims[0] > 0x10000) return 0;
    if (ndims == 1) {
        switch (*cname) {
        case 'B': case 'Z': return ref_alloc(T_ARR_B, 4 + dims[0], 0);
        case 'C':           return ref_alloc(T_ARR_C, 4 + 2 * dims[0], 0);
        case 'S':           return ref_alloc(T_ARR_S, 4 + 2 * dims[0], 0);
        case 'I':           return ref_alloc(T_ARR_I, 4 + 4 * dims[0], 0);
        case 'F':           return ref_alloc(T_ARR_F, 4 + 4 * dims[0], 0);
        case 'J':           return ref_alloc(T_ARR_J, 4 + 8 * dims[0], 0);
        case 'D':           return ref_alloc(T_ARR_D, 4 + 8 * dims[0], 0);
        default:            return ref_alloc(T_ARR_A, 4 + 4 * dims[0], 0);
        }
    }
    JRef arr = ref_alloc(T_ARR_A, 4 + 4 * dims[0], 0);
    JObj *o = obj(arr);
    if (!o) return 0;
    JRef *el = (JRef *)ARR_DATA(o);
    for (u32 i = 0; i < dims[0]; i++)       /* skip one '[' per level   */
        el[i] = multi_new(cname + 1, dims + 1, ndims - 1);
    return arr;
}

/* ------------------------------------------------------------------ */
/* frame engine                                                         */
/* ------------------------------------------------------------------ */
static Slot *pool_top;                      /* next free fpj_stack slot */
static Frame fpool[96];                     /* Java frames (engine-deep) */
static int   fpool_sp;

static void push_frame(Frame *f, JMethod *m, Slot *args, int nargs)
{
    if (!pool_top) pool_top = fpj_stack;   /* lazy init (test entries)  */
    u32 need = (u32)m->max_locals + m->max_stack;
    if (pool_top + need > fpj_stack + FPJ_STACK_WORDS)
        fpj_panic("Java stack overflow");
    f->method = m;
    f->ivk_len = 3;                       /* default; invoke updates    */
    f->locals = pool_top;  pool_top += m->max_locals;
    f->stack  = pool_top;  pool_top += m->max_stack;
    f->sp = 0;
    if (args)
        for (int i = 0; i < nargs; i++) f->locals[i] = args[i];
}

/* kind: 0 virtual, 1 special, 2 static, 3 interface.
 * returns the frame to continue in (callee), or cf when a native was
 * dispatched inline; *ok = 0 on resolution failure.                    */
/* ---- cp resolution cache -------------------------------------------- *
 * THE dispatch-loop hot-path fix. Every invoke used to redo
 * resolve_ref (class_load = a LINEAR registry walk with strcmps) plus
 * resolve_virtual (linear method walk up the whole superclass chain)
 * PER CALL; the field opcodes redid find_field the same way. At the
 * per-pixel density of GLLib's blit loops that was most of the ~1600
 * cycles/bytecode the it2163 diag strip measured (v=128k ops/s on a
 * 208 MHz ARM9). Call sites are monomorphic in practice, so a
 * one-entry memo per cp index collapses the entire resolution to a
 * single load + compare on the hot path. The cache lives in the
 * class's meta-arena block and dies together with the classes on
 * heap_vm_reset — nothing to invalidate within a boot.              */
typedef struct InvokeSite {
    JMethod *m;      /* resolved target: the cp's static target, or the
                      * virtual override resolved for receiver class rc  */
    JClass  *rc;     /* receiver class m was resolved against; 0 = the
                      * static target IS the dispatch target             */
    u8       kind;   /* opcode family this entry was built for           */
} InvokeSite;
/* field-op sentinels: library field not modeled -> default 0 / drop */
#define CPC_MISS1 ((void *)(uintptr_t)1)   /* 1-word (I/F/ref) default  */
#define CPC_MISS2 ((void *)(uintptr_t)2)   /* 2-word (J/D) default      */

static void run_clinit(JClass *c);   /* defined below (JVMS 5.5) */

static Frame *invoke_dispatch(Frame *cf, JMethod *m, int total)
{
    if (m->native) {
        /* nret MUST start zeroed: no-op/missing natives never write it,
         * and an uninitialized word lands on the operand stack as a
         * bogus reference (AC1: getClass() returned the Display handle
         * that happened to sit in that stack slot, dispatch then
         * "refined" against the wrong class and walked garbage).   */
        Slot nret[2] = {{0}, {0}}, nbuf[24];
        if (total > 24) fpj_panic("native arg overflow");
        for (int i = 0; i < total; i++)
            nbuf[i] = cf->stack[cf->sp - total + i];
        cf->sp -= total;
        m->native->fn(nbuf, total, nret);
        int rc = sig_ret_cells(m->sig);
        if (rc >= 1) PUSHV(cf, nret[0].w);
        if (rc == 2) PUSHV(cf, nret[1].w);
        return cf;
    }
    if (!m->code) fpj_panic("invoked abstract method");

    if (fpool_sp >= (int)(sizeof fpool / sizeof fpool[0]))
        fpj_panic("call depth overflow");
    Frame *nf = &fpool[fpool_sp++];
    push_frame(nf, m, 0, 0);
    for (int i = total - 1; i >= 0; i--)
        nf->locals[i] = cf->stack[--cf->sp];
    nf->prev = cf;
    fpj_cur_frame = nf;
    return nf;
}

static Frame *do_invoke(Frame *cf, u16 idx, int kind, int *ok)
{
    *ok = 1;
    CpEntry *cp = cf->method->cp;
    void **cc = cf->method->cp_cache;
    InvokeSite *site = cc ? (InvokeSite *)cc[idx] : 0;

    if (kind == 2 && !site) {   /* first static call via this cp index:
                                   JVMS 5.5 active use -> <clinit>    */
        JClass *iown = class_load(cp_class_name(cp, cp[idx].idx1));
        if (iown) run_clinit(iown);
    }

    JMethod *m = (site && site->kind == (u8)kind) ? site->m
                                                 : resolve_ref(cp, idx);
    if (!m) { *ok = 0; return cf; }

    /* instance calls (virtual/special/interface) ALWAYS pass `this`,
     * even for zero-arg methods — the old `sig[1] != ')'` guard broke
     * <init>()V and repaint()V (no receiver popped, garbage locals). */
    int total = sig_arg_cells(m->sig) + (kind != 2 ? 1 : 0); /* + this */

    if (kind == 0 || kind == 3) {           /* dynamic dispatch         */
        JObj *ro = obj(cf->stack[cf->sp - total].ref);
        if (!ro) {
            /* Java raises NullPointerException and unwinds to the
             * nearest catch — Gameloft probes optional APIs exactly
             * like this, so a panic (frozen red screen) breaks far
             * more jars than the exception does. The invoke label
             * checks fpj_pending_exc right after do_invoke returns.  */
            fp_dbg("invokevirtual on null: ");
            fp_dbg(m->name);
            if (cf->method) {
                fp_dbg(" (in ");
                fp_dbg(cf->method->name);
                fp_dbg(")");
            }
            fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
            return cf;
        }
        JClass *rc = (JClass *)ro->clazz;
        if (rc) {                            /* refine by runtime class  */
            if (site && site->kind == (u8)kind && site->rc == rc) {
                m = site->m;                 /* monomorphic hit          */
            } else {
                JMethod *dm = resolve_virtual(rc, m->name, m->sig);
                if (!dm) { *ok = 0; return cf; }
                if (!site && cc &&
                    (site = meta_alloc(sizeof *site)) != 0)
                    cc[idx] = site;
                if (site) { site->kind = (u8)kind; site->rc = rc;
                            site->m = dm; }
                m = dm;
            }
        }
        /* receiver without a clazz pointer (java/lang/String payloads
         * and arrays): the statically resolved target IS the dispatch
         * target — String methods are final in CLDC anyway.          */
    }
    if (!site && cc) {          /* memo the static target (1st exec)   */
        site = meta_alloc(sizeof *site);
        if (site) { site->kind = (u8)kind; site->rc = 0; site->m = m;
                    cc[idx] = site; }
    }

    return invoke_dispatch(cf, m, total);
}

int interp_call(JMethod *m, Slot *args, int nargs, Slot *ret)
{
    if (m->native) {
        Slot nret[2] = {{0}, {0}};
        m->native->fn(args, nargs, nret);
        if (ret) {
            int rc = sig_ret_cells(m->sig);
            if (rc >= 1) ret[0].w = nret[0].w;
            if (rc == 2) ret[1].w = nret[1].w;
        }
        return 1;
    }
    if (!m->code) { fpj_panic("call of abstract method"); return 0; }

    Slot *saved = pool_top;
    Frame *saved_f = fpj_cur_frame;
    int saved_sp = fpool_sp;

    /* Keep the OUTER frame chain GC-visible: blocking natives inside
     * this call (Thread.sleep, serviceRepaints) dispatch paint()/
     * keyPressed() through a nested interp_call, which replaces
     * fpj_cur_frame — without the hold, a collection during the nested
     * call could sweep objects referenced from OUR locals/stack.     */
    gc_frame_hold(saved_f);

    Frame f;                                /* per C-level invocation */
    push_frame(&f, m, args, nargs);
    f.prev = 0;
    fpj_cur_frame = &f;
    int r = exec_run(&f, ret);

    pool_top = saved; fpj_cur_frame = saved_f; fpool_sp = saved_sp;
    gc_frame_release(saved_f ? 1 : 0);
    return r;
}

/* ------------------------------------------------------------------ */
/* static initialization (<clinit>)                                     */
/*                                                                      */
/* JVMS 5.5: a class initializes on FIRST ACTIVE USE — new, getstatic,  */
/* putstatic, invokestatic. The engine used to skip <clinit> entirely,  */
/* so every static kept its default value: AC1 froze exactly there —   */
/* GLLib's f.<clinit> arms the splash's first-frame flag (f.aa = true   */
/* and the double-buffer images); without it the state machine never    */
/* left state 18 and the frame loop repainted the same empty splash     */
/* forever (green screen, last telemetry "entry ok / c").               */
/*                                                                      */
/*  - the done-flag is set BEFORE running (a <clinit> that does a       */
/*    putstatic on its own class must not re-enter itself);            */
/*  - the superclass <clinit> runs first (JLS 12.4.1) — chain-walk     */
/*    recursion only, stubs are skipped;                               */
/*  - execution goes through interp_call: the caller frame (engine     */
/*    label or C entry) stays intact, and blocking natives inside a    */
/*    <clinit> still pump paint/keys like the cooperative Thread.start; */
/*  - a throwing <clinit>: log + clear + stay "done" (the JVM would    */
/*    mark the class erroneous; degrading beats a frozen green screen). */
/* ------------------------------------------------------------------ */
static void run_clinit(JClass *c)
{
    if (!c || c->clinit_done || c->native_only) return;
    c->clinit_done = 1;                     /* before the run: re-entry   */
    run_clinit(c->super);                   /* JLS: superclass first      */
    for (u16 i = 0; i < c->n_methods; i++) {
        JMethod *m = &c->methods[i];
        if (!m->name || strcmp(m->name, "<clinit>") || strcmp(m->sig, "()V"))
            continue;
        if (!m->code) continue;             /* native/abstract: nothing   */
        Slot rret[2];
        interp_call(m, 0, 0, rret);
        if (fpj_pending_exc) {
            JObj *eo = obj(fpj_pending_exc);
            fp_dbg("<clinit> threw in ");
            fp_dbg(c->name);
            fp_dbg(" (");
            fp_dbg((eo && eo->clazz) ? ((JClass *)eo->clazz)->name : "?");
            fp_dbg(")");
            fpj_pending_exc = 0;
        }
        break;                              /* one <clinit> per class     */
    }
}

int fpj_call_static(const char *cls, const char *name, const char *sig,
                    Slot *ret)
{
    JClass *c = class_load(cls);
    if (!c) { fp_dbg("no class "); fp_dbg(cls); return 0; }
    run_clinit(c);                          /* C entry = active use       */
    JMethod *m = resolve_virtual(c, name, sig);
    if (!m) { fp_dbg("no method "); fp_dbg(name); return 0; }
    return interp_call(m, 0, 0, ret);
}

/* ------------------------------------------------------------------ */
/* dispatch loop (computed goto)                                        */
/* ------------------------------------------------------------------ */
/* ---- precise array-bounds diagnostics + CATCHABLE exception ---------
 * Real Java raises ArrayIndexOutOfBoundsException / NullPointerException;
 * GLLib catch blocks rely on that. Diagnostics go to the log (host:
 * fp_printf one line), the exception object unwinds to the game's
 * own handler — no red banner unless the game fails to catch it.      */
static void arr_put_dec(char *b, int *n, int cap, u32 v)
{
    char tmp[10]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t && *n < cap - 1) b[(*n)++] = tmp[--t];
}
static void arr_put_str(char *b, int *n, int cap, const char *s)
{
    while (s && *s && *n < cap - 1) b[(*n)++] = *s++;
}

#define ARR_ELEMSZ(o) (o->type==T_ARR_B?1: o->type==T_ARR_C||o->type==T_ARR_S?2: \
                       o->type==T_ARR_J||o->type==T_ARR_D?8: 4)
#define ARR_CHECK(o, i) do { if (!(o)) { \
        fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0)); \
        exc_raise = (u32)(pc - 1 - f->method->code); goto exc_unwind; } \
    if ((u32)(i) >= ARR_LEN(o)) { \
        char b[96]; int n = 0; \
        arr_put_str(b, &n, sizeof b, "OOB "); \
        if (f->method) { arr_put_str(b, &n, sizeof b, f->method->name); \
                         arr_put_str(b, &n, sizeof b, "@"); \
                         arr_put_dec(b, &n, sizeof b, \
                                     (u32)(pc - 1 - f->method->code)); } \
        arr_put_str(b, &n, sizeof b, " i="); \
        arr_put_dec(b, &n, sizeof b, (u32)(i)); \
        arr_put_str(b, &n, sizeof b, " len="); \
        arr_put_dec(b, &n, sizeof b, ARR_LEN(o)); \
        b[n] = 0; \
        fp_printf("fpj2me: AIOOBE %s\n", b); \
        fpj_throw(fpj_new_exception("java/lang/ArrayIndexOutOfBoundsException", 0)); \
        exc_raise = (u32)(pc - 1 - f->method->code); goto exc_unwind; } } while (0)

int exec_run(Frame *bottom, Slot *ret)
{
    static void *disp[202];
    static int disp_init;
    /* liveness hook: every 2^18 dispatches (~25-50 Hz at interpreter
     * speed on the 208 MHz ARM926), poll input + deliver a pending
     * repaint from INSIDE the engine — a game that spins on
     * getKeyStates() without sleeping still gets frames + keys (see
     * midp_paint_pump in natives.c). Cheap: one increment + mask per
     * instruction, well under the dispatch cost itself. fpj_vcpu is
     * the same counter exported for the diag overlay: if it stops
     * growing, the VM is NOT executing bytecode (stuck in C: SD read,
     * inflate, GC) — that distinction is the whole overlay.        */
#define DISPATCH() do { fpj_vcpu++; if ((++pump_ctr & 0x3FFFF) == 0) midp_paint_pump(); \
                        goto *disp[*pc++]; } while (0)
    u32 pump_ctr = 0x3FFFFu;         /* first pump after ~2^0 instrs */
    if (!disp_init) {
    #define D(op, lbl) disp[op] = &&lbl
        D(0x00,nop); D(0x01,aconst_null); D(0x02,iconst_m1);
        D(0x03,iconst_0); D(0x04,iconst_1); D(0x05,iconst_2);
        D(0x06,iconst_3); D(0x07,iconst_4); D(0x08,iconst_5);
        D(0x09,lconst_0); D(0x0a,lconst_1);
        D(0x0b,fconst_0); D(0x0c,fconst_1); D(0x0d,fconst_2);
        D(0x0e,dconst_0); D(0x0f,dconst_1);
        D(0x10,bipush); D(0x11,sipush);
        D(0x12,ldc); D(0x13,ldc_w); D(0x14,ldc2_w);
        D(0x15,iload); D(0x16,lload); D(0x17,fload); D(0x18,dload); D(0x19,aload);
        D(0x1a,iload_n); D(0x1b,iload_n); D(0x1c,iload_n); D(0x1d,iload_n);
        D(0x1e,lload_n); D(0x1f,lload_n); D(0x20,lload_n); D(0x21,lload_n);
        D(0x22,fload_n); D(0x23,fload_n); D(0x24,fload_n); D(0x25,fload_n);
        D(0x26,dload_n); D(0x27,dload_n); D(0x28,dload_n); D(0x29,dload_n);
        D(0x2a,aload_n); D(0x2b,aload_n); D(0x2c,aload_n); D(0x2d,aload_n);
        D(0x2e,iaload); D(0x2f,laload); D(0x30,faload); D(0x31,daload);
        D(0x32,aaload); D(0x33,baload); D(0x34,caload); D(0x35,saload);
        D(0x36,istore); D(0x37,lstore); D(0x38,fstore); D(0x39,dstore); D(0x3a,astore);
        D(0x3b,istore_n); D(0x3c,istore_n); D(0x3d,istore_n); D(0x3e,istore_n);
        D(0x3f,lstore_n); D(0x40,lstore_n); D(0x41,lstore_n); D(0x42,lstore_n);
        D(0x43,fstore_n); D(0x44,fstore_n); D(0x45,fstore_n); D(0x46,fstore_n);
        D(0x47,dstore_n); D(0x48,dstore_n); D(0x49,dstore_n); D(0x4a,dstore_n);
        D(0x4b,astore_n); D(0x4c,astore_n); D(0x4d,astore_n); D(0x4e,astore_n);
        D(0x4f,iastore); D(0x50,lastore); D(0x51,fastore); D(0x52,dastore);
        D(0x53,aastore); D(0x54,bastore); D(0x55,castore); D(0x56,sastore);
        D(0x57,pop); D(0x58,pop2); D(0x59,dup); D(0x5a,dup_x1); D(0x5b,dup_x2);
        D(0x5c,dup2); D(0x5d,dup2_x1); D(0x5e,dup2_x2); D(0x5f,swap);
        D(0x60,iadd); D(0x61,ladd); D(0x62,fadd); D(0x63,dadd);
        D(0x64,isub); D(0x65,lsub); D(0x66,fsub); D(0x67,dsub);
        D(0x68,imul); D(0x69,lmul); D(0x6a,fmul); D(0x6b,dmul);
        D(0x6c,idiv); D(0x6d,ldiv); D(0x6e,fdiv); D(0x6f,ddiv);
        D(0x70,irem); D(0x71,lrem); D(0x72,frem); D(0x73,drem);
        D(0x74,ineg); D(0x75,lneg); D(0x76,fneg); D(0x77,dneg);
        D(0x78,ishl); D(0x79,lshl); D(0x7a,ishr); D(0x7b,lshr);
        D(0x7c,iushr); D(0x7d,lushr);
        D(0x7e,iand); D(0x7f,land); D(0x80,ior); D(0x81,lor);
        D(0x82,ixor); D(0x83,lxor); D(0x84,iinc);
        D(0x85,i2l); D(0x86,i2f); D(0x87,i2d);
        D(0x88,l2i); D(0x89,l2f); D(0x8a,l2d);
        D(0x8b,f2i); D(0x8c,f2l); D(0x8d,f2d);
        D(0x8e,d2i); D(0x8f,d2l); D(0x90,d2f);
        D(0x91,i2b); D(0x92,i2c); D(0x93,i2s);
        D(0x94,lcmp); D(0x95,fcmpl); D(0x96,fcmpg); D(0x97,dcmpl); D(0x98,dcmpg);
        D(0x99,ifeq); D(0x9a,ifne); D(0x9b,iflt); D(0x9c,ifge);
        D(0x9d,ifgt); D(0x9e,ifle);
        D(0x9f,if_icmpeq); D(0xa0,if_icmpne); D(0xa1,if_icmplt);
        D(0xa2,if_icmpge); D(0xa3,if_icmpgt); D(0xa4,if_icmple);
        D(0xa5,if_acmpeq); D(0xa6,if_acmpne);
        D(0xa7,goto_); D(0xa8,jsr); D(0xa9,ret);
        D(0xaa,tableswitch); D(0xab,lookupswitch);
        D(0xac,ireturn); D(0xad,lreturn); D(0xae,freturn); D(0xaf,dreturn);
        D(0xb0,areturn); D(0xb1,vreturn);
        D(0xb2,getstatic); D(0xb3,putstatic); D(0xb4,getfield); D(0xb5,putfield);
        D(0xb6,invokevirtual); D(0xb7,invokespecial); D(0xb8,invokestatic);
        D(0xb9,invokeinterface); D(0xba,invokedyn);
        D(0xbb,new); D(0xbc,newarray); D(0xbd,anewarray); D(0xbe,arraylength);
        D(0xbf,athrow); D(0xc0,checkcast); D(0xc1,instanceof_);
        D(0xc2,monitorenter); D(0xc3,monitorexit);
        D(0xc4,wide); D(0xc5,multianewarray); D(0xc6,ifnull); D(0xc7,ifnonnull);
        D(0xc8,goto_w); D(0xc9,jsr_w);
        for (int i = 0; i < 202; i++) if (!disp[i]) disp[i] = &&nyi;
        disp_init = 1;
    }

    Frame *f = bottom;
    const u8 *pc = f->method->code;
    u32 a, b, hi, lo, rv0 = 0, rv1 = 0;
    s64 la;
    float fa;
    double da, db;
    int ret_cells = 0;
    u32 exc_raise = 0;                    /* raise offset while unwinding */

    DISPATCH();

    /* ---- constants -------------------------------------------------- */
nop:            DISPATCH();
aconst_null:    PUSHV(f, 0); DISPATCH();
iconst_m1:      PUSHV(f, (u32)-1); DISPATCH();
iconst_0:       PUSHV(f, 0); DISPATCH();
iconst_1:       PUSHV(f, 1); DISPATCH();
iconst_2:       PUSHV(f, 2); DISPATCH();
iconst_3:       PUSHV(f, 3); DISPATCH();
iconst_4:       PUSHV(f, 4); DISPATCH();
iconst_5:       PUSHV(f, 5); DISPATCH();
lconst_0:       PUSH64(f, 0, 0); DISPATCH();
lconst_1:       PUSH64(f, 1, 0); DISPATCH();
fconst_0:       PUSHV(f, f2w(0.0f)); DISPATCH();
fconst_1:       PUSHV(f, f2w(1.0f)); DISPATCH();
fconst_2:       PUSHV(f, f2w(2.0f)); DISPATCH();
dconst_0:       PUSH64(f, 0, 0); DISPATCH();
dconst_1:       { u64 d = 0x3FF0000000000000ULL;
                  PUSH64(f, (u32)d, (u32)(d >> 32)); } DISPATCH();
bipush:         PUSHV(f, (s8)*pc++); DISPATCH();
sipush:         PUSHV(f, (s16)(pc[0] << 8 | pc[1])); pc += 2; DISPATCH();

ldc: ldc_w: { u16 i; int wide = (pc[-1] == 0x13);
              if (wide) { i = (u16)(pc[0] << 8 | pc[1]); pc += 2; }
              else     { i = *pc++; }
              CpEntry *e = &f->method->cp[i];
              if (e->tag == CP_STRING)
                  PUSHV(f, intern_str(cp_utf8(f->method->cp, e->idx1)));
              else if (e->tag == CP_CLASS) {
                  /* real Class mirror: `Foo.class` literals feed
                   * getClass().getResourceAsStream-style loading. A
                   * null mirror here became AC1's "invokevirtual on
                   * null" panic the moment the game called a method
                   * on the pushed reference.                            */
                  JRef mr = 0;
                  const char *cn = cp_class_name(f->method->cp, i);
                  if (cn) {
                      JClass *mc = class_load(cn);
                      if (mc) mr = fpj_class_mirror(mc);
                  }
                  PUSHV(f, mr);
              }
              else PUSHV(f, e->v.u); } DISPATCH();
ldc2_w:         { u16 i = pc[0] << 8 | pc[1]; pc += 2;
                  u64 v = f->method->cp[i].v.l;
                  PUSH64(f, (u32)v, (u32)(v >> 32)); } DISPATCH();

    /* ---- loads / stores --------------------------------------------- */
/* The _n families (iload_0..3 = 0x1a..0x1d, lload/dload 0x1e/0x26,
 * fload 0x22, aload 0x2a; stores at 0x3b/0x3f/0x43/0x47/0x4b) are
 * OPERAND-FREE: the local index is encoded in the opcode itself.
 * Every load family base is 2 (mod 4), every store base 3 (mod 4).
 * The old code did *pc++ & 7 — it consumed a phantom operand byte
 * AND computed a wrong index, desynchronizing the PC (javac emits
 * aload_0 in every non-static method, so no real jar could run).    */
#define LOADIDX()  ((pc[-1] - 0x1a) & 3)
#define STOREIDX() ((pc[-1] - 0x3b) & 3)
#define LOAD_N(n)   PUSHV(f, f->locals[(n)].w)
iload:           LOAD_N(*pc++); DISPATCH();
fload:           LOAD_N(*pc++); DISPATCH();
aload:           LOAD_N(*pc++); DISPATCH();
lload: dload:    { u32 n = *pc++; PUSHV(f, f->locals[n].w);
                   PUSHV(f, f->locals[n+1].w); } DISPATCH();
iload_n: fload_n: aload_n: LOAD_N(LOADIDX()); DISPATCH();
lload_n: dload_n:{ u32 n = LOADIDX(); PUSHV(f, f->locals[n].w);
                   PUSHV(f, f->locals[n+1].w); } DISPATCH();
#undef LOAD_N

#define STORE_N(n) (f->locals[(n)].w = POPV(f))
istore:          STORE_N(*pc++); DISPATCH();
fstore:          STORE_N(*pc++); DISPATCH();
astore:          STORE_N(*pc++); DISPATCH();
lstore: dstore:  { u32 n = *pc++; f->locals[n+1].w = POPV(f);
                   f->locals[n].w = POPV(f); } DISPATCH();
istore_n: fstore_n: astore_n: STORE_N(STOREIDX()); DISPATCH();
lstore_n: dstore_n: { u32 n = STOREIDX(); f->locals[n+1].w = POPV(f);
                   f->locals[n].w = POPV(f); } DISPATCH();
#undef STORE_N
#undef LOAD_N
#undef LOADIDX
#undef STOREIDX

    /* ---- array element access --------------------------------------- */
iaload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          PUSHV(f, ((s32*)ARR_DATA(o))[i]); } DISPATCH();
faload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          PUSHV(f, ((u32*)ARR_DATA(o))[i]); } DISPATCH();
laload: daload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          u64 v = ((u64*)ARR_DATA(o))[i];
          PUSH64(f, (u32)v, (u32)(v>>32)); } DISPATCH();
aaload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          PUSHV(f, ((JRef*)ARR_DATA(o))[i]); } DISPATCH();
baload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          PUSHV(f, (s32)(s8)ARR_DATA(o)[i]); } DISPATCH();
caload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          PUSHV(f, ((u16*)ARR_DATA(o))[i]); } DISPATCH();
saload: { u32 i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          PUSHV(f, (s32)(s16)((u16*)ARR_DATA(o))[i]); } DISPATCH();

iastore: fastore: { u32 v=POPV(f),i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          ((u32*)ARR_DATA(o))[i]=v; } DISPATCH();
lastore: dastore: { hi=POPV(f); lo=POPV(f); u32 i=POPV(f);
          JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          ((u64*)ARR_DATA(o))[i]=(u64)lo|((u64)hi<<32); } DISPATCH();
aastore: { u32 v=POPV(f),i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          ((JRef*)ARR_DATA(o))[i]=v; } DISPATCH();
bastore: { u32 v=POPV(f),i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          ARR_DATA(o)[i]=(u8)v; } DISPATCH();
castore: sastore: { u32 v=POPV(f),i=POPV(f); JObj*o=obj(POPV(f)); ARR_CHECK(o,i);
          ((u16*)ARR_DATA(o))[i]=(u16)v; } DISPATCH();

    /* ---- stack ops --------------------------------------------------- */
pop:             f->sp--; DISPATCH();
pop2:            f->sp -= 2; DISPATCH();
dup:             f->stack[f->sp]=f->stack[f->sp-1]; f->sp++;
                 DISPATCH();
dup_x1:          { Slot v1=f->stack[f->sp-1], v2=f->stack[f->sp-2];
                   f->stack[f->sp-2]=v1; f->stack[f->sp-1]=v2;
                   f->stack[f->sp]=v1; f->sp++; } DISPATCH();
dup_x2:          { Slot v1=f->stack[f->sp-1], v2=f->stack[f->sp-2],
                         v3=f->stack[f->sp-3];
                   f->stack[f->sp-3]=v1; f->stack[f->sp-2]=v3;
                   f->stack[f->sp-1]=v2; f->stack[f->sp]=v1; f->sp++; }
                 DISPATCH();
dup2:            f->stack[f->sp]=f->stack[f->sp-2];
                 f->stack[f->sp+1]=f->stack[f->sp-1]; f->sp+=2;
                 DISPATCH();
dup2_x1:         { Slot v1=f->stack[f->sp-1], v2=f->stack[f->sp-2],
                         v3=f->stack[f->sp-3];
                   f->stack[f->sp-3]=v2; f->stack[f->sp-2]=v1;
                   f->stack[f->sp-1]=v3; f->stack[f->sp]=v2;
                   f->stack[f->sp+1]=v1; f->sp+=2; } DISPATCH();
dup2_x2:         { Slot v1=f->stack[f->sp-1], v2=f->stack[f->sp-2],
                         v3=f->stack[f->sp-3], v4=f->stack[f->sp-4];
                   f->stack[f->sp-4]=v2; f->stack[f->sp-3]=v1;
                   f->stack[f->sp-2]=v4; f->stack[f->sp-1]=v3;
                   f->stack[f->sp]=v2; f->stack[f->sp+1]=v1; f->sp+=2; }
                 DISPATCH();
swap:            { Slot t=f->stack[f->sp-1];
                   f->stack[f->sp-1]=f->stack[f->sp-2];
                   f->stack[f->sp-2]=t; } DISPATCH();

    /* ---- int / long arithmetic --------------------------------------- */
iadd:   a=POPV(f); b=POPV(f); PUSHV(f, b+a); DISPATCH();
isub:   a=POPV(f); b=POPV(f); PUSHV(f, b-a); DISPATCH();
imul:   a=POPV(f); b=POPV(f); PUSHV(f, b*a); DISPATCH();
idiv:   a=POPV(f); b=POPV(f); if(!a) { fpj_throw(fpj_new_exception("java/lang/ArithmeticException", "/ by zero")); exc_raise=(u32)(pc-1-f->method->code); goto exc_unwind; }
        PUSHV(f, (u32)((s32)b/(s32)a)); DISPATCH();
irem:   a=POPV(f); b=POPV(f); if(!a) { fpj_throw(fpj_new_exception("java/lang/ArithmeticException", "/ by zero")); exc_raise=(u32)(pc-1-f->method->code); goto exc_unwind; }
        PUSHV(f, (u32)((s32)b%(s32)a)); DISPATCH();
ineg:   { u32 v=POPV(f); PUSHV(f, (u32)-(s32)v); } DISPATCH();
ishl:   a=POPV(f); b=POPV(f); PUSHV(f, b<<(a&31)); DISPATCH();
ishr:   a=POPV(f); b=POPV(f); PUSHV(f, (u32)((s32)b>>(a&31))); DISPATCH();
iushr:  a=POPV(f); b=POPV(f); PUSHV(f, b>>(a&31)); DISPATCH();
iand:   a=POPV(f); b=POPV(f); PUSHV(f, b&a); DISPATCH();
ior:    a=POPV(f); b=POPV(f); PUSHV(f, b|a); DISPATCH();
ixor:   a=POPV(f); b=POPV(f); PUSHV(f, b^a); DISPATCH();

ladd:   { TAKE64(f,lo,hi); s64 v1=w2l(lo,hi); TAKE64(f,lo,hi);
          l2w(w2l(lo,hi)+v1,&lo,&hi); PUSH64(f,lo,hi); } DISPATCH();
lsub:   { TAKE64(f,lo,hi); s64 v1=w2l(lo,hi); TAKE64(f,lo,hi);
          l2w(w2l(lo,hi)-v1,&lo,&hi); PUSH64(f,lo,hi); } DISPATCH();
lmul:   { TAKE64(f,lo,hi); s64 v1=w2l(lo,hi); TAKE64(f,lo,hi);
          l2w(w2l(lo,hi)*v1,&lo,&hi); PUSH64(f,lo,hi); } DISPATCH();
ldiv:   { TAKE64(f,lo,hi); s64 v1=w2l(lo,hi); TAKE64(f,lo,hi);
          s64 v2=w2l(lo,hi); if(!v1) { fpj_throw(fpj_new_exception("java/lang/ArithmeticException", "/ by zero")); exc_raise=(u32)(pc-1-f->method->code); goto exc_unwind; }
          l2w(v2/v1,&lo,&hi); PUSH64(f,lo,hi); } DISPATCH();
lrem:   { TAKE64(f,lo,hi); s64 v1=w2l(lo,hi); TAKE64(f,lo,hi);
          s64 v2=w2l(lo,hi); if(!v1) { fpj_throw(fpj_new_exception("java/lang/ArithmeticException", "/ by zero")); exc_raise=(u32)(pc-1-f->method->code); goto exc_unwind; }
          l2w(v2%v1,&lo,&hi); PUSH64(f,lo,hi); } DISPATCH();
lneg:   { TAKE64(f,lo,hi); l2w(-w2l(lo,hi),&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
lshl:   { a=POPV(f); TAKE64(f,lo,hi); u64 u=(u64)w2l(lo,hi)<<(a&63);
          PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
lshr:   { a=POPV(f); TAKE64(f,lo,hi); u64 u; s64 v=w2l(lo,hi)>>(a&63);
          memcpy(&u,&v,8); PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
lushr: { a=POPV(f); TAKE64(f,lo,hi); u64 u=((u64)w2l(lo,hi))>>(a&63);
          PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
land:   { TAKE64(f,lo,hi); u64 v1=(u64)w2l(lo,hi); TAKE64(f,lo,hi);
          u64 u=(u64)w2l(lo,hi)&v1;
          PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
lor:    { TAKE64(f,lo,hi); u64 v1=(u64)w2l(lo,hi); TAKE64(f,lo,hi);
          u64 u=(u64)w2l(lo,hi)|v1;
          PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
lxor:   { TAKE64(f,lo,hi); u64 v1=(u64)w2l(lo,hi); TAKE64(f,lo,hi);
          u64 u=(u64)w2l(lo,hi)^v1;
          PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();

    /* ---- float / double arithmetic ----------------------------------- */
fadd:   { u32 aw=POPV(f),bw=POPV(f); fa=w2f(bw)+w2f(aw);
          PUSHV(f, f2w(fa)); } DISPATCH();
fsub:   { u32 aw=POPV(f),bw=POPV(f); fa=w2f(bw)-w2f(aw);
          PUSHV(f, f2w(fa)); } DISPATCH();
fmul:   { u32 aw=POPV(f),bw=POPV(f); fa=w2f(bw)*w2f(aw);
          PUSHV(f, f2w(fa)); } DISPATCH();
fdiv:   { u32 aw=POPV(f),bw=POPV(f); fa=w2f(bw)/w2f(aw);
          PUSHV(f, f2w(fa)); } DISPATCH();
frem:   { u32 aw=POPV(f),bw=POPV(f); fa=w2f(bw); float fb=w2f(aw);
          fa = fa - fb * (float)(s32)(fa / fb);
          PUSHV(f, f2w(fa)); } DISPATCH();
fneg:   { u32 v=POPV(f); PUSHV(f, f2w(-w2f(v))); } DISPATCH();

dadd:   { TAKE64(f,lo,hi); da=w2d(lo,hi); TAKE64(f,lo,hi);
          db=w2d(lo,hi); d2w(da+db,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
dsub:   { TAKE64(f,lo,hi); da=w2d(lo,hi); TAKE64(f,lo,hi);
          db=w2d(lo,hi); d2w(db-da,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
dmul:   { TAKE64(f,lo,hi); da=w2d(lo,hi); TAKE64(f,lo,hi);
          db=w2d(lo,hi); d2w(da*db,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
ddiv:   { TAKE64(f,lo,hi); da=w2d(lo,hi); TAKE64(f,lo,hi);
          db=w2d(lo,hi); d2w(db/da,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
drem:   { TAKE64(f,lo,hi); db=w2d(lo,hi); TAKE64(f,lo,hi);
          da=w2d(lo,hi); double q=da/db;          /* CLDC-style frem    */
          d2w(da - db*(double)(s64)q,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
dneg:   { TAKE64(f,lo,hi); d2w(-w2d(lo,hi),&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();

    /* ---- conversions -------------------------------------------------- */
i2l:    { s32 v=(s32)POPV(f); u64 u=(u64)(s64)v;
          PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
i2f:    { u32 v=POPV(f); PUSHV(f, f2w((float)(s32)v)); } DISPATCH();
i2d:    { s32 v=(s32)POPV(f); d2w((double)v,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
l2i:    f->sp--; DISPATCH();               /* drop hi, keep lo  */
l2f:    { TAKE64(f,lo,hi); PUSHV(f, f2w((float)w2l(lo,hi))); }
        DISPATCH();
l2d:    { TAKE64(f,lo,hi); u64 u; double v=(double)w2l(lo,hi);
          memcpy(&u,&v,8); PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
f2i:    { fa=w2f(POPV(f)); s32 v = (fa!=fa) ? 0 :
            fa >= 2147483647.0f ? 2147483647 :
            fa <= -2147483648.0f ? (s32)0x80000000 : (s32)fa;
          PUSHV(f,(u32)v); } DISPATCH();
f2l:    { fa=w2f(POPV(f)); s64 v = (fa!=fa) ? 0 : (s64)fa; u64 u;
          memcpy(&u,&v,8); PUSH64(f,(u32)u,(u32)(u>>32)); } DISPATCH();
f2d:    { fa=w2f(POPV(f)); d2w((double)fa,&lo,&hi); PUSH64(f,lo,hi); }
        DISPATCH();
d2i:    { TAKE64(f,lo,hi); da=w2d(lo,hi); s32 v = (da!=da) ? 0 :
            da >= 2147483647.0 ? 2147483647 :
            da <= -2147483648.0 ? (s32)0x80000000 : (s32)da;
          PUSHV(f,(u32)v); } DISPATCH();
d2l:    { TAKE64(f,lo,hi); da=w2d(lo,hi); s64 v = (da!=da) ? 0 : (s64)da;
          u64 u; memcpy(&u,&v,8); PUSH64(f,(u32)u,(u32)(u>>32)); }
        DISPATCH();
d2f:    { TAKE64(f,lo,hi); PUSHV(f, f2w((float)w2d(lo,hi))); }
        DISPATCH();
i2b:    { u32 v=POPV(f); PUSHV(f, (s32)(s8)v); } DISPATCH();
i2c:    { u32 v=POPV(f); PUSHV(f, (u16)v); } DISPATCH();
i2s:    { u32 v=POPV(f); PUSHV(f, (s32)(s16)v); } DISPATCH();

lcmp:   { TAKE64(f,lo,hi); s64 v2=w2l(lo,hi); TAKE64(f,lo,hi);
          s64 v1=w2l(lo,hi);      /* v1 pushed FIRST: value1 (JVMS 6.5) */
          PUSHV(f, v1>v2?1:v1<v2?(u32)(-1):0); } DISPATCH();
fcmpl: fcmpg: { u32 aw=POPV(f),bw=POPV(f); fa=w2f(bw); float fb=w2f(aw);
          int nan = (fa!=fa || fb!=fb);
          int g = (pc[-1] == 0x96);                 /* fcmpg: NaN -> 1  */
          PUSHV(f, nan ? (g?1:(u32)(-1)) : fa>fb?1:fa<fb?(u32)(-1):0); }
        DISPATCH();
dcmpl: dcmpg: { TAKE64(f,lo,hi); db=w2d(lo,hi); TAKE64(f,lo,hi); da=w2d(lo,hi);
          int nan = (da!=da || db!=db);
          int g = (pc[-1] == 0x98);
          PUSHV(f, nan ? (g?1:(u32)(-1)) : da>db?1:da<db?(u32)(-1):0); }
        DISPATCH();

    /* ---- branches ------------------------------------------------------ */
    /* JVM spec: branch offsets are measured from the OPCODE address. At
     * each label below, the dispatch already consumed the opcode byte,
     * so pc = opcode+1 when the operands are read; the fixes (-3 after
     * reading a 2-byte offset, -1 for goto-style, etc.) land the target
     * exactly on opcode+offset as javac emits it.                        */
#define BRANCH(cond) do { s16 o=(s16)(pc[0]<<8|pc[1]); pc+=2; \
    if (cond) pc += o - 3; } while (0)
ifeq:           BRANCH((s32)POPV(f)==0); DISPATCH();
ifne:           BRANCH((s32)POPV(f)!=0); DISPATCH();
iflt:           BRANCH((s32)POPV(f)< 0); DISPATCH();
ifge:           BRANCH((s32)POPV(f)>=0); DISPATCH();
ifgt:           BRANCH((s32)POPV(f)> 0); DISPATCH();
ifle:           BRANCH((s32)POPV(f)<=0); DISPATCH();
if_icmpeq:      a=POPV(f); b=POPV(f); BRANCH((s32)b==(s32)a); DISPATCH();
if_icmpne:      a=POPV(f); b=POPV(f); BRANCH((s32)b!=(s32)a); DISPATCH();
if_icmplt:      a=POPV(f); b=POPV(f); BRANCH((s32)b< (s32)a); DISPATCH();
if_icmpge:      a=POPV(f); b=POPV(f); BRANCH((s32)b>=(s32)a); DISPATCH();
if_icmpgt:      a=POPV(f); b=POPV(f); BRANCH((s32)b> (s32)a); DISPATCH();
if_icmple:      a=POPV(f); b=POPV(f); BRANCH((s32)b<=(s32)a); DISPATCH();
if_acmpeq:      a=POPV(f); b=POPV(f); BRANCH(a==b); DISPATCH();
if_acmpne:      a=POPV(f); b=POPV(f); BRANCH(a!=b); DISPATCH();
ifnull:         BRANCH(POPV(f)==0); DISPATCH();
ifnonnull:      BRANCH(POPV(f)!=0); DISPATCH();
goto_:          { s16 o=(s16)(pc[0]<<8|pc[1]); pc+=2+o-3; } DISPATCH();
goto_w:         { s32 o=(s32)((u32)pc[0]<<24|(u32)pc[1]<<16|(u32)pc[2]<<8|pc[3]);
                  pc+=4+o-5; } DISPATCH();
jsr:            { s16 o=(s16)(pc[0]<<8|pc[1]); pc+=2;
                  PUSHV(f, (u32)(pc - f->method->code)); pc += o - 3; }
                DISPATCH();
jsr_w:          { s32 o=(s32)((u32)pc[0]<<24|(u32)pc[1]<<16|(u32)pc[2]<<8|pc[3]);
                  pc+=4; PUSHV(f, (u32)(pc - f->method->code)); pc += o - 5; }
                DISPATCH();
ret:            { u32 n=*pc++; pc = f->method->code + f->locals[n].w; }
                DISPATCH();

tableswitch: { u32 p = (u32)((pc - f->method->code) + 3) & ~3u;
    const u8 *q = f->method->code + p;
    s32 def = q[0]<<24|q[1]<<16|q[2]<<8|q[3];
    s32 lo_ = q[4]<<24|q[5]<<16|q[6]<<8|q[7];
    s32 hi_ = q[8]<<24|q[9]<<16|q[10]<<8|q[11];
    s32 v = (s32)POPV(f);
    if (v < lo_ || v > hi_) pc += def - 1;
    else { q += 12 + 4*(u32)(v-lo_);
           pc += (q[0]<<24|q[1]<<16|q[2]<<8|q[3]) - 1; }
    DISPATCH(); }
lookupswitch: {
    u32 p = (u32)((pc - f->method->code) + 3) & ~3u;
    const u8 *q = f->method->code + p;
    s32 def = q[0]<<24|q[1]<<16|q[2]<<8|q[3];
    s32 np  = q[4]<<24|q[5]<<16|q[6]<<8|q[7];
    s32 v = (s32)POPV(f); q += 8;
    s32 off = def; int l0 = 0, h0 = np - 1;
    while (l0 <= h0) {
        int mid = (l0 + h0) >> 1;
        s32 k = q[(u32)mid*8]<<24|q[(u32)mid*8+1]<<16
              |q[(u32)mid*8+2]<<8|q[(u32)mid*8+3];
        if (k == v) { off = q[(u32)mid*8+4]<<24|q[(u32)mid*8+5]<<16
                    |q[(u32)mid*8+6]<<8|q[(u32)mid*8+7]; break; }
        if (k < v) l0 = mid + 1; else h0 = mid - 1;
    }
    pc += off - 1; DISPATCH(); }

    /* ---- returns ------------------------------------------------------- */
ireturn: areturn: freturn: ret_cells=1; rv0=POPV(f); goto do_return;
lreturn: dreturn: TAKE64(f, rv0, rv1); ret_cells=2; goto do_return;
vreturn: ret_cells=0;

do_return:
    { Frame *caller = f->prev;
      pool_top = f->locals;
      if (f != bottom) fpool_sp--;           /* pop engine frame        */
      if (!caller) {
          if (ret) {
              if (ret_cells >= 1) ret[0].w = rv0;
              if (ret_cells == 2) ret[1].w = rv1;
          }
          fpj_cur_frame = caller;
          return ret_cells ? 1 : 0;
      }
      if (ret_cells >= 1) PUSHV(caller, rv0);
      if (ret_cells == 2) PUSHV(caller, rv1);
      f = caller; fpj_cur_frame = f;
      pc = f->pc; }
    DISPATCH();

    /* ---- exception unwinding ------------------------------------------
     * fpj_pending_exc is set (fpj_throw) either by a native, by the
     * engine's own checks above, or by athrow. Walk the frame chain
     * from the CURRENT frame outward; the raise offset in the inner-
     * most frame was captured at the throw site (exc_raise), in every
     * caller it is the address of the invoke that entered the frame
     * (f->pc - f->ivk_len). Matching handler: exception_table entry
     * whose [start, end) covers the raise and whose catch type is a
     * superclass of the exception (0 = catch-all / finally). JVM
     * semantics on entry: operand stack discarded, exception pushed,
     * pc = handler_pc. Uncaught at the bottom frame: return to the C
     * caller with fpj_pending_exc still SET — interp_call's callers
     * (fpj2me_boot, deliver_repaint, key bridges) inspect and clear.  */
exc_unwind: {
    JRef exc = fpj_pending_exc;
    fpj_pending_exc = 0;
    JObj *eo = obj(exc);
    JClass *ec = (eo && eo->clazz) ? (JClass *)eo->clazz : 0;
    if (ec) { fp_dbg("throw "); fp_dbg(ec->name); }
    for (;;) {
        JMethod *m = f->method;
        int matched = 0;
        for (u16 k2 = 0; k2 < m->n_exc && !matched; k2++) {
            const u16 *e = &m->exc[k2 * 4];
            if (exc_raise < e[0] || exc_raise >= e[1]) continue;
            u16 ci = e[3];
            if (ci) {                            /* typed catch           */
                const char *cn = cp_class_name(m->cp, ci);
                JClass *want = cn ? class_load(cn) : 0;
                if (!want || !ec || !class_is_subclass(ec, want)) continue;
            }
            f->sp = 0;
            PUSHV(f, exc);
            pc = m->code + e[2];
            matched = 1;
        }
        if (matched) break;
        if (f == bottom) {                 /* uncaught at a C boundary    */
            pool_top = f->locals;
            fpj_pending_exc = exc;         /* caller inspects and clears  */
            fpj_cur_frame = 0;
            return 0;
        }
        pool_top = f->locals;
        fpool_sp--;                        /* pop the engine frame        */
        f = f->prev;
        fpj_cur_frame = f;
        exc_raise = (u32)(f->pc - f->ivk_len - f->method->code);
    }
} DISPATCH();

    /* ---- fields --------------------------------------------------------- */
getstatic: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JField *fd;
    void **cc = f->method->cp_cache;
    if (cc && (fd = (JField *)cc[i]) != 0) {  /* cached: no class_load /
        run_clinit / find_field on the hot path                     */
        if (fd->sig[0]=='J' || fd->sig[0]=='D')
            PUSH64(f, fd->static_val, fd->static_hi);
        else PUSHV(f, fd->static_val); }
    else {
    CpEntry *cp = f->method->cp;
    JClass *fc = class_load(cp_class_name(cp, cp[i].idx1));
    if (!fc) { fpj_throw(fpj_new_exception("java/lang/NoClassDefFoundError", 0));
               exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
    run_clinit(fc);                        /* JVMS 5.5: active use       */
    const char *fnm = cp_utf8(cp, cp[cp[i].idx2].idx1);
    const char *fsg = cp_utf8(cp, cp[cp[i].idx2].idx2);
    fd = find_field(fc, fnm, fsg);
    if (!fd) {
        JClass *st = stub_in_chain(fc);
        if (st) fd = stub_add_static(st, fnm, fsg);   /* library static */
        /* java.lang.System.out/.err must be REAL objects: GLLib's font
         * engine prints on its decode-error path and a null there NPEs
         * a glyph-sheet build that the game would otherwise survive. */
        if (fd && !strcmp(fc->name, "java/lang/System") &&
            (!strcmp(fnm, "out") || !strcmp(fnm, "err")))
            fd->static_val = fpj_std_stream(fnm);
    }
    if (!fd) fpj_panic("getstatic: no field");
    if (cc) cc[i] = fd;
    if (fd->sig[0]=='J' || fd->sig[0]=='D')
        PUSH64(f, fd->static_val, fd->static_hi);
    else PUSHV(f, fd->static_val); } } DISPATCH();
putstatic: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JField *fd;
    void **cc = f->method->cp_cache;
    if (cc && (fd = (JField *)cc[i]) != 0) {          /* cached site */
        if (fd->sig[0]=='J' || fd->sig[0]=='D')
            TAKE64(f, fd->static_val, fd->static_hi);
        else fd->static_val = POPV(f); }
    else {
    CpEntry *cp = f->method->cp;
    JClass *fc = class_load(cp_class_name(cp, cp[i].idx1));
    if (!fc) { fpj_throw(fpj_new_exception("java/lang/NoClassDefFoundError", 0));
               exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
    run_clinit(fc);                        /* JVMS 5.5: active use       */
    const char *fnm = cp_utf8(cp, cp[cp[i].idx2].idx1);
    const char *fsg = cp_utf8(cp, cp[cp[i].idx2].idx2);
    fd = find_field(fc, fnm, fsg);
    if (!fd) {
        JClass *st = stub_in_chain(fc);
        if (st) fd = stub_add_static(st, fnm, fsg);
    }
    if (!fd) fpj_panic("putstatic: no field");
    if (cc) cc[i] = fd;
    if (fd->sig[0]=='J' || fd->sig[0]=='D')
        TAKE64(f, fd->static_val, fd->static_hi);
    else fd->static_val = POPV(f); } } DISPATCH();
getfield: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JField *fd;
    void **cc = f->method->cp_cache;
    if (cc && (fd = (JField *)cc[i]) != 0) {          /* cached site */
        if (fd == CPC_MISS1) { (void)POPV(f); PUSHV(f, 0); DISPATCH(); }
        if (fd == CPC_MISS2) { (void)POPV(f); PUSHV(f, 0); PUSHV(f, 0);
                               DISPATCH(); }
        if (fd->sig[0]=='J' || fd->sig[0]=='D') {
            JObj *o = obj(POPV(f));
            if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                      exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
            u32 *pl = OBJ_PAYLOAD(o);
            PUSH64(f, pl[fd->offset], pl[fd->offset+1]);
        } else {
            JObj *o = obj(POPV(f));
            if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                      exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
            PUSHV(f, OBJ_PAYLOAD(o)[fd->offset]);
        }
        DISPATCH();
    }
    /* first execution: resolve by name ONCE, park the answer      */
    {   CpEntry *cp = f->method->cp;
        JClass *fc = class_load(cp_class_name(cp, cp[i].idx1));
        const char *fnm = cp_utf8(cp, cp[cp[i].idx2].idx1);
        const char *fsg = cp_utf8(cp, cp[cp[i].idx2].idx2);
        fd = fc ? find_field(fc, fnm, fsg) : 0;
        if (!fd) {
            /* instance field on a library stub we never modeled: hand back
             * a zero of the declared width instead of dying              */
            if (cc) cc[i] = (fsg && (fsg[0]=='J' || fsg[0]=='D'))
                            ? CPC_MISS2 : CPC_MISS1;
            (void)POPV(f);
            if (fsg && (fsg[0]=='J' || fsg[0]=='D')) { PUSHV(f, 0); PUSHV(f, 0); }
            else PUSHV(f, 0);
            DISPATCH();
        }
        if (cc) cc[i] = fd;
    }
    {   JObj *o = obj(POPV(f));
        if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                  exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
        u32 *pl = OBJ_PAYLOAD(o);
        if (fd->sig[0]=='J' || fd->sig[0]=='D')
            PUSH64(f, pl[fd->offset], pl[fd->offset+1]);
        else PUSHV(f, pl[fd->offset]); } } DISPATCH();
putfield: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JField *fd;
    void **cc = f->method->cp_cache;
    if (cc && (fd = (JField *)cc[i]) != 0) {          /* cached site */
        if (fd == CPC_MISS1) { (void)POPV(f); (void)POPV(f); DISPATCH(); }
        if (fd == CPC_MISS2) { (void)POPV(f); (void)POPV(f); (void)POPV(f);
                               DISPATCH(); }
        if (fd->sig[0]=='J' || fd->sig[0]=='D') {
            hi=POPV(f); lo=POPV(f); JObj *o=obj(POPV(f));
            if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                      exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
            u32 *pl=OBJ_PAYLOAD(o); pl[fd->offset]=lo; pl[fd->offset+1]=hi;
        } else {
            u32 v=POPV(f); JObj *o=obj(POPV(f));
            if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                      exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
            OBJ_PAYLOAD(o)[fd->offset]=v; }
        DISPATCH();
    }
    /* first execution: resolve by name ONCE, park the answer      */
    {   CpEntry *cp = f->method->cp;
        JClass *fc = class_load(cp_class_name(cp, cp[i].idx1));
        const char *fnm = cp_utf8(cp, cp[cp[i].idx2].idx1);
        const char *fsg = cp_utf8(cp, cp[cp[i].idx2].idx2);
        fd = fc ? find_field(fc, fnm, fsg) : 0;
        if (!fd) {
            /* unknown library instance field: consume value(s) + receiver,
             * discard — MIDP games tolerate missing optional state       */
            if (cc) cc[i] = (fsg && (fsg[0]=='J' || fsg[0]=='D'))
                            ? CPC_MISS2 : CPC_MISS1;
            if (fsg && (fsg[0]=='J' || fsg[0]=='D')) { (void)POPV(f); (void)POPV(f); }
            (void)POPV(f);
            DISPATCH();
        }
        if (cc) cc[i] = fd;
    }
    if (fd->sig[0]=='J' || fd->sig[0]=='D') {
        hi=POPV(f); lo=POPV(f); JObj *o=obj(POPV(f));
        if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                  exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
        u32 *pl=OBJ_PAYLOAD(o); pl[fd->offset]=lo; pl[fd->offset+1]=hi;
    } else {
        u32 v=POPV(f); JObj *o=obj(POPV(f));
        if (!o) { fp_dbg("putfield null in ");
                  fp_dbg(f->method->name);
                  fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
                  exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
        OBJ_PAYLOAD(o)[fd->offset]=v; } } DISPATCH();

    /* ---- invokes --------------------------------------------------------- */
invokevirtual: invokespecial: invokestatic: invokeinterface: {
    u8 kindop = pc[-1];
    u16 i = pc[0]<<8|pc[1];
    pc += (kindop == 0xb9) ? 4 : 2;         /* interface: count + 0     */
    f->pc = pc;
    f->ivk_len = (kindop == 0xb9) ? 5 : 3;  /* raise addr in this frame */
    exc_raise = (u32)(pc - 1 - f->method->code);
    int kind = kindop==0xb6?0 : kindop==0xb7?1 : kindop==0xb8?2 : 3;
    /* invokestatic's JVMS-5.5 clinit of the ref's owner class now runs
     * inside do_invoke's UNCACHED path only (first execution per cp
     * index); it used to redo class_load + run_clinit on every call */
    int ok;
    Frame *nf = do_invoke(f, i, kind, &ok);
    if (fpj_pending_exc) goto exc_unwind;   /* null receiver / native threw */
    if (!ok) { fpj_throw(fpj_new_exception("java/lang/NoClassDefFoundError", 0));
               goto exc_unwind; }
    if (nf != f) { f = nf; pc = f->method->code; }
    else pc = f->pc; } DISPATCH();
invokedyn: fpj_panic("invokedynamic (Java 7+) not in CLDC");

    /* ---- object creation -------------------------------------------------- */
new: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JClass *c = class_load(cp_class_name(f->method->cp, i));
    if (!c) { fpj_throw(fpj_new_exception("java/lang/NoClassDefFoundError", 0));
              exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
    run_clinit(c);                         /* JVMS 5.5: new triggers init */
    u32 words = c->instance_words < 4 ? 4 : c->instance_words;
    JRef r = ref_alloc(T_OBJECT, words * 4, c);
    if (!r) {                             /* Java-correct: catchable OOME  */
        fpj_throw(fpj_oome ? fpj_oome
                : fpj_new_exception("java/lang/OutOfMemoryError", "Java heap"));
        exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
    PUSHV(f, r); } DISPATCH();
newarray: { u8 t=*pc++; u32 n=POPV(f);
    if (n > 0x100000) fpj_panic("newarray too large");
    u8 at; u32 esz;
    switch (t) {
    case 4: case 8:  at=T_ARR_B; esz=1; break;   /* boolean/byte */
    case 5:          at=T_ARR_C; esz=2; break;
    case 6:          at=T_ARR_F; esz=4; break;
    case 7:          at=T_ARR_D; esz=8; break;
    case 9:          at=T_ARR_S; esz=2; break;
    case 10:         at=T_ARR_I; esz=4; break;
    case 11:         at=T_ARR_J; esz=8; break;
    default: fpj_panic("bad newarray type");
    }
    JRef r = ref_alloc(at, 4 + n * esz, 0);
    if (!r) {                             /* catchable OOME, not a panic  */
        fpj_throw(fpj_oome ? fpj_oome
                : fpj_new_exception("java/lang/OutOfMemoryError", "array"));
        exc_raise=(u32)(pc-1-f->method->code); goto exc_unwind; }
    ARR_LEN(obj(r)) = n;
    PUSHV(f, r); } DISPATCH();
anewarray: { u16 i=pc[0]<<8|pc[1]; pc+=2; u32 n=POPV(f);
    if (n > 0x100000) fpj_panic("anewarray too large");
    JRef r = ref_alloc(T_ARR_A, 4 + 4 * n, 0);
    if (!r) {                             /* catchable OOME, not a panic  */
        fpj_throw(fpj_oome ? fpj_oome
                : fpj_new_exception("java/lang/OutOfMemoryError", "array"));
        exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
    ARR_LEN(obj(r)) = n;
    PUSHV(f, r); } DISPATCH();
multianewarray: { u16 i=pc[0]<<8|pc[1]; u8 nd=pc[2]; pc+=3;
    u32 dims[8];
    if (nd > 8) fpj_panic("multianewarray dims");
    for (int k2 = nd-1; k2 >= 0; k2--) dims[k2] = POPV(f);
    JRef r = multi_new(cp_class_name(f->method->cp, i), dims, nd);
    if (!r) {                             /* catchable OOME, not a panic  */
        fpj_throw(fpj_oome ? fpj_oome
                : fpj_new_exception("java/lang/OutOfMemoryError", "array"));
        exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind; }
    PUSHV(f, r); } DISPATCH();
arraylength: { JObj *o = obj(POPV(f));
    if (!o) { fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
              exc_raise=(u32)(pc-1-f->method->code); goto exc_unwind; }
    PUSHV(f, ARR_LEN(o)); } DISPATCH();

athrow: { JRef r0 = POPV(f);
    exc_raise = (u32)(pc - 1 - f->method->code);
    if (!r0) fpj_throw(fpj_new_exception("java/lang/NullPointerException", 0));
    else     fpj_throw(r0);
    goto exc_unwind; }

checkcast: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JRef r = f->stack[f->sp-1].ref;
    if (r) {
        JClass *want = class_load(cp_class_name(f->method->cp, i));
        JClass *have = (JClass *)obj(r)->clazz;
        if (!want || !have || !class_is_subclass(have, want)) {
            fp_dbg("ClassCastException in ");
            fp_dbg(f->method->name);
            fpj_throw(fpj_new_exception("java/lang/ClassCastException", 0));
            exc_raise=(u32)(pc-3-f->method->code); goto exc_unwind;
        }
    } } DISPATCH();
instanceof_: { u16 i=pc[0]<<8|pc[1]; pc+=2;
    JRef r = POPV(f); int v = 0;
    if (r) {
        JClass *want = class_load(cp_class_name(f->method->cp, i));
        JClass *have = (JClass *)obj(r)->clazz;
        v = (want && have && class_is_subclass(have, want)) ? 1 : 0;
    }
    PUSHV(f, v); } DISPATCH();

monitorenter: monitorexit: f->sp--; DISPATCH();

wide: { u8 op = *pc++;
    if (op == 0x84) {
        u32 n = pc[0]<<8|pc[1]; s32 d = (s16)(pc[2]<<8|pc[3]); pc += 4;
        f->locals[n].w = (u32)((s32)f->locals[n].w + d);
    } else {
        u32 n = pc[0]<<8|pc[1]; pc += 2;
        switch (op) {
        case 0x15: case 0x17: case 0x19:
            PUSHV(f, f->locals[n].w); break;
        case 0x16: case 0x18:
            PUSHV(f, f->locals[n].w); PUSHV(f, f->locals[n+1].w); break;
        case 0x36: case 0x38: case 0x3a:
            f->locals[n].w = POPV(f); break;
        case 0x37: case 0x39:
            f->locals[n+1].w = POPV(f); f->locals[n].w = POPV(f); break;
        case 0xa9: pc = f->method->code + f->locals[n].w; break;
        default: fpj_panic("bad wide opcode");
        }
    } } DISPATCH();

iinc: { u32 n=*pc++; s32 d=(s8)*pc++;
    f->locals[n].w = (u32)((s32)f->locals[n].w + d); } DISPATCH();

nyi: fpj_panic("opcode not implemented");

    #undef PUSHV
    #undef POPV
    #undef PUSH64
    #undef TAKE64
    #undef ARR_CHECK
    #undef DISPATCH
    return 0; /* unreachable */
}

/* =================================================================== */
/* BOOT                                                                  */
/* =================================================================== */
const char *fpj_get_property(const char *key)
{
    static const struct { const char *k, *v; } props[] = {
        { "microedition.platform",     "Nokia6300" },  /* GL device db    */
        { "microedition.profiles",     "MIDP-2.0" },
        { "microedition.configuration","CLDC-1.1" },
        { "microedition.encoding",     "ISO-8859-1" },
        { "microedition.locale",       "en-US" },
        { "microedition.media.version","1.0" },
        { "microedition.m3g.version",  "1.0" },
        { "microedition.jtwi.version", "1.0" },
        { "microedition.wma.version",  "1.1" },
        { "microedition.chapi.version", "1.0" },
        { "microedition.hostname",    "localhost" },
        { "supports.mixing",           "false" },
        { "supports.audio.capture",   "false" },
        { "supports.video.capture",   "false" },
        { "screen.width",              "128" },
        { "screen.height",             "160" },
    };
    for (u32 i = 0; i < sizeof props / sizeof props[0]; i++)
        if (!strcmp(props[i].k, key)) return props[i].v;
    return 0;   /* unknown key -> null: Java-correct "unsupported", and
                   exactly what GLLib's feature probes test for       */
}

/* find the MIDlet class in a JAD/manifest text buffer: the third
 * comma-separated field of the "MIDlet-1:" line                      */
static const char *jad_midlet_class(const char *buf)
{
    static char cls[96];
    for (const char *p = buf; *p; ) {
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (eol - p > 10) {
            char head[10]; u32 i;
            for (i = 0; i < 9 && p + i < eol; i++)
                head[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
            head[i] = 0;
            if (!strcmp(head, "midlet-1:")) {
                const char *v = p;
                while (v < eol && *v != ':') v++;
                if (v < eol) v++;                    /* past ':'         */
                /* comma field 3 (fields 1/2 may be empty)              */
                int commas = 0; const char *s = v, *fld = v;
                while (s < eol && commas < 2)
                    if (*s++ == ',') { commas++; fld = s; }
                if (commas < 2) fld = v;             /* no commas: value */
                while (fld < eol && (*fld == ' ' || *fld == '\t')) fld++;
                int n = 0;
                while (fld < eol && *fld != ' ' && *fld != '\r' && *fld != '\n'
                       && *fld != ',' && n < 95)
                    cls[n++] = *fld++;
                cls[n] = 0;
                return n ? cls : 0;
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return 0;
}

void fpj_panic(const char *why)
{
    fpj_telemetry = 0;               /* stop stage drawing over the banner */
    fp_dbg("fpj2me panic: ");
    fp_dbg(why);
    /* RED banner so a panic is distinguishable from a hang on a phone
     * with no log channel (the old silent loop-freeze looked exactly
     * like the "stuck loading" symptom: green screen, stale text).  */
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0xF800);
    fp_draw_text(2, 1 * FP_FONT_H, "fpj2me panic", 0xFFFF);
    fp_draw_text(2, 3 * FP_FONT_H, why, 0xFFFF);
    fp_draw_text(2, 17 * FP_FONT_H, "press any key", 0x07FF);
    fp_lcd_flush();
    /* OLD: for(;;) fp_sleep_ms(1000) — the user had to pull the
     * battery. NEW: one keypress (or 10 s) -> rms flush + exit(3) ->
     * sys_exit watchdog reset -> fpmenu. exit() is what target_main
     * uses to return to the menu; host rigs get exit code 3 so the
     * run.sh boot checks FAIL loudly instead of passing silently.   */
    {   fp_key_t k; int pressed, waited = 0;
        for (;;) {
            while (fp_key_event(&k, &pressed))
                if (pressed) { rms_close_all(); exit(3); }
            if (++waited > 500) break;      /* ~10 s on 20 ms ticks      */
            fp_sleep_ms(20);
        }
    }
    rms_close_all();
    exit(3);
}

/* =================================================================== */
/* BOOT / SHUTDOWN  (launcher menu: boot any *.jar, return to menu)     */
/* =================================================================== */
char fpj_manifest[2048];               /* active suite's manifest text   */
char fpj_jad[2048];                    /* sibling .jad text (MIDP 2.0)    */
int  fpj_midlet_done;                  /* notifyDestroyed() sets this    */

/* case-insensitive entry-name compare (letters only) */
static int name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* exact match first, then a case-insensitive sweep (some repackagers
 * ship "meta-inf/manifest.mf")                                        */
static const JarEntry *find_manifest(Jar *j)
{
    const JarEntry *e = jar_find(j, "META-INF/MANIFEST.MF");
    if (e) return e;
    for (u32 i = 0; i < j->n; i++) {
        const char *nm = j->names + j->ent[i].name_off;
        if (name_ieq(nm, "META-INF/MANIFEST.MF")) return &j->ent[i];
    }
    return 0;
}

void fpj2me_shutdown(void)
{
    fpj_telemetry = 0;    /* stop LCD tracing on error paths too      */
    fpj_diag = 0;         /* vitals strip off until the next boot    */
    fpj_oome = 0;         /* handle dies with the heap               */
    fpj_pending_exc = 0;  /* drop in-flight exception (heap dies next) */
    natives_reset();       /* unpin canvas, drop Display/repaint caches */
    input_reset();        /* drop queued keys / held-key state         */
    media_reset();        /* drop player + queued media events         */
    rms_close_all();      /* flush + close any open RecordStores       */
    image_free_all();     /* free tracked native image buffers         */
    jar_close_game();
    heap_vm_reset();      /* heap, handles, classes, meta, interns     */
    fpj_manifest[0] = 0;
    fpj_jad[0] = 0;
    fpj_midlet_done = 0;
}

int fpj2me_boot(const char *jar)
{
    fpj2me_shutdown();                 /* idempotent: safe on 1st boot  */
    fpj_stage("heap");
    heap_init();
    fpj_telemetry = 1;                 /* LCD trace from here on         */

    /* pre-allocated, pinned OutOfMemoryError: when the heap is FULL,
     * building an exception on the spot fails (fpj_new_exception ->
     * fpj_panic "OOM while throwing") — the new/newarray opcodes now
     * throw THIS singleton instead, so games can catch OOME like real
     * Java and uncaught ones reach the red fatal screen with a name   */
    fpj_oome = fpj_new_exception("java/lang/OutOfMemoryError", "Java heap");
    if (fpj_oome) gc_pin(fpj_oome);

    fpj_stage("open jar");
    if (jar_open_boot())  fp_dbg("(no boot.jar — pure-native classes)");
    if (jar_open_game(jar)) { fp_dbg("no jar: "); fp_dbg(jar); return -1; }

    /* MIDlet attributes live in the jar's own manifest (the JAD is
     * optional on-device; menu-launched jars carry everything inside) */
    u32 sz = 0;
    fpj_stage("manifest");
    const JarEntry *e = find_manifest(jar_game());
    u8 *mf = e ? jar_read(jar_game(), e, &sz) : 0;
    if (!mf) { fp_dbg("no META-INF/MANIFEST.MF"); return -2; }
    u32 n = sz < sizeof fpj_manifest - 1 ? sz : sizeof fpj_manifest - 1;
    memcpy(fpj_manifest, mf, n);
    fpj_manifest[n] = 0;
    fp_free(mf);

    /* MIDP 2.0: a sibling <jar_basename>.jad is the PRIMARY property
     * source (it overrides MANIFEST.MF keys in getAppProperty and may
     * carry the MIDlet-1 line for standalone jars). Absent -> the
     * manifest alone still boots, exactly as before.                 */
    fpj_stage("jad");
    if (jad_load_sibling(jar, fpj_jad, sizeof fpj_jad) == 0)
        fp_dbg("sibling .jad loaded");
    else
        fp_dbg("(no sibling .jad — manifest only)");

    const char *mcls = jad_midlet_class(fpj_jad);
    if (!mcls) mcls = jad_midlet_class(fpj_manifest);
    if (!mcls) { fp_dbg("no MIDlet-1 in jad/manifest"); return -2; }
    fp_dbg("MIDlet: "); fp_dbg(mcls);

    fpj_stage("class");
    JClass *mc = class_load(mcls);
    if (!mc || mc->native_only) { fp_dbg("cannot load MIDlet class"); return -3; }
    run_clinit(mc);        /* the boot's "new" — JVMS 5.5 active use */

    fpj_stage("instance");
    JRef inst = ref_alloc(T_OBJECT,
                          (mc->instance_words < 4 ? 4 : mc->instance_words) * 4,
                          mc);
    if (!inst) { fp_dbg("OOM allocating MIDlet"); return -4; }

    Slot a0[1], rret[2];
    a0[0].ref = inst;

    /* vitals reset (diag overlay starts every jar from zero) + the
     * diag-mode switch: a zero-byte file named "diag" next to the
     * jars turns the on-LCD vitals strip on (see README "diag mode") */
    fpj_vcpu = 0;
    fpj_last_class[0] = 0;
    fpj_diag = 0;
    {
        char dpath[128]; int di = 0;
        for (const char *r = FPJ_SD_ROOT; *r && di < 110; r++) dpath[di++] = *r;
        for (const char *p = "diag"; *p; p++) dpath[di++] = *p;
        dpath[di] = 0;
        fp_file *df = fp_fopen(dpath, "rb");
        if (df) { fp_fclose(df); fpj_diag = 1; }
    }

    fpj_stage("ctor");
    JMethod *mi = class_find_method(mc, "<init>", "()V");
    if (mi) interp_call(mi, a0, 1, rret);
    if (fpj_pending_exc) { boot_exc_note("<init>"); return -6; }
    fpj_stage("startApp");
    JMethod *ma = class_find_method(mc, "startApp", "()V");
    if (!ma) { fp_dbg("no startApp"); return -5; }

    /* THE TELEMETRY FIX (target issue 1). Gameloft midlets block
     * INSIDE startApp for their whole life, so the old "fpj_telemetry
     * = 0" AFTER interp_call(startApp) never ran: fpj_stage_class kept
     * repainting row 11 (java/lang/String ...) over the game's own
     * sprites on the green boot banner, and "entry ok" stayed visible
     * forever. Kill the printer BEFORE the call, wipe the green boot
     * banner to solid black (0x0000) and flush — the first paint()
     * starts from a clean black screen and nothing draws over it
     * afterwards. (hostcheck re-verifies: screen must be black at
     * startApp entry, then the game paints on top)                    */
    fpj_telemetry = 0;               /* printer OFF from startApp on      */
    fp_fill_rect(0, 0, FP_LCD_W, FP_LCD_H, 0x0000);
    fp_lcd_flush();                  /* black before the first paint()    */

    interp_call(ma, a0, 1, rret);
    if (fpj_pending_exc) { boot_exc_note("startApp"); return -6; }

    fp_dbg("startApp returned; entering pump");
    return 0;
}
