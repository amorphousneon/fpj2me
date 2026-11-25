/*
 * fpj2me — core VM types (fpj2me.h)
 *
 * Object model (KVM-inspired, chosen for precise mark-sweep GC on a
 * machine with no MMU):
 *   - Every Java reference is a 32-bit *handle* (index into an object
 *     table), never a raw pointer. 0 == null.
 *   - Object table entries point into a bump-allocated heap arena.
 *     Handles make GC precise without scanning the C stack, and allow
 *     later heap compaction.
 *   - One 32-bit slot per int/float/ref; long/double use two slots
 *     (lo, hi) exactly like the CLDC spec.
 */
#ifndef FPJ2ME_H
#define FPJ2ME_H

#include "platform.h"

/* forward decls: runtime externs below reference these */
struct JClass;
struct JMethod;
struct Frame;
struct NativeEntry;

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

#define FPJ_MAX_OBJECTS   32768     /* 32k live objects is plenty for GL */
#define FPJ_HEAP_SIZE   (1024*1024) /* 1 MiB Java heap arena            */
#define FPJ_META_SIZE   (512*1024)  /* class metadata arena             */
#define FPJ_STACK_WORDS  16384      /* interpreter stack, 64 KiB        */

/* ------------------------------------------------------------------ */
/* Slots                                                               */
/* ------------------------------------------------------------------ */
typedef uint32_t JRef;             /* object handle, 0 == null          */

typedef union {
    u32    w;                      /* raw word: int bits or float bits  */
    JRef   ref;                    /* object handle                     */
} Slot;

/* ------------------------------------------------------------------ */
/* Runtime objects                                                     */
/* ------------------------------------------------------------------ */
enum {
    T_NULL = 0, T_OBJECT,           /* instance fields follow header    */
    T_ARR_B, T_ARR_C, T_ARR_S,      /* byte / char / short arrays       */
    T_ARR_I, T_ARR_F,               /* int / float (32-bit elem)        */
    T_ARR_J, T_ARR_D,               /* long / double (64-bit elem)      */
    T_ARR_A                         /* array of refs                    */
};

typedef struct __attribute__((aligned(8))) JObj {
    u8       type;                  /* enum above                       */
    u8       flags;                 /* 1 = marked, 2 = live, 4 = pinned */
    u16      handle;                /* index into fpj_objects           */
    u32      size;                  /* payload BYTES (arrays: 4+len*esz)*/
    void    *clazz;                 /* JClass* for T_OBJECT             */
    /* payload follows at (u32*)(o+1): 8-aligned on ARM and x86-64      */
} JObj;

#define OBJ_PAYLOAD(o)  ((u32 *)((o) + 1))          /* field words     */
#define ARR_LEN(o)      (*(u32 *)OBJ_PAYLOAD(o))     /* array length    */
#define ARR_DATA(o)     ((u8 *)OBJ_PAYLOAD(o) + 4)   /* array elements  */

typedef struct {
    JObj  *obj;                     /* NULL slot == free                */
    u16    gen;                     /* bumped when slot reused          */
} ObjSlot;

extern ObjSlot fpj_objects[FPJ_MAX_OBJECTS];
extern u8      fpj_heap[FPJ_HEAP_SIZE] __attribute__((aligned(8)));
extern Slot    fpj_stack[FPJ_STACK_WORDS];   /* per-JVM operand/locals pool */
extern struct Frame *fpj_cur_frame;       /* GC root: active frame chain  */

static inline JObj *obj(JRef r)              /* handle -> object, NULL-safe */
{ return (r && fpj_objects[r].obj) ? fpj_objects[r].obj : (JObj *)0; }

/* heap allocator: bump pointer + coalescing free list after sweep     */
void  heap_init(void);              /* call once before any ref_alloc   */
void *heap_alloc(u32 nbytes);
JRef  ref_alloc(u8 type, u32 payload, void *clazz);
void  gc_run(void);                 /* mark & sweep                     */
void  gc_pin(JRef r);               /* native-held ref across an alloc  */
void  gc_unpin(int n);
void  handle_rebind(JRef h, JRef rep); /* 'new String(bytes..)' ctor: the
                                      caller's handle moves onto the real
                                      T_ARR_C string (see heap.c)       */
void  gc_frame_hold(struct Frame *f); /* keep an outer frame chain    */
void  gc_frame_release(int n);       /* GC-visible while a nested    */
                                      /* interp_call runs (paint/keys  */
                                      /* from blocking natives)       */
void  heap_vm_reset(void);          /* full VM wipe (launcher shutdown) */

/* ---- meta arena (class metadata; bump, never freed) ---------------- */
void *meta_alloc(u32 nbytes);       /* 4-byte aligned                   */
char *meta_strdup(const char *s);
u32   meta_used(void);

/* ---- class registry: GC roots for statics + lookup cache ----------- */
void   class_register(struct JClass *c);
struct JClass *class_find_loaded(const char *name);
int    class_count(void);

