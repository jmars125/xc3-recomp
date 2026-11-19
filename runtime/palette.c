// Button diamonds that follow the pad layout (prompts.c reads them through palette_patch). Under pad_layout = xbox
// Switch A is the south button, B east, X west and Y north, but the game draws its button diamonds in Switch places:
// A right, B bottom, X top, Y left. The battle palette (menu/image/mnu417_cont01.wilay, mc_GroupPallet) is such a
// diamond: on the right the talent art on A and the arts on X, Y and B, on the left the master arts mirrored (A outer,
// Y inner). The same file holds the Interlink palette (mc_GroupInterLink: X, Y and B on each side, mirrored on the
// left); mnu414_cont06.wilay holds a three-art palette and mnu501_cont01.wilay the shortcut menu's glyph cross with a
// label per button. palette_patch moves each diamond's slots to the places their buttons have under the xbox layout:
// slot offsets from the diamond's centre are transposed, (dx, dy) to (dy, dx) (y points down), or (-dy, -dx) on a
// mirrored side, so A goes to the bottom, B right, X left and Y top (mirrored: B outer, Y top, X inner). The larger
// A slot keeps its distance from the centre. Labels go with their slots: X and Y swap label places, B takes A's
// (the right side), and A's label moves as its slot does (it sits on the other side of its slot).
//
// The files are LAGP layouts: a Flash-like tree of movie clips ("symbols") whose children are placed by animation
// tracks. Field names follow xc3_lib (MIT, ScanMountGoat, xc3_lib/src/lagp.rs and dhal.rs), which leaves most of
// them unnamed; the meaning of the ones used here was read off the battle HUD's data. Offsets are from the file start
// unless said otherwise; all values little endian.
//   +0x18 u32: the track table's base T. At T + 4: the offset (from T) of the rows, 0x40 bytes each.
//   +0x3c u32: the symbol table's base S. At S: offset (from S) and count of the symbols, 28 bytes each:
//     +4 placements (from S): u32 offset (from there) of one (u32 flags, u32 track << 16 | 0xffff) per child;
//     +12 children (from S): u32 count, u32 offset (from there) of (u32 kind, u16 slot, u16 symbol, u32) per child,
//     kind 0 a movie clip; the child's placement is entry `slot`; +24 the name (from S).
//   A track row: +8..+0x24 eight data offsets (from T), d1..d8; +0x30 kind: channels << 16 | 1 constant, 2 keyed.
//   Translation keys: with channel bit 3 (0xb0001, 0x80002) d1 holds (0, 0) and the translation follows: a constant
//   at d1 + 8, keys as (x, y, z) triples from d2 + 20 up to d5 (more triples of other channels follow, so only the
//   run near the first is taken). Without it, (x, y) pairs from d1 up to d2 (or d5 when there is no d2).
// Positions are in stage units: the stage is 1.0 wide and 0.5625 high (the 1280x720 design), y down.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "palette.h"

enum { SX, SY, SA, SB, SLOTS };   // the diamond's buttons, by Switch letter
enum { MAX_KEYS = 64, MAX_MOVES = 256 };

typedef struct {
    const char *icons;                     // parent symbol of the slots
    const char *icon[SLOTS];               // the slot children for X, Y, A and B; NULL: no such slot
    const char *labels;                    // parent symbol of the labels, NULL: none
    const char *label[SLOTS];
    int mirrored;                          // the left half of a pair of palettes
} Diamond;

typedef struct { const char *file, *future_redeemed; Diamond d[4]; int n; } Layout;

static const Layout layouts[] = {
    {"mnu417_cont01.wilay", "mnu403_cont01.wilay",
     {{"mc_Btn", {"mc_BtnR01", "mc_BtnR02", "mc_BtnR04", "mc_BtnR03"}, "mc_Info",
       {"mc_InfoR01", "mc_InfoR02", "mc_InfoR04", "mc_InfoR03"}, 0},
      {"mc_Btn", {"mc_BtnL01", "mc_BtnL02", "mc_BtnL04", "mc_BtnL03"}, "mc_Info",
       {"mc_InfoL01", "mc_InfoL02", "mc_InfoL04", "mc_InfoL03"}, 1},
      {"mc_InterLinkAnim", {"mc_InterLinkR01", "mc_InterLinkR02", NULL, "mc_InterLinkR03"}, NULL, {0}, 0},
      {"mc_InterLinkAnim", {"mc_InterLinkL01", "mc_InterLinkL02", NULL, "mc_InterLinkL03"}, NULL, {0}, 1}},
     4},
    {"mnu414_cont06.wilay", "mnu414_cont10.wilay",
     {{"mc_Btn", {"mc_BtnR01", "mc_BtnR02", NULL, "mc_BtnR03"}, "mc_Info", {"mc_InfoR01", "mc_InfoR02", NULL, "mc_InfoR03"},
       0}},
     1},
    {"mnu501_cont01.wilay", NULL,
     {{"mc_ShortcutAll", {"mc_IconBtnX", "mc_IconBtnY", "mc_IconBtnA", "mc_IconBtnB"}, "mc_ShortcutAll",
       {"mc_BtnX", "mc_BtnY", "mc_BtnA", "mc_BtnB"}, 0}},
     1},
};

