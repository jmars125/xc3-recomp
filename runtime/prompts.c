// Button prompts for Xbox and PlayStation pads (XC3_PROMPTS). The game draws its button glyphs from two atlases,
// menu/image/icon_button.wilay (64x64 cells: white discs with the letter cut out) and icon_button_y.wilay (72x72
// cells: cream discs with brown letters and an orange glow). Row 0, columns 4-7 hold X, Y, A and B. The game reads each
// from bf3.ard as an xbc1 container: first the 48-byte header (magic, compression, decompressed size, compressed size,
// hash, a 28-byte name), then the compressed bytes. romfs_read hands both reads to prompts_read: the header gets the
// size of a recompressed patched file and the read after it gets that file. The add-on mounts' loose copies of these
// paths, which the game asks for first, do not exist. The same hooks serve the button diamonds palette.c moves under
// pad_layout = xbox, read from bf3.ard or from an add-on archive (fs_host.c, read_file: DLC 3 has its own battle HUD).
// A patched file must not grow past its stored compressed size, which the game reads it with.
//
// The texture is the file's one Mibl (footer magic LBIM: data size, alignment, width, height, depth, view dimension,
// format, mip count, version), BC7 (format 77) in Tegra block-linear layout, mips one after another. Changed cells are
// decoded, redrawn and encoded again as BC7 mode 6 (one subset, RGBA endpoints); the rest keeps its blocks.
//
// Which glyph goes in a cell follows where that Switch button sits on the pad (pad_layout = nintendo or auto: X top,
// Y left, B bottom, A right): an Xbox pad shows Y, X, A, B there and a PlayStation pad triangle, square, cross, circle.
// With pad_layout = xbox (Switch A on the button labelled A) the Xbox letters are the Switch ones and a PlayStation pad
// shows square, triangle, cross, circle for X, Y, A, B. Picked once, when the game loads the atlases at start.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "palette.h"
#include "prompts.h"

enum { GLYPH_X, GLYPH_Y, GLYPH_A, GLYPH_B, GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_KEEP };

// ---------------------------------------------------------------- BC7
typedef struct { const uint8_t *p; unsigned pos; } Bits;
static unsigned bits(Bits *b, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++, b->pos++) v |= (unsigned)((b->p[b->pos >> 3] >> (b->pos & 7)) & 1) << i;
    return v;
}

// Subsets of the 2-subset partitions (bit i: pixel i in subset 1) and the second subset's anchor pixel.
static const uint16_t part2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00,
    0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c,
    0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8,
    0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c,
    0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22};
static const uint8_t anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
// The 3-subset partitions (2 bits per pixel, pixel i at bits 2i) and the anchors of subsets 1 and 2.
static const uint32_t part3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050, 0xaa550000,
    0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054,
    0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500, 0x0050a4a4, 0xaaa59090, 0x14696914,
    0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424,
    0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0,
    0x69286928, 0x44aaaa44, 0x66666600, 0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580,
    0xaa141414, 0x96960000, 0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44,
    0x2a4a5254};
static const uint8_t anchor3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3,
    5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
static const uint8_t anchor3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};
static const uint8_t w2[4] = {0, 21, 43, 64}, w3[8] = {0, 9, 18, 27, 37, 46, 55, 64},
                     w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
static const uint8_t *weights(unsigned n) { return n == 2 ? w2 : n == 3 ? w3 : w4; }
static uint8_t lerp6(unsigned a, unsigned b, unsigned w) { return (uint8_t)(((64 - w) * a + w * b + 32) >> 6); }
static unsigned expand(unsigned v, unsigned n) { return (v << (8 - n)) | (v >> (2 * n - 8)); }

