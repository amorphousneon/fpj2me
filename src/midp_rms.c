/*
 * fpj2me — midp_rms.c : javax.microedition.rms.RecordStore backend.
 *
 * One flat file per store, straight on the SD card:
 *   /fpj2me/rms/<suite>/<name>.rms
 *
 * Layout (little-endian, PSRAM-native):
 *   offset 0  : 'F' 'J' 'R' 'S'          magic
 *   offset 4  : u16 version = 1
 *   offset 6  : u16 reserved
 *   offset 8  : u32 record_count
 *   offset 12 : u32 next_id   (monotonic, ids never reused)
 *   offset 16 : records[], each:
 *        u32 id          (0 => tombstone: dead record, size ignored)
 *        u32 size
 *        u8  data[size]
 *
 * addRecord = append. setRecord/deleteRecord = append a new record with
 * the same id (id wins, last one is live; older ones become garbage) —
 * an O(1) flash-friendly log. The in-RAM index keeps only the LAST
 * record per id. closeRecordStore rewrites the file compacted when
 * garbage exceeds 50% of the file.
 *
 * Durability note: libc_embed fwrite goes through fpdoom's SD driver;
 * call fp_fclose() on closeRecordStore AND on MIDlet destroyApp so a
 * battery pull mid-game never corrupts more than the last append.
 */
#include "midp.h"
#include "libc_shims.h"

#define RMS_MAX_OPEN 4
#define RMS_MAX_REC  512

typedef struct {
    u32 id; u32 off; u32 size;    /* file offset of the live copy      */
} RecIdx;

typedef struct {
    fp_file *f;
    char     path[96];
    RecIdx   idx[RMS_MAX_REC];
    u32      n_idx, next_id;
    u32      live_bytes, file_bytes;
} Rs;
static Rs rs_tab[RMS_MAX_OPEN];

