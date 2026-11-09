// Uniform blocks as of the submit call. NVN starts the GPU when nvnQueueSubmitCommands returns and the GPU reads the
// constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it
// runs the part that uses them (the VRAM mirror's sync copies run then, vk_memory.c "VRAM mirror"). Meanwhile the game,
// having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
// another object's matrix (phantom rocks, a flat far field; XC3 1.2.1's producer f_70008c6460, 14-24 ms after the
// submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
// and the uniform bindings of the replay use the copy.
// - Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
//   minUniformBufferOffsetAlignment) and at most 64 KiB from the bound address, overlapping ones merged.
// - A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with
//   that submission's write-back, the mirror holds them until then.
// - A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill,
//   an image-to-buffer copy: vk_usnap_drop) drops the copies it touches from then on, so later draws read the result.
// - The arena goes back to a free list once the submission's fence is done (completion thread).
// The design follows MacNativeGames' XC3_UBO_SNAPSHOT (games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m usnap_take,
// usnap_drop; MIT, Copyright (c) 2026 jhaizhou); the code is ours.
// The copy is taken at the submit call. On the submitting thread that is where vk_exec_record starts. With
// XC3_REPLAY_ASYNC the record starts up to a frame later, after the game has rewritten the blocks (the scene corrupted
// while the camera turned, v0.1.7), so the queueing call takes the bytes into the heap (vk_usnap_take) and the replay
// hands them to the record (vk_usnap_pending); the GPU-write filter still runs at the record, against the replay's
// state. XC3_UBO_SNAPSHOT=0 binds live memory as before; =ab alternates (XC3_AB).
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define ARENA_MIN (4u << 20)
#define CALL_DEPTH 16

typedef struct Arena {
    struct Arena *next;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint64_t size;
} Arena;

typedef struct { uint64_t lo, hi; } Rng;
typedef struct { uint64_t lo, hi, off; int dropped; } Reg;
typedef struct { uint64_t lo, hi, fence; } Wrote;
typedef struct { Rng *col, *tmp; int ncol, ccol, ctmp; } Col;   // collected ranges and the sort's scratch

// vk_usnap_take's copy for one queued submission: regions as in U.reg (off into data), then the bytes.
typedef struct UsnapTake { int nreg; uint64_t total, refused, copy_ns; Reg *reg; uint8_t *data; } UsnapTake;

static struct {
    int mode;                   // rt_knob: 0 off, 1 on, 2 ab
    int on;                     // this submission has a snapshot
    uint64_t fence, gen;
    Arena *arena;
    Col c;                      // collected ranges (the record's thread)
    UsnapTake *pending;         // vk_usnap_pending: the next begin copies from it
    Reg *reg;                   // merged, sorted by lo, disjoint
    int nreg, creg;
    Wrote *wr;                  // GPU writes of submissions not known to be complete
    int nwr, cwr;
    // since the last summary
    uint64_t subs, regions, bytes, copy_ns, hits, drops, inflight, refused, off_subs, changed;
} U;

static Arena *free_arenas;
static PlatMutex free_lock = PLAT_MUTEX_INIT;

static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 1024;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("vk_usnap: out of memory");
    *cap = n;
    return q;
}

static Arena *arena_get(uint64_t need) {
    plat_mutex_lock(&free_lock);
    Arena **pp = &free_arenas, *a = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= need) {
            a = *pp, *pp = a->next;
            break;
        }
    plat_mutex_unlock(&free_lock);
    if (a) return a;
    a = calloc(1, sizeof *a);
    if (!a) rt_fatal("vk_usnap: out of memory");
    a->size = need < ARENA_MIN ? ARENA_MIN : (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = a->size,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    // Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &a->buf, &a->alloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk_usnap: arena of %llu bytes failed (%d): this submission binds live memory",
               (unsigned long long)a->size, (int)res);
        free(a);
        return NULL;
    }
    a->map = info.pMappedData;
    rt_log("vk_usnap: new arena of %.1f MiB", a->size / 1048576.0);
    return a;
}