// One block to 16 RGBA pixels; the reserved mode 8 (a first byte of 0) decodes to transparent black.
static void bc7_decode(const uint8_t blk[16], uint8_t out[16][4]) {
    static const struct { uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; } M[8] = {
        {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
        {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
        {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};
    Bits b = {blk, 0};
    unsigned mode = 0;
    while (mode < 8 && !bits(&b, 1)) mode++;
    if (mode == 8) { memset(out, 0, 64); return; }
    unsigned ns = M[mode].ns, part = bits(&b, M[mode].pb), rot = bits(&b, M[mode].rb), isel = bits(&b, M[mode].isb);
    unsigned e[3][2][4] = {{{0}}};   // subset, endpoint, channel
    for (unsigned ch = 0; ch < 3; ch++)
        for (unsigned s = 0; s < ns; s++)
            for (unsigned k = 0; k < 2; k++) e[s][k][ch] = bits(&b, M[mode].cb);
    if (M[mode].ab)
        for (unsigned s = 0; s < ns; s++)
            for (unsigned k = 0; k < 2; k++) e[s][k][3] = bits(&b, M[mode].ab);
    unsigned cbits = M[mode].cb, abits = M[mode].ab;
    if (M[mode].epb || M[mode].spb) {
        unsigned p[3][2];
        for (unsigned s = 0; s < ns; s++) {
            if (M[mode].epb) p[s][0] = bits(&b, 1), p[s][1] = bits(&b, 1);
            else p[s][0] = p[s][1] = bits(&b, 1);
        }
        for (unsigned s = 0; s < ns; s++)
            for (unsigned k = 0; k < 2; k++)
                for (unsigned ch = 0; ch < 4; ch++) e[s][k][ch] = e[s][k][ch] << 1 | p[s][k];
        cbits++;
        if (abits) abits++;
    }
    for (unsigned s = 0; s < ns; s++)
        for (unsigned k = 0; k < 2; k++) {
            for (unsigned ch = 0; ch < 3; ch++) e[s][k][ch] = expand(e[s][k][ch], cbits);
            e[s][k][3] = abits ? expand(e[s][k][3], abits) : 255;
        }
    unsigned a1 = ns == 2 ? anchor2[part] : ns == 3 ? anchor3a[part] : 0, a2 = ns == 3 ? anchor3b[part] : 0;
    unsigned ib = M[mode].ib, ib2 = M[mode].ib2;
    uint8_t i1[16], i2[16];
    for (unsigned i = 0; i < 16; i++) i1[i] = (uint8_t)bits(&b, ib - (i == 0 || (ns > 1 && i == a1) || (ns == 3 && i == a2)));
    if (ib2)
        for (unsigned i = 0; i < 16; i++) i2[i] = (uint8_t)bits(&b, ib2 - (i == 0));
    for (unsigned i = 0; i < 16; i++) {
        unsigned s = ns == 2 ? (part2[part] >> i) & 1 : ns == 3 ? (part3[part] >> 2 * i) & 3 : 0;
        const uint8_t *wc = weights(ib), *wa = wc;
        unsigned ic = i1[i], ia = i1[i];
        if (ib2) {
            if (isel) wc = weights(ib2), ic = i2[i];
            else wa = weights(ib2), ia = i2[i];
        }
        for (unsigned ch = 0; ch < 3; ch++) out[i][ch] = lerp6(e[s][0][ch], e[s][1][ch], wc[ic]);
        out[i][3] = lerp6(e[s][0][3], e[s][1][3], wa[ia]);
        if (rot) {
            uint8_t t = out[i][3];
            out[i][3] = out[i][rot - 1];
            out[i][rot - 1] = t;
        }
    }
}

typedef struct { uint8_t *p; unsigned pos; } WBits;
static void put(WBits *b, unsigned v, unsigned n) {
    for (unsigned i = 0; i < n; i++, b->pos++)
        if ((v >> i) & 1) b->p[b->pos >> 3] |= (uint8_t)(1u << (b->pos & 7));
}

// 16 RGBA pixels to a mode 6 block: endpoints on the principal axis of the pixels, each 7 bits per channel plus a
// p-bit, and the nearest of the 16 interpolated colours for each pixel.
static void bc7_encode6(const uint8_t px[16][4], uint8_t blk[16]) {
    double mean[4] = {0}, cov[4][4] = {{0}}, ax[4] = {1, 1, 1, 1};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) mean[c] += px[i][c] / 16.0;
    for (int i = 0; i < 16; i++)
        for (int a = 0; a < 4; a++)
            for (int c = 0; c < 4; c++) cov[a][c] += (px[i][a] - mean[a]) * (px[i][c] - mean[c]);
    for (int it = 0; it < 8; it++) {   // power iteration
        double n[4] = {0}, len = 0;
        for (int a = 0; a < 4; a++)
            for (int c = 0; c < 4; c++) n[a] += cov[a][c] * ax[c];
        for (int a = 0; a < 4; a++) len += n[a] * n[a];
        if (len < 1e-9) break;
        len = sqrt(len);
        for (int a = 0; a < 4; a++) ax[a] = n[a] / len;
    }
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 16; i++) {
        double t = 0;
        for (int c = 0; c < 4; c++) t += (px[i][c] - mean[c]) * ax[c];
        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }
    unsigned q[2][4], pb[2];
    for (int k = 0; k < 2; k++) {
        double t = k ? hi : lo;
        int v[4];
        for (int c = 0; c < 4; c++) {
            double x = mean[c] + t * ax[c];
            v[c] = x < 0 ? 0 : x > 255 ? 255 : (int)(x + 0.5);
        }
        unsigned best = ~0u;
        for (unsigned p = 0; p < 2; p++) {   // the p-bit with the smaller error
            unsigned err = 0, qq[4];
            for (int c = 0; c < 4; c++) {
                int r = (v[c] - (int)p + 1) >> 1;
                qq[c] = (unsigned)(r < 0 ? 0 : r > 127 ? 127 : r);
                int d = (int)(qq[c] << 1 | p) - v[c];
                err += (unsigned)(d * d);
            }
            if (err < best) best = err, pb[k] = p, memcpy(q[k], qq, sizeof qq);
        }
    }
    uint8_t pal[16][4];
    for (int k = 0; k < 16; k++)
        for (int c = 0; c < 4; c++) pal[k][c] = lerp6(q[0][c] << 1 | pb[0], q[1][c] << 1 | pb[1], w4[k]);
    uint8_t idx[16];
    for (int i = 0; i < 16; i++) {
        unsigned best = ~0u;
        for (int k = 0; k < 16; k++) {
            unsigned err = 0;
            for (int c = 0; c < 4; c++) {
                int d = pal[k][c] - px[i][c];
                err += (unsigned)(d * d);
            }
            if (err < best) best = err, idx[i] = (uint8_t)k;
        }
    }
    if (idx[0] & 8) {   // the anchor index has 3 bits: swap the endpoints
        unsigned t[4];
        memcpy(t, q[0], sizeof t), memcpy(q[0], q[1], sizeof t), memcpy(q[1], t, sizeof t);
        unsigned tp = pb[0];
        pb[0] = pb[1], pb[1] = tp;
        for (int i = 0; i < 16; i++) idx[i] = (uint8_t)(15 - idx[i]);
    }
    memset(blk, 0, 16);
    WBits b = {blk, 0};
    put(&b, 1u << 6, 7);
    for (int c = 0; c < 4; c++) put(&b, q[0][c], 7), put(&b, q[1][c], 7);
    put(&b, pb[0], 1), put(&b, pb[1], 1);
    for (int i = 0; i < 16; i++) put(&b, idx[i], i ? 4 : 3);
}

// ---------------------------------------------------------------- Tegra block linear
static unsigned bh_mip0(unsigned h) {   // GOBs per block for a surface h blocks high
    unsigned hh = h + h / 2;
    return hh >= 128 ? 16 : hh >= 64 ? 8 : hh >= 32 ? 4 : hh >= 16 ? 2 : 1;
}
static unsigned bh_mip(unsigned h, unsigned bh) {
    while (bh > 1 && h <= bh / 2 * 8) bh /= 2;
    return bh;
}
static size_t gob_addr(unsigned x, unsigned y, unsigned wbytes, unsigned bh) {   // x in bytes, y in block rows
    unsigned gobs_x = (wbytes + 63) / 64, rows = 8 * bh;
    size_t base = ((size_t)(y / rows) * gobs_x + x / 64) * 512u * bh + (size_t)((y % rows) / 8) * 512;
    unsigned xo = x % 64, yo = y % 8;
    return base + (xo / 32) * 256 + (yo / 2) * 64 + ((xo % 32) / 16) * 32 + (yo % 2) * 16 + xo % 16;
}
static size_t level_size(unsigned wb, unsigned hb, unsigned bh) {
    unsigned rows = 8 * bh;
    return (size_t)((wb * 16 + 63) / 64) * ((hb + rows - 1) / rows) * 512u * bh;
}

// ---------------------------------------------------------------- glyphs
typedef struct { int x, y, w, h; } Rect;
typedef struct { uint8_t *px; int w, h; } Img;   // RGBA rows
static uint8_t *at(Img *m, int x, int y) { return m->px + ((size_t)y * m->w + x) * 4; }

// Coverage of the PlayStation shape at (u, v), in disc radii from the centre, +v down.
static int in_shape(int g, double u, double v) {
    double t = 0.135;   // stroke
    switch (g) {
    case GLYPH_CIRCLE: { double r = sqrt(u * u + v * v); return r < 0.46 + t / 2 && r > 0.46 - t / 2; }
    case GLYPH_CROSS: {
        double a = fabs(u + v) / sqrt(2), b = fabs(u - v) / sqrt(2);
        return (a < t / 2 && b < 0.5) || (b < t / 2 && a < 0.5);
    }
    case GLYPH_SQUARE: {
        double m = fmax(fabs(u), fabs(v));
        return m < 0.37 + t / 2 && m > 0.37 - t / 2;
    }
    case GLYPH_TRIANGLE: {   // edges of an upward triangle with circumradius 0.5, centred on its centroid
        double R = 0.5, d[3];
        for (int k = 0; k < 3; k++) {
            double ang = -M_PI / 2 + k * 2 * M_PI / 3 + M_PI / 3;   // outward normal of edge k
            d[k] = u * cos(ang) + v * sin(ang) - R / 2;               // signed distance (inside < 0)
        }
        double m = fmax(d[0], fmax(d[1], d[2]));
        return m < t / 2 && m > -t / 2;
    }
    }
    return 0;
}

// Redraws cell r of m as PlayStation glyph g in the style of the letter there: the disc colour (its brightest opaque
// pixels) fills the disc, then the shape takes the letter's colour (the pixel inside the disc farthest from the disc
// colour; transparent in icon_button), 4x4 samples per pixel.
static void draw_ps(Img *m, Rect r, int g) {
    int maxl = -1;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint8_t *p = at(m, x, y);
            if (p[3] >= 250 && p[0] + p[1] + p[2] > maxl) maxl = p[0] + p[1] + p[2];
        }
    if (maxl < 0) return;
    double D[4] = {0}, n = 0, cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint8_t *p = at(m, x, y);
            if (p[3] >= 250 && p[0] + p[1] + p[2] >= maxl - 30) {
                for (int c = 0; c < 4; c++) D[c] += p[c];
                n++;
            }
        }
    for (int c = 0; c < 4; c++) D[c] /= n;
    double R = 0;   // the disc: the farthest pixel of the disc colour from the cell centre
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint8_t *p = at(m, x, y);
            double dc = fabs(p[0] - D[0]) + fabs(p[1] - D[1]) + fabs(p[2] - D[2]);
            if (p[3] >= 240 && dc < 60) {
                double d = hypot(x + 0.5 - cx, y + 0.5 - cy);
                if (d > R) R = d;
            }
        }
    if (R < 4) return;
    double L[4] = {0}, far = -1;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            if (hypot(x + 0.5 - cx, y + 0.5 - cy) > 0.6 * R) continue;
            uint8_t *p = at(m, x, y);
            double d = fabs(p[0] - D[0]) + fabs(p[1] - D[1]) + fabs(p[2] - D[2]) + 2 * fabs(p[3] - D[3]);
            if (d > far) {
                far = d;
                for (int c = 0; c < 4; c++) L[c] = p[c];
            }
        }
    if (L[3] < 16) L[0] = D[0], L[1] = D[1], L[2] = D[2];   // a cut-out: no dark fringe
    double inner = R - 1.5;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            if (hypot(x + 0.5 - cx, y + 0.5 - cy) > inner) continue;
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    hit += in_shape(g, (x + (sx + 0.5) / 4 - cx) / R, (y + (sy + 0.5) / 4 - cy) / R);
            uint8_t *p = at(m, x, y);
            for (int c = 0; c < 4; c++) p[c] = (uint8_t)(D[c] + (L[c] - D[c]) * hit / 16.0 + 0.5);
        }
}