/* ---- interned String literals (T_ARR_C objects) -------------------- */
JRef intern_str(const char *utf8_and_len);   /* returns 0 if OOM        */
/* fresh String decoding MODIFIED-UTF-8 bytes (JVMS 4.4.7) to UTF-16 —
 * literals pushed by ldc / ConstantValue MUST decode, not widen: GL
 * text tables carry Latin-1/accented chars and the byte-widening made
 * every 2-byte sequence two chars (AC1: glyph lookup ran off the end
 * of the font table -> ArrayIndexOutOfBoundsException every frame).  */
JRef   str_from_utf8(const char *s, u32 n);  /* returns 0 if OOM        */

/* ---- heap stats + vitals (debug + tests) ---------------------------- */
u32   heap_used_bytes(void);
u32   heap_free_bytes(void);
extern u32 fpj_gc_count;        /* collections run (diag overlay)      */

/* ------------------------------------------------------------------ */
/* Class file model (subset a CLDC class file needs)                   */
/* ------------------------------------------------------------------ */
typedef struct {
    u8 tag;                         /* 1 Utf8 3 Int 4 Float 5 Long 6    */
    u16 idx1, idx2;                 /* Double 7 Class 8 String 9 Field  */
    union {                         /* 10 Method 11 IfaceMethod 12 Name */
        s32 i; u32 u;               /* Type 15..20 ignored (CLDC)       */
        u64 l;
        u16 idx;
    } v;
} CpEntry;                          /* Utf8 bytes live in the meta arena*/

typedef struct JField {
    u16 access;
    const char *name;                /* resolved cp utf8, meta arena     */
    const char *sig;                 /* "I" / "J" / "Ljava/lang/String;" */
    u32 offset;                      /* WORD offset in instance payload  */
    u32 static_val;                  /* static: lo word (or JRef)        */
    u32 static_hi;                   /* static: hi word for J/D          */
} JField;

typedef struct JMethod {
    u16 access;
    const char *name;                /* resolved cp utf8                 */
    const char *sig;                 /* "(II)V" etc                      */
    u8 *code;                        /* NULL => abstract/native          */
    u16 max_stack, max_locals, code_len;
    u16 n_exc;                       /* exception_table entries          */
    u16 *exc;                        /* 4 u16 per entry: start,end,      */
                                     /* handler,catch_idx (native-endian)*/
    CpEntry *cp;                     /* back-pointer to owning class cp   */
    const struct NativeEntry *native; /* resolved native, if any          */
    void **cp_cache;                 /* per-CLASS resolution cache, one
                                      * entry per cp index (shared by all
                                      * methods of the class; 0 on
                                      * synthesized stub methods).
                                      * THE hot-path fix: getfield/
                                      * putfield/getstatic/putstatic used
                                      * to redo class_load + find_field
                                      * (linear registry + strcmp walks)
                                      * on EVERY execution; invokes redid
                                      * resolve_ref + resolve_virtual.
                                      * Now the first execution resolves
                                      * and parks the answer here:
                                      *   field ops -> JField* or the
                                      *     CPC_MISS1/CPC_MISS2 sentinels
                                      *     (library field not modeled)
                                      *   invokes   -> InvokeSite*        */
} JMethod;

typedef struct JClass {
    const char *name;
    struct JClass *super;
    const char *super_name;          /* unresolved name (iterative load) */
    u16 access;
    u16 n_ifaces;                     /* cp indices, resolved lazily    */
    u16 *iface_idx;
    CpEntry  *cp;  u16 cp_count;
    JField   *fields; u16 n_fields;
    JMethod  *methods; u16 n_methods;
    JRef      static_ref;           /* java/lang/Class instance         */
    u8       *field_image;          /* template for instance allocation */
    u32       instance_words;
    u8        state;                /* 0 raw 1 loading 2 ready 3 error  */
    u8        native_only;          /* 1 = stub (no .class file)        */
    u8        clinit_done;          /* 1 = <clinit> ran (or none/err)   */
} JClass;

/* class loading: find class in boot jars (SD) or app jar              */
JClass *class_load(const char *utf8_name);   /* "java/lang/String"      */

/* ---- Java exceptions (catchable, GLLib-style) ---------------------- */
/* 2007+ Gameloft jars probe optional APIs inside try/catch and expect
 * NullPointerException / ClassNotFoundException / RecordStoreNotFound
 * etc. to UNWIND to the nearest handler. Natives and the engine raise
 * through fpj_throw(); exec_run unwinds fpj_pending_exc frame by frame
 * (exception_table matching, catch-all included); uncaught at a C-level
 * call boundary the flag stays set for the C caller to inspect.       */
extern JRef fpj_pending_exc;              /* 0 = none in flight           */
JRef fpj_new_exception(const char *cls, const char *msg);
void fpj_throw(JRef exc);                 /* never fails; panics on OOM    */
JRef fpj_class_mirror(JClass *c);         /* java/lang/Class instance      */
extern char fpj_boot_err[64];             /* detail text for boot errors   */