static void arena_done(void *arg) {   // completion thread: the GPU has read it
    Arena *a = arg;
    plat_mutex_lock(&free_lock);
    a->next = free_arenas, free_arenas = a;
    plat_mutex_unlock(&free_lock);
}

static void collect(Col *k, const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk_usnap: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) collect(k, nvn_op_first(((const p_jump *)a)->target), depth + 1);
        if (h->op == OP_BIND_UBO) {
            const p_bind_buf *b = a;
            if (b->addr && b->size) {
                if (k->ncol == k->ccol) k->col = grow(k->col, &k->ccol, sizeof *k->col);
                uint64_t n = b->size < MAX_RANGE ? b->size : MAX_RANGE;
                k->col[k->ncol++] = (Rng){b->addr & ~(uint64_t)(ALIGN - 1), b->addr + n};
            }
        }
        h = nvn_op_next(h);
    }
}

static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Rng *)a)->lo, y = ((const Rng *)b)->lo;
    return x < y ? -1 : x > y;
}
static int wr_by_lo(const void *a, const void *b) {
    uint64_t x = ((const Wrote *)a)->lo, y = ((const Wrote *)b)->lo;
    return x < y ? -1 : x > y;
}

// Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond
// of the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three
// passes of 11 bits; qsort is left for a span that does not fit in 33 bits (2 TiB).
#define RADIX_BITS 11
static void sort_col(Col *k) {
    int n = k->ncol;
    if (n < 2) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (k->col[i].lo < lo) lo = k->col[i].lo;
        if (k->col[i].lo > hi) hi = k->col[i].lo;
    }
    if ((hi - lo) / ALIGN >> (3 * RADIX_BITS)) {
        qsort(k->col, (size_t)n, sizeof *k->col, by_lo);
        return;
    }
    if (k->ctmp < n) {
        free(k->tmp);
        k->ctmp = k->ccol;
        if (!(k->tmp = malloc((size_t)k->ctmp * sizeof *k->tmp))) rt_fatal("vk_usnap: out of memory");
    }
    Rng *src = k->col, *dst = k->tmp;
    for (int pass = 0; pass < 3; pass++) {
        int shift = pass * RADIX_BITS, count[1 << RADIX_BITS] = {0};
        for (int i = 0; i < n; i++) count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++;
        for (int k = 0, sum = 0; k < 1 << RADIX_BITS; k++) {
            int c = count[k];
            count[k] = sum, sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++] = src[i];
        Rng *t = src;
        src = dst, dst = t;
    }
    if (src != k->col) memcpy(k->col, src, (size_t)n * sizeof *src);   // three passes end in tmp
}

// Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
// one block before every draw: hundreds of equal entries), each with the newest fence among its parts.
static void wr_prune(void) {
    uint64_t done = vk_sync_fence_done();
    int n = 0;
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].fence > done) U.wr[n++] = U.wr[i];
    qsort(U.wr, (size_t)n, sizeof *U.wr, wr_by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && U.wr[i].lo <= U.wr[m - 1].hi) {
            if (U.wr[i].hi > U.wr[m - 1].hi) U.wr[m - 1].hi = U.wr[i].hi;
            if (U.wr[i].fence > U.wr[m - 1].fence) U.wr[m - 1].fence = U.wr[i].fence;
        } else {
            U.wr[m++] = U.wr[i];
        }
    }
    U.nwr = m;
}
static int wr_hits(uint64_t lo, uint64_t hi) {   // wr_prune ran: disjoint, sorted
    int a = 0, b = U.nwr;
    while (a < b) {
        int k = (a + b) / 2;
        if (U.wr[k].hi <= lo) a = k + 1;
        else b = k;
    }
    return a < U.nwr && U.wr[a].lo < hi;
}