// 2x2 box filter of src rect sr into dst rect dr (half the size, rounded down).
static void shrink(Img *dst, Rect dr, Img *src, Rect sr) {
    for (int y = 0; y < dr.h; y++)
        for (int x = 0; x < dr.w; x++)
            for (int c = 0; c < 4; c++) {
                int sx = sr.x + 2 * x, sy = sr.y + 2 * y, x1 = sx + 1 < sr.x + sr.w ? sx + 1 : sx,
                    y1 = sy + 1 < sr.y + sr.h ? sy + 1 : sy;
                at(dst, dr.x + x, dr.y + y)[c] =
                    (uint8_t)((at(src, sx, sy)[c] + at(src, x1, sy)[c] + at(src, sx, y1)[c] + at(src, x1, y1)[c] + 2) / 4);
            }
}

// Patches the Mibl texture at the end of a decompressed atlas: glyph[k] for the cells of X, Y, A, B. 0 when the file
// is not laid out as expected.
static int patch_atlas(uint8_t *file, size_t size, const int glyph[4]) {
    if (size < 0x28 || memcmp(file + size - 4, "LBIM", 4)) return 0;
    uint32_t f[9];
    memcpy(f, file + size - 0x28, sizeof f);
    uint32_t data_size = f[0], w = f[2], h = f[3], fmt = f[6], mips = f[7];
    if (fmt != 77 || data_size > size - 0x28 || w % 8 || !w || !h || mips > 16) return 0;
    uint8_t *data = file + size - data_size;   // the footer is the last 0x28 bytes of the data
    unsigned cell = w / 8, bh0 = bh_mip0((h + 3) / 4);
    Img prev = {0};
    Rect prev_cells[4];
    size_t off = 0;
    int ok = 1;
    for (uint32_t lv = 0; lv < mips && ok; lv++) {
        unsigned lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1, wb = (lw + 3) / 4, hb = (lh + 3) / 4;
        unsigned bh = bh_mip(hb, bh0);
        size_t ls = level_size(wb, hb, bh);
        if (off + ls > data_size - 0x28) { ok = 0; break; }
        uint8_t *lvl = data + off;
        off += ls;
        Img m = {calloc((size_t)wb * hb * 64, 1), (int)(wb * 4), (int)(hb * 4)};
        if (!m.px) rt_fatal("prompts: out of memory");
        for (unsigned by = 0; by < hb && ok; by++)
            for (unsigned bx = 0; bx < wb && ok; bx++) {
                uint8_t px[16][4];
                bc7_decode(lvl + gob_addr(bx * 16, by, wb * 16, bh), px);
                for (int i = 0; i < 16; i++) memcpy(at(&m, (int)bx * 4 + i % 4, (int)by * 4 + i / 4), px[i], 4);
            }
        Rect cells[4];
        Img orig = {malloc((size_t)m.w * m.h * 4), m.w, m.h};
        if (!orig.px) rt_fatal("prompts: out of memory");
        memcpy(orig.px, m.px, (size_t)m.w * m.h * 4);
        for (int k = 0; k < 4; k++) {
            int c0 = (int)((4 + k) * cell) >> lv, c1 = (int)((5 + k) * cell) >> lv, r1 = (int)cell >> lv;
            cells[k] = (Rect){c0, 0, c1 - c0, r1};
        }
        for (int k = 0; k < 4 && ok; k++) {
            if (glyph[k] == GLYPH_KEEP || cells[k].w < 1 || cells[k].h < 1) continue;
            if (lv && prev.px && prev_cells[k].w >= 2) {   // lower levels follow the redrawn level above
                shrink(&m, cells[k], &prev, prev_cells[k]);
            } else if (glyph[k] <= GLYPH_B) {   // another letter of the atlas
                Rect s = cells[glyph[k]];
                for (int y = 0; y < cells[k].h; y++) memcpy(at(&m, cells[k].x, y), at(&orig, s.x, s.y + y), (size_t)s.w * 4);
            } else {
                draw_ps(&m, cells[k], glyph[k]);
            }
        }
        if (ok)   // encode the blocks that changed
            for (unsigned by = 0; by < hb; by++)
                for (unsigned bx = 0; bx < wb; bx++) {
                    uint8_t px[16][4];
                    int diff = 0;
                    for (int i = 0; i < 16; i++) {
                        memcpy(px[i], at(&m, (int)bx * 4 + i % 4, (int)by * 4 + i / 4), 4);
                        diff |= memcmp(px[i], at(&orig, (int)bx * 4 + i % 4, (int)by * 4 + i / 4), 4);
                    }
                    if (diff) bc7_encode6(px, lvl + gob_addr(bx * 16, by, wb * 16, bh));
                }
        free(orig.px);
        free(prev.px);
        prev = m;
        memcpy(prev_cells, cells, sizeof cells);
    }
    free(prev.px);
    return ok;
}