/* find a method walking the super chain (name+sig exact match)        */
struct JMethod *class_find_method(JClass *c, const char *name,
                                   const char *sig);
/* is `c` a subtype of `sup` (super chain + interfaces)?               */
int class_is_subclass(JClass *c, JClass *sup);

/* ------------------------------------------------------------------ */
/* Native method registry (no JNI — CLDC-style direct dispatch)        */
/* ------------------------------------------------------------------ */
typedef struct NativeEntry {
    const char *cls, *name, *sig;
    /* args[0..n-1] = declared params (incl. `this` for instance);
       write result into *ret (2 slots for J/D).                        */
    void (*fn)(const Slot *args, int nargs, Slot *ret);
} NativeEntry;

const NativeEntry *native_lookup(const char *cls, const char *name,
                                 const char *sig);
/* graceful degradation: a library method with no registered native
 * resolves to a zero-returning no-op entry (logged on synthesis, then
 * cached on the stub class). Keeps Gameloft jars alive instead of
 * panicking at the first com/nokia/mid/ui or java/io call.          */
const NativeEntry *native_missing(const char *cls, const char *name,
                                  const char *sig);

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */
typedef struct Frame {
    JMethod        *method;
    const u8       *pc;
    Slot           *locals;         /* into thread stack chunk          */
    Slot           *stack;          /* base of operand stack            */
    int             sp;
    u8              ivk_len;        /* bytes of the invoke that last     */
                                     /* called into this frame (3/5):    */
                                     /* exception raise address in the    */
                                     /* CALLER = pc - ivk_len             */
    struct Frame   *prev;
} Frame;

int  interp_run(Frame *f);          /* returns when frame returns       */
/* call a resolved method from C (natives, MIDlet bootstrap):
   args are the declared params in order (incl. this for instance
   methods); returns 1 and fills ret (2 slots for J/D) on success.     */
int  interp_call(JMethod *m, Slot *args, int nargs, Slot *ret);
/* resolve "java/lang/String" + run public static main/startApp etc.   */
int  fpj_call_static(const char *cls, const char *name, const char *sig,
                     Slot *ret);
void fpj_panic(const char *why);    /* aborts to fp_dbg + LCD banner    */

/* system properties the games query (spoof these!)                   */
const char *fpj_get_property(const char *key);
/* java.lang.System.out / .err — real (silent) PrintStream stubs so
 * println() on the error path cannot NPE (see natives.c)           */
JRef fpj_std_stream(const char *which);

/* ---- boot telemetry ------------------------------------------------- *
 * fpj2me_boot() and the jar/class loaders report progress through
 * these. interp.c provides weak no-op defaults (host tests); on the
 * phone target_main.c overrides them and draws the text live on the
 * green loading screen, so a hang shows its last completed step.
 * fpj_stage      — one-line milestone (row 9, yellow)
 * fpj_stage_class— class currently being loaded (row 11, white)
 * fpj_telemetry  — 1 only while fpj2me_boot runs; it is cleared at
 *                  startApp ENTRY (the game blocks inside startApp,
 *                  so clearing after it returns was too late: runtime
 *                  class loads painted over the game). interp.c also
 *                  wipes the LCD to black at that moment.            */
void fpj_stage(const char *msg);
void fpj_stage_class(const char *name);
extern int fpj_telemetry;

/* ---- runtime vitals + on-target diagnostics ------------------------- *
 * LIBC_SDIO=3 has no log channel; these put the information on the LCD
 * instead (see natives.c and README "diag mode" / "fatal screen").   */
extern int  fpj_diag;               /* 1 = vitals overlay (file "diag")  */
extern u32  fpj_vcpu;               /* interpreter dispatches executed   */
extern char fpj_last_class[64];     /* class parse_one touched last      */
extern JRef fpj_oome;               /* pinned OutOfMemoryError singleton */
extern char fpj_miss_native[96];    /* last no-op'd library method       */
extern u32  fpj_paint_count;        /* paint() deliveries                */
extern u32  fpj_last_paint_ms;      /* time of the last paint            */
extern u32  fpj_key_events;         /* key events queued (midp_input)    */
extern int  fpj_dead_raw[2];        /* unmapped raw pad codes (target)   */

/* red screen with full exception class + message + vitals, then the
 * fpj_on_fatal hook (default: keypress/10s -> exit(3) -> fpmenu; host
 * rigs override it). Call with fpj_pending_exc still set.
 * NOTE: keep __attribute__((weak)) ONLY at the definition sites
 * (natives.c / interp.c): a weak DECLARATION here would weaken the
 * strong overrides too (linker then picks by order — subtle bug). */
void fpj_fatal(const char *ctx);
void fpj_on_fatal(void);
/* diag overlay strip; call from any pump path (self-throttled)      */
void fpj_diag_overlay(void);
/* C-level I/O liveness hook (weak no-op in interp.c; target
 * fp_platform.c makes SD reads keep delivering keys + paints)      */
void fpj_io_pump(void);

#endif /* FPJ2ME_H */