static const Layout *find_layout(const char *name) {
    for (size_t i = 0; i < sizeof layouts / sizeof *layouts; i++) {
        const Layout *l = &layouts[i];
        if (!strcmp(l->file, name) || (l->future_redeemed && !strcmp(l->future_redeemed, name))) return l;
    }
    return NULL;
}

typedef struct { uint8_t *p; size_t n; uint32_t syms, nsyms, tracks, rows, ntracks; } Lagp;

static int u32at(const Lagp *f, size_t o, uint32_t *v) {
    if (o > f->n || f->n - o < 4) return 0;
    memcpy(v, f->p + o, 4);
    return 1;
}

static const char *cstr(const Lagp *f, size_t o) {
    if (o >= f->n || !memchr(f->p + o, 0, f->n - o)) return NULL;
    return (const char *)f->p + o;
}

static int lagp_open(Lagp *f, uint8_t *p, size_t n) {
    *f = (Lagp){p, n, 0, 0, 0, 0, 0};
    uint32_t off;
    if (n < 0x70 || memcmp(p, "LAGP", 4) || !u32at(f, 0x18, &f->tracks) || !u32at(f, 0x3c, &f->syms) ||
        !u32at(f, f->syms, &off) || !u32at(f, f->syms + 4, &f->nsyms) || !u32at(f, f->tracks + 4, &f->rows) ||
        !u32at(f, f->tracks + 8, &f->ntracks))
        return 0;
    f->rows += f->tracks;
    return (uint64_t)f->syms + off + 28ull * f->nsyms <= n && (uint64_t)f->rows + 0x40ull * f->ntracks <= n;
}

static int sym_field(const Lagp *f, uint32_t i, int k, uint32_t *v) {
    uint32_t off;
    return u32at(f, f->syms, &off) && u32at(f, (size_t)f->syms + off + 28u * i + 4u * k, v);
}

static const char *sym_name(const Lagp *f, uint32_t i) {
    uint32_t o;
    return sym_field(f, i, 6, &o) ? cstr(f, (size_t)f->syms + o) : NULL;
}

static int sym_find(const Lagp *f, const char *name, uint32_t *out) {
    for (uint32_t i = 0; i < f->nsyms; i++) {
        const char *s = sym_name(f, i);
        if (s && !strcmp(s, name)) return *out = i, 1;
    }
    return 0;
}

// The track that places child `name` of symbol parent; 0 when there is none.
static int child_track(const Lagp *f, uint32_t parent, const char *name, uint32_t *track) {
    uint32_t place, kids, n, off, poff;
    if (!sym_field(f, parent, 1, &place) || !sym_field(f, parent, 3, &kids)) return 0;
    size_t k0 = (size_t)f->syms + kids, p0 = (size_t)f->syms + place;
    if (!u32at(f, k0, &n) || !u32at(f, k0 + 4, &off) || !u32at(f, p0, &poff)) return 0;
    for (uint32_t k = 0; k < n && k < 4096; k++) {
        size_t e = k0 + off + 12u * k;
        uint32_t kind, b;
        uint16_t slot, sym;
        if (!u32at(f, e, &kind) || e + 8 > f->n) return 0;
        memcpy(&slot, f->p + e + 4, 2), memcpy(&sym, f->p + e + 6, 2);
        const char *s = kind == 0 && sym < f->nsyms ? sym_name(f, sym) : NULL;
        if (!s || strcmp(s, name)) continue;
        if (!u32at(f, p0 + poff + 8u * slot + 4, &b)) return 0;
        *track = b >> 16;
        return *track < f->ntracks;
    }
    return 0;
}

// The translation keys of a track: file offsets of (x, y) float pairs. 0 when the track is not one of the encodings
// above.
static int track_keys(const Lagp *f, uint32_t t, size_t *keys) {
    uint32_t r[16];
    size_t row = f->rows + 0x40ull * t;
    for (int i = 0; i < 16; i++)
        if (!u32at(f, row + 4u * i, &r[i])) return 0;
    uint32_t d1 = r[2], d2 = r[3], d5 = r[6], kind = r[12], ch = kind >> 16, how = kind & 0xffff;
    if (!d1 || (how != 1 && how != 2)) return 0;
    size_t b1 = (size_t)f->tracks + d1;
    float v[2];
    if (ch & 8) {
        if (how == 1) {
            if (b1 + 16 > f->n) return 0;
            memcpy(v, f->p + b1, 8);
            if (v[0] != 0 || v[1] != 0) return 0;
            keys[0] = b1 + 8;
            return 1;
        }
        if (!d2 || d5 <= d2 + 20 || (size_t)f->tracks + d5 > f->n) return 0;
        size_t b = (size_t)f->tracks + d2 + 20, end = (size_t)f->tracks + d5;
        float first[2];
        memcpy(first, f->p + b, 8);
        int n = 0;
        for (; b + 12 <= end && n < MAX_KEYS; b += 12) {
            memcpy(v, f->p + b, 8);
            if (fabsf(v[0] - first[0]) > 0.03f || fabsf(v[1] - first[1]) > 0.03f) break;
            keys[n++] = b;
        }
        return n;
    }
    uint32_t e = d2 ? d2 : d5;
    if (e <= d1 || (e - d1) % 8 || (e - d1) / 8 > MAX_KEYS || (size_t)f->tracks + e > f->n) return 0;
    int n = (int)((e - d1) / 8);
    for (int i = 0; i < n; i++) keys[i] = b1 + 8u * i;
    return n;
}