// ---------------------------------------------------------------- the read hook
// Files patched as the game reads them: the two glyph atlases (glyphs picked once, at the first read) and the button
// diamonds of palette.c (moved while pad_layout is xbox, looked at on every read of the file's header, so a change of
// layout in the menu shows the next time the game loads that layout). Each patched file is built once per container
// and kept; a read of the header under the other choice gets the original size, and the payload read after it is
// left alone.
static const char *const atlases[] = {"icon_button.wilay", "icon_button_y.wilay"};
enum { ATLASES = 2, MAX_PATCHED = 8 };
typedef struct {
    char name[29];
    uint64_t file, at;   // file: RomFS id; at: header offset
    uint8_t *z;
    size_t zn;
    int failed, live;    // live: the last header read got the patched size
} Patched;
static Patched patched[MAX_PATCHED];   // [0], [1]: the atlases
static PlatMutex lock = PLAT_MUTEX_INIT;
static int glyphs[4] = {GLYPH_KEEP, GLYPH_KEEP, GLYPH_KEEP, GLYPH_KEEP}, picked;

static void pick(void) {
    if (picked) return;
    picked = 1;
    const char *set = cfg_value("XC3_PROMPTS");
    int kind = 0;   // 0 Switch, 1 Xbox, 2 PlayStation
    if (set && !strcasecmp(set, "xbox")) kind = 1;
    else if (set && !strcasecmp(set, "playstation")) kind = 2;
    else if (!set || !strcasecmp(set, "auto")) {
        int south = input_pad_south_label();
        kind = south == PAD_LABEL_CROSS ? 2 : south == PAD_LABEL_A ? 1 : 0;
    }
    int by_label = input_pad_layout_pick(cfg_value("XC3_PAD_LAYOUT")) == PAD_LAYOUT_XBOX;
    static const int xbox_pos[4] = {GLYPH_Y, GLYPH_X, GLYPH_B, GLYPH_A},
                     ps_pos[4] = {GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_CIRCLE, GLYPH_CROSS},
                     ps_label[4] = {GLYPH_SQUARE, GLYPH_TRIANGLE, GLYPH_CROSS, GLYPH_CIRCLE};
    if (kind == 1 && !by_label) memcpy(glyphs, xbox_pos, sizeof glyphs);
    if (kind == 2) memcpy(glyphs, by_label ? ps_label : ps_pos, sizeof glyphs);
    rt_log("prompts: %s glyphs (prompts = %s, pad_layout %s)", kind == 2 ? "PlayStation" : kind == 1 ? "Xbox" : "Switch",
           set ? set : "auto", by_label ? "by label" : "by position");
}