static void put32(u8 *p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static u16 get16(const u8 *p){ return (u16)(p[0]|(p[1]<<8)); }
static u32 get32(const u8 *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((u32)p[3]<<24); }

int rms_open(const char *suite, const char *name, int create)
{
    int slot = -1;
    for (int i = 0; i < RMS_MAX_OPEN; i++)
        if (!rs_tab[i].f) { slot = i; break; }
    if (slot < 0) return -1;

    Rs *rs = &rs_tab[slot];
    /* MIDP restricts names to [A-Za-z0-9_-] <=32 chars — trust it,
     * but clamp defensively against path traversal                  */
    char clean[40]; int ci = 0;
    for (const char *p = name; *p && ci < 32; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            clean[ci++] = *p;
    clean[ci] = 0;

    strcpy(rs->path, FPJ_SD_ROOT "rms/");
    strcat(rs->path, suite); strcat(rs->path, "/");
    strcat(rs->path, clean); strcat(rs->path, ".rms");

    /* No mkdir in the fpdoom libc: the per-suite rms/<suite>/ folder
     * only exists if the card was prepared with it. Fall back to a
     * FLAT store file (rms_<suite>_<name>.rms) so RecordStores work
     * on a bare games/fpj2me/ layout — suite and name are sanitized,
     * so the flat name cannot collide or escape the folder.        */
    char flat[96];
    strcpy(flat, FPJ_SD_ROOT "rms_");
    strcat(flat, suite); strcat(flat, "_");
    strcat(flat, clean); strcat(flat, ".rms");

    rs->f = fp_fopen(rs->path, "r+b");
    if (!rs->f) {
        rs->f = fp_fopen(flat, "r+b");
        if (rs->f) { strcpy(rs->path, flat); }
        else if (create) {
            rs->f = fp_fopen(rs->path, "wb+");   /* dir exists after all? */
            if (!rs->f) {
                rs->f = fp_fopen(flat, "wb+");
                if (rs->f) strcpy(rs->path, flat);
            }
            if (!rs->f) return -1;
            u8 nh[16] = {'F','J','R','S',1,0,0,0};
            put32(nh + 8, 0); put32(nh + 12, 1);
            fp_fwrite(nh, 1, 16, rs->f);
            fp_fseek(rs->f, 0, 0);
        } else return -1;                        /* no store, no create    */
    }

    /* index the file: last record per id wins                          */
    u8 hdr[16];
    if (fp_fread(hdr, 1, 16, rs->f) != 16) goto bad;
    if (memcmp(hdr, "FJRS", 4) || get16(hdr + 4) != 1) goto bad;
    rs->next_id   = get32(hdr + 12);
    rs->n_idx     = 0;
    rs->file_bytes = 16;

    u8 rh[8];
    for (;;) {
        long pos = fp_ftell(rs->f);
        if (fp_fread(rh, 1, 8, rs->f) != 8) break;
        u32 id = get32(rh), sz = get32(rh + 4);
        /* replace older entry with the same id (log semantics)       */
        u32 at = 0xFFFFFFFF;
        for (u32 i = 0; i < rs->n_idx; i++)
            if (rs->idx[i].id == id) { at = i; break; }
        if (id != 0) {                              /* skip tombstones */
            if (at == 0xFFFFFFFF && rs->n_idx < RMS_MAX_REC)
                at = rs->n_idx++;
            if (at != 0xFFFFFFFF)
                rs->idx[at] = (RecIdx){ id, (u32)pos + 8, sz };
        }
        fp_fseek(rs->f, (long)sz, 1);               /* skip payload    */
        rs->file_bytes += 8 + sz;
    }
    /* seek to end for appends                                         */
    fp_fseek(rs->f, 0, 2);
    return slot;

bad: fp_fclose(rs->f); rs->f = 0; return -1;
}

static RecIdx *find(Rs *rs, int id)
{
    for (u32 i = 0; i < rs->n_idx; i++)
        if (rs->idx[i].id == (u32)id) return &rs->idx[i];
    return 0;
}

int rms_add(int h, const void *data, int len)
{
    if (h < 0 || h >= RMS_MAX_OPEN || !rs_tab[h].f) return -1;
    Rs *rs = &rs_tab[h];
    u32 id = rs->next_id++;
    long pos = fp_ftell(rs->f);              /* append position        */
    u8 rh[8]; put32(rh, id); put32(rh + 4, (u32)len);
    if (fp_fwrite(rh, 1, 8, rs->f) != 8) return -1;
    if (len && fp_fwrite(data, 1, len, rs->f) != len) return -1;
    if (rs->n_idx < RMS_MAX_REC) {
        rs->idx[rs->n_idx].id   = id;
        rs->idx[rs->n_idx].size = (u32)len;
        rs->idx[rs->n_idx].off  = (u32)pos + 8;
        rs->n_idx++;
    }
    fp_fseek(rs->f, 0, 0);
    u8 hdr[16];
    fp_fread(hdr, 1, 16, rs->f);
    put32(hdr + 12, rs->next_id);
    fp_fseek(rs->f, 0, 0);
    fp_fwrite(hdr, 1, 16, rs->f);
    fp_fseek(rs->f, 0, 2);
    return (int)id;
}

int rms_get(int h, int id, void **data, int *len)
{
    if (h < 0 || h >= RMS_MAX_OPEN) return -1;
    Rs *rs = &rs_tab[h];
    RecIdx *r = find(rs, id);
    if (!r || r->off == 0) return -1;            /* not-yet-indexed live copy */
    u8 *buf = fp_alloc(r->size);
    if (!buf) return -1;
    long cur = fp_ftell(rs->f);
    fp_fseek(rs->f, (long)r->off, 0);
    int ok = fp_fread(buf, 1, (int)r->size, rs->f) == (int)r->size;
    fp_fseek(rs->f, cur, 0);
    if (!ok) { fp_free(buf); return -1; }
    *data = buf; *len = (int)r->size;
    return 0;
}

int rms_set(int h, int id, const void *data, int len)
{
    if (h < 0 || h >= RMS_MAX_OPEN) return -1;
    Rs *rs = &rs_tab[h];
    if (!find(rs, id)) return -1;
    long pos = fp_ftell(rs->f);
    u8 rh[8]; put32(rh, (u32)id); put32(rh + 4, (u32)len);
    if (fp_fwrite(rh, 1, 8, rs->f) != 8) return -1;
    if (len && fp_fwrite(data, 1, len, rs->f) != len) return -1;
    RecIdx *r = find(rs, id);
    r->off  = (u32)pos + 8;
    r->size = (u32)len;
    return 0;
}

int rms_delete(int h, int id)
{
    if (h < 0 || h >= RMS_MAX_OPEN) return -1;
    Rs *rs = &rs_tab[h];
    RecIdx *r = find(rs, id);
    if (!r) return -1;
    u8 rh[8]; put32(rh, (u32)id); put32(rh + 4, 0);
    if (fp_fwrite(rh, 1, 8, rs->f) != 8) return -1;
    *r = rs->idx[--rs->n_idx];      /* swap-remove                      */
    return 0;
}

int rms_enum(int h, int *ids, int max)
{
    if (h < 0 || h >= RMS_MAX_OPEN) return -1;
    Rs *rs = &rs_tab[h];
    /* selection sort: ids ascending, RecordStore.enumerateRecords
     * semantics on real handsets                                       */
    int n = (int)rs->n_idx < max ? (int)rs->n_idx : max;
    for (int i = 0; i < n; i++) ids[i] = (int)rs->idx[i].id;
    for (int i = 1; i < n; i++) {
        int v = ids[i], j = i - 1;
        while (j >= 0 && ids[j] > v) { ids[j+1] = ids[j]; j--; }
        ids[j+1] = v;
    }
    return n;
}

void rms_close(int h)
{
    if (h < 0 || h >= RMS_MAX_OPEN || !rs_tab[h].f) return;
    fp_fclose(rs_tab[h].f);
    /* TODO(M3): if garbage > 50% of file_bytes, rewrite compacted      */
    memset(&rs_tab[h], 0, sizeof(Rs));
}

/* fpj2me_shutdown(): a game that forgot closeRecordStore before
 * notifyDestroyed() must not leave files open when the next jar boots */
void rms_close_all(void)
{
    for (int i = 0; i < RMS_MAX_OPEN; i++)
        if (rs_tab[i].f) rms_close(i);
}