typedef struct { size_t at; float x, y; } Move;
typedef struct { uint32_t track; size_t keys[MAX_KEYS]; int nkeys; float x, y; } Slot;   // x, y: the first key

static int slot_get(const Lagp *f, uint32_t parent, const char *name, Slot *s) {
    if (!child_track(f, parent, name, &s->track) || !(s->nkeys = track_keys(f, s->track, s->keys))) return 0;
    memcpy(&s->x, f->p + s->keys[0], 4), memcpy(&s->y, f->p + s->keys[0] + 4, 4);
    return 1;
}

// Shifts every key of s by (dx, dy), or maps it about centre (cx, cy) with the transpose when map is set.
static int moves_add(Move *m, int *n, const Lagp *f, const Slot *s, int map, int mirrored, float cx, float cy,
                     float dx, float dy) {
    for (int i = 0; i < s->nkeys; i++) {
        if (*n >= MAX_MOVES) return 0;
        float v[2];
        memcpy(v, f->p + s->keys[i], 8);
        if (map) {
            float ox = v[0] - cx, oy = v[1] - cy;
            v[0] = cx + (mirrored ? -oy : oy), v[1] = cy + (mirrored ? -ox : ox);
        } else {
            v[0] += dx, v[1] += dy;
        }
        m[(*n)++] = (Move){s->keys[i], v[0], v[1]};
    }
    return 1;
}

static int diamond_moves(const Lagp *f, const Diamond *d, Move *m, int *n) {
    uint32_t ip, lp = 0;
    Slot icon[SLOTS] = {0}, label[SLOTS] = {0};
    int has[SLOTS] = {0}, lab[SLOTS] = {0};
    if (!sym_find(f, d->icons, &ip) || (d->labels && !sym_find(f, d->labels, &lp))) return 0;
    for (int k = 0; k < SLOTS; k++) {
        if (!d->icon[k]) continue;
        if (!slot_get(f, ip, d->icon[k], &icon[k])) return 0;
        has[k] = 1;
        if (d->labels && d->label[k]) {
            if (!slot_get(f, lp, d->label[k], &label[k])) return 0;
            lab[k] = 1;
        }
    }
    if (!has[SX] || !has[SB]) return 0;
    float cx = (icon[SX].x + icon[SB].x) / 2, cy = (icon[SX].y + icon[SB].y) / 2;
    for (int k = 0; k < SLOTS; k++)
        if (has[k] && !moves_add(m, n, f, &icon[k], 1, d->mirrored, cx, cy, 0, 0)) return 0;
    // Labels: X and Y trade places, B takes A's place (or, without an A, mirrors its own place to the other side of its
    // slot), A follows its slot. B's banner is drawn for the left side, so on the right its connector dot is at the far
    // end; kept on the left, the banner would end on the A slot and read as A's.
    for (int k = 0; k < SLOTS; k++) {
        if (!lab[k]) continue;
        float ix = icon[k].x - cx, iy = icon[k].y - cy;
        float nx = cx + (d->mirrored ? -iy : iy), ny = cy + (d->mirrored ? -ix : ix);   // the slot's new place
        float tx, ty;
        if (k == SX && lab[SY]) tx = label[SY].x, ty = label[SY].y;
        else if (k == SY && lab[SX]) tx = label[SX].x, ty = label[SX].y;
        else if (k == SB && lab[SA]) tx = label[SA].x, ty = label[SA].y;
        else if (k == SA) tx = label[k].x + nx - icon[k].x, ty = label[k].y + ny - icon[k].y;
        else tx = nx - (label[k].x - icon[k].x), ty = ny + (label[k].y - icon[k].y);
        if (!moves_add(m, n, f, &label[k], 0, 0, 0, 0, tx - label[k].x, ty - label[k].y)) return 0;
    }
    return 1;
}

int palette_patch(uint8_t *file, size_t size, const char *name) {
    const Layout *l = find_layout(name);
    Lagp f;
    if (!l || !lagp_open(&f, file, size)) return 0;
    static Move m[MAX_MOVES];   // under prompts.c's lock
    int n = 0;
    for (int i = 0; i < l->n; i++)
        if (!diamond_moves(&f, &l->d[i], m, &n)) {
            rt_log("palette: %s: diamond %s/%s not as expected", name, l->d[i].icons, l->d[i].icon[SX]);
            return 0;
        }
    for (int i = 0; i < n; i++) memcpy(file + m[i].at, &m[i].x, 4), memcpy(file + m[i].at + 4, &m[i].y, 4);
    return n;
}

int palette_file(const char *name) {
    return find_layout(name) != NULL;
}