static int any_change(void) {
    for (int k = 0; k < 4; k++)
        if (glyphs[k] != GLYPH_KEEP) return 1;
    return 0;
}

// Builds p's patched container from its header and the compressed bytes after it; 0 on any failure.
static int build(Patched *p, int atlas, const uint8_t hdr[48], uint64_t handle, uint64_t file, int64_t offset,
                 uint32_t (*raw)(uint64_t *, uint64_t, int64_t, void *, uint64_t)) {
    uint32_t comp, dsize, csize;
    memcpy(&comp, hdr + 4, 4), memcpy(&dsize, hdr + 8, 4), memcpy(&csize, hdr + 12, 4);
    if (comp != 3 || dsize > (64u << 20) || csize > (64u << 20)) return 0;
    uint8_t *z = malloc(csize), *d = malloc(dsize);
    if (!z || !d) rt_fatal("prompts: out of memory");
    uint64_t got = 0;
    int moved = 0,
        ok = !raw(&got, handle, offset + 48, z, csize) && got == csize && ZSTD_decompress(d, dsize, z, csize) == dsize &&
             (atlas ? patch_atlas(d, dsize, glyphs) : (moved = palette_patch(d, dsize, p->name)) > 0);
    free(z);
    if (ok) {
        size_t cap = ZSTD_compressBound(dsize);
        uint8_t *out = malloc(cap);
        if (!out) rt_fatal("prompts: out of memory");
        // A patched file must not grow: the game reads it with the archive index's compressed size and cuts a longer
        // stream short, then crashes as the HUD loads (the glyph atlases under pad_layout = nintendo grew by 3%, the
        // shortcut menu's layout by 4%). Stronger levels until it fits.
        static const int levels[] = {9, 19, 22};
        size_t zn = 0;
        for (int l = 0; l < 3; l++) {
            zn = ZSTD_compress(out, cap, d, dsize, levels[l]);
            if (ZSTD_isError(zn) || zn <= csize) break;
        }
        if (ZSTD_isError(zn) || zn > csize) ok = 0, free(out);
        else p->file = file, p->at = (uint64_t)offset, p->z = out, p->zn = zn;
    }
    free(d);
    if (!ok) rt_log("%s: %s left as it is (unexpected layout, or it would not fit)", atlas ? "prompts" : "palette", p->name);
    else if (atlas) rt_log("prompts: %s patched (%u -> %zu compressed bytes)", p->name, csize, p->zn);
    else rt_log("palette: %s moved to the xbox layout (%d keys; %u -> %zu compressed bytes)", p->name, moved, csize, p->zn);
    return ok;
}