static void summary(void) {
    if (U.subs + U.off_subs < 3600) return;
    double s = U.subs ? (double)U.subs : 1.0;
    rt_log("vk_usnap: %llu submissions with a snapshot, %llu without; per submission %.0f ranges, %.1f KiB copied in "
           "%.3f ms, %.0f uniform bindings from the copy, %.1f dropped by GPU writes, %.1f left out (GPU writes in "
           "flight), %.1f refused (not an imported pool), %.1f rewritten by the game between the submit call and the record "
           "(XC3_REPLAY_ASYNC)",
           (unsigned long long)U.subs, (unsigned long long)U.off_subs, U.regions / s, U.bytes / s / 1024.0,
           U.copy_ns / s / 1e6, U.hits / s, U.drops / s, U.inflight / s, U.refused / s, U.changed / s);
    U.subs = U.off_subs = U.regions = U.bytes = U.copy_ns = U.hits = U.drops = U.inflight = U.refused = U.changed = 0;
}

static int snap_mode(void) {
    static int mode;   // stored +1: 0 is "not read yet"
    if (!mode) mode = rt_knob("UBO_SNAPSHOT", "1") + 1;
    return mode;
}

// Collects the uniform ranges the handles bind into k, sorted, and merges them into regions (*reg, *nreg, *creg) that
// lie wholly in one imported pool the CPU writes, laid out back to back in 256-byte steps; returns their total size.
// skip_inflight leaves out regions a GPU write in flight touches (the record's thread only: U.wr).
static uint64_t gather(Col *k, const uint64_t *handles, int n, Reg **reg, int *nreg, int *creg, uint64_t *refused,
                       uint64_t *inflight, int skip_inflight) {
    k->ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(k, nvn_op_first(handles[i]), 0);
    sort_col(k);
    *nreg = 0;
    uint64_t total = 0;
    for (int i = 0; i < k->ncol;) {
        uint64_t lo = k->col[i].lo, hi = k->col[i].hi;
        for (i++; i < k->ncol && k->col[i].lo <= hi; i++)
            if (k->col[i].hi > hi) hi = k->col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_resolve(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            ++*refused;
            continue;
        }
        if (skip_inflight && wr_hits(lo, hi)) {
            ++*inflight;
            continue;
        }
        if (*nreg == *creg) *reg = grow(*reg, creg, sizeof **reg);
        (*reg)[(*nreg)++] = (Reg){lo, hi, total, 0};
        total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    return total;
}

// XC3_REPLAY_ASYNC, at the submit call (nvnQueueSubmitCommands under its submit lock, so one thread at a time): the
// bytes of every uniform range the submission binds, as of now. NULL when snapshots are off or nothing qualifies.
void *vk_usnap_take(const uint64_t *handles, int n) {
    static Col k;
    if (!RT_KNOB_ON(snap_mode() - 1) || n <= 0) return NULL;
    uint64_t t0 = kern_now_ns(), refused = 0, inflight = 0;
    UsnapTake *t = calloc(1, sizeof *t);
    if (!t) rt_fatal("vk_usnap: out of memory");
    int creg = 0;
    t->total = gather(&k, handles, n, &t->reg, &t->nreg, &creg, &refused, &inflight, 0);
    if (!t->nreg) {
        free(t->reg), free(t);
        return NULL;
    }
    if (!(t->data = malloc(t->total))) rt_fatal("vk_usnap: out of memory (%llu bytes)", (unsigned long long)t->total);
    for (int i = 0; i < t->nreg; i++)
        memcpy(t->data + t->reg[i].off, (const void *)(uintptr_t)t->reg[i].lo, t->reg[i].hi - t->reg[i].lo);
    t->refused = refused;
    t->copy_ns = kern_now_ns() - t0;
    return t;
}

void vk_usnap_free(void *take) {
    UsnapTake *t = take;
    if (t && take != (void *)&U) free(t->reg), free(t->data), free(t);
}

// The replay thread, right before the queued submission's record: its vk_usnap_begin copies from take (owned, freed by
// that begin). With take NULL the record binds live memory, as the take found nothing to copy.
void vk_usnap_pending(void *take) {
    vk_usnap_free(U.pending);
    U.pending = take ? take : (void *)&U;   // &U: "taken, empty"
}

void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence) {
    U.mode = snap_mode();
    U.on = 0, U.fence = fence, U.gen++;
    summary();
    UsnapTake *t = U.pending;
    U.pending = NULL;
    if (!RT_KNOB_ON(U.mode - 1)) {
        vk_usnap_free(t);
        U.off_subs++;
        return;
    }
    uint64_t t0 = kern_now_ns();
    wr_prune();
    uint64_t total = 0;
    if (t == (void *)&U) return;   // queued: the take found nothing
    if (t) {
        // Queued: the bytes as of the submit call, less the regions a GPU write of an earlier submission touches.
        U.refused += t->refused;
        U.nreg = 0;
        for (int i = 0; i < t->nreg; i++) {
            Reg r = t->reg[i];
            if (wr_hits(r.lo, r.hi)) {
                U.inflight++;
                continue;
            }
            if (U.nreg == U.creg) U.reg = grow(U.reg, &U.creg, sizeof *U.reg);
            U.reg[U.nreg++] = (Reg){r.lo, r.hi, total, 0};
            t->reg[U.nreg - 1].off = r.off;   // source offset of the kept region, in step with U.reg
            total += (r.hi - r.lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
        }
    } else {
        total = gather(&U.c, handles, n, &U.reg, &U.nreg, &U.creg, &U.refused, &U.inflight, 1);
    }
    if (!U.nreg || !(U.arena = arena_get(total))) {
        vk_usnap_free(t);
        return;
    }
    for (int i = 0; i < U.nreg; i++) {
        const uint8_t *src = t ? t->data + t->reg[i].off : (const uint8_t *)(uintptr_t)U.reg[i].lo;
        memcpy(U.arena->map + U.reg[i].off, src, U.reg[i].hi - U.reg[i].lo);
        if (t && memcmp(src, (const void *)(uintptr_t)U.reg[i].lo, U.reg[i].hi - U.reg[i].lo)) U.changed++;
    }
    vmaFlushAllocation(vk_ctx()->vma, U.arena->alloc, 0, total);   // a no-op on coherent memory
    U.on = 1;
    U.subs++, U.regions += (uint64_t)U.nreg, U.bytes += total, U.copy_ns += kern_now_ns() - t0 + (t ? t->copy_ns : 0);
    vk_usnap_free(t);
}

void vk_usnap_end(uint64_t fence) {
    if (U.arena) vk_completion_add(fence, arena_done, U.arena), U.arena = NULL;
    U.on = 0, U.gen++;
}

// First region whose end is above a.
static int first_above(uint64_t a) {
    int lo = 0, hi = U.nreg;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (U.reg[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (!U.on || !n) return 0;
    int i = first_above(addr);
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
    U.hits++;
    return 1;
}

int vk_usnap_read(uint64_t addr, uint64_t n, void *out) {
    if (!U.on || !n) return 0;
    int i = first_above(addr);
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi || !U.arena->map) return 0;
    memcpy(out, U.arena->map + r->off + (addr - r->lo), n);
    return 1;
}

void vk_usnap_drop(uint64_t addr, uint64_t n) {
    if (!U.mode || !n) return;
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence};
    if (!U.on) return;
    for (int i = first_above(addr); i < U.nreg && U.reg[i].lo < addr + n; i++)
        if (!U.reg[i].dropped) U.reg[i].dropped = 1, U.drops++, U.gen++;
}

uint64_t vk_usnap_gen(void) { return U.gen; }

// The arenas back on the free list (vk_exec_draw_shutdown: the device is idle, every completion ran).
void vk_usnap_shutdown(void) {
    plat_mutex_lock(&free_lock);
    for (Arena *a = free_arenas, *next; a; a = next) next = a->next, vmaDestroyBuffer(vk_ctx()->vma, a->buf, a->alloc), free(a);
    free_arenas = NULL;
    plat_mutex_unlock(&free_lock);
}