int prompts_payload(uint64_t file, int64_t offset, void *buffer, uint64_t size, uint64_t *out) {
    int served = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < MAX_PATCHED && !served; i++) {
        Patched *p = &patched[i];
        if (!p->z || !p->live || p->file != file || (uint64_t)offset < p->at + 48 || (uint64_t)offset >= p->at + 48 + p->zn)
            continue;
        uint64_t from = (uint64_t)offset - p->at - 48, n = size < p->zn - from ? size : p->zn - from;
        memcpy(buffer, p->z + from, n);
        if (size > n) memset((uint8_t *)buffer + n, 0, size - n);
        *out = size;
        served = 1;
    }
    plat_mutex_unlock(&lock);
    return served;
}

void prompts_header(uint64_t handle, uint64_t file, int64_t offset, void *buffer, uint64_t size,
                    uint32_t (*raw)(uint64_t *, uint64_t, int64_t, void *, uint64_t)) {
    uint8_t *b = buffer;
    if (size != 48 || memcmp(b, "xbc1", 4)) return;
    char name[29] = {0};
    memcpy(name, b + 20, 28);
    int atlas = -1;
    for (int i = 0; i < ATLASES; i++)
        if (!strcmp(name, atlases[i])) atlas = i;
    if (atlas < 0 && !palette_file(name)) return;
    plat_mutex_lock(&lock);
    Patched *p = NULL;
    if (atlas >= 0) p = &patched[atlas];
    for (int i = ATLASES; i < MAX_PATCHED && !p; i++)
        if (!strcmp(patched[i].name, name) || !patched[i].name[0]) p = &patched[i];
    if (p) {
        if (!p->name[0]) memcpy(p->name, name, sizeof name);
        int want;
        if (atlas >= 0) pick(), want = any_change();
        else want = input_pad_layout_pick(cfg_value("XC3_PAD_LAYOUT")) == PAD_LAYOUT_XBOX;
        p->live = 0;
        if (want && !p->failed) {
            if (!p->z || p->at != (uint64_t)offset || p->file != file) {
                free(p->z);
                p->z = NULL, p->zn = 0;
                p->failed = !build(p, atlas >= 0, b, handle, file, offset, raw);
            }
            if (p->z) {
                uint32_t zn = (uint32_t)p->zn;
                memcpy(b + 12, &zn, 4);
                p->live = 1;
            }
        }
        if (atlas < 0) rt_log("palette: %s read with %s places", name, p->live ? "xbox" : "Switch");
    }
    plat_mutex_unlock(&lock);
}
