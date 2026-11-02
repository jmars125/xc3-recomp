// Game resolution: the size the game composes its frame in, above or beside the shipped 1920x1080 (2560x1440,
// 3840x2160, 21:9 3440x1440). The game does the rendering itself, as with the community resolution mods for 1.0.1 by
// Fl4sh_ (github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods), which this follows.
//
// The game reads its frame size from a table in its read-only data, main+0x14dfd8c in 2.2.1 (1.0.0 +0x119ec0c):
// two sizes, shipped {1920, 1080, 1280, 720}. Its users load it through GOT entries (2.2.1 main+0x1b65858, +0x1b65860,
// +0x1b65938; read at main+0xea77a0, +0xed5a64, +0xf64b6c and others) and take the frame size and the aspect ratio
// from it, so a wider table widens the 3D view as well. The second size is the one the game draws its picture and
// interface in, docked too: shipped, the present gets 1280x720 frames, and with the first size alone made 2560x1080
// the picture fills only the top left 1280x540 of a 2560x1080 frame. res_init writes the table once the modules are
// loaded, before the game runs, with XC3_OUTPUT_RES = <w>x<h> (default 1920x1080) as both sizes, so the default is a
// whole 1920x1080 picture where the Switch draws 1280x720.
//
// One more site reads the table: main+0x9ccce0 sizes two buffers from a width it loads (main+0x9cccec) and the table's
// docked height (main+0x9ccd00). The mods replace both loads with sizes of their own that differ per mod (1280x720 at
// 3840x2160, 1920x1080 at 2560x1440, none at 5160x2160). The patch set "resbuf" (recomp/patches-upd.txt) loads them
// from the patch page's +0x50 and +0x54 instead: XC3_RES_BUFFER = <w>x<h> switches it on, unset keeps the game's.
//
// XC3_RENDER_SCALE sets the share of the game resolution the 3D scene draws in. The game has a scaled-scene path that
// play never uses: the patch set "scene" (recomp/patches-upd.txt) turns it on with both factors from the patch page's
// +0x58 and +0x5c, and the game draws the scene in that part of its full-size targets. The scale is fixed, from 25 to
// 99 %; 100 or more, or "game", leaves the set off and the scene whole. Supersampling is a game resolution above the
// window's.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
// Where each version keeps the table; res_init takes the one that holds the shipped sizes.
static const uint64_t TABLES[] = {0x14dfd8c /* 2.2.1 */, 0x119ec0c /* 1.0.0 */};

static struct {
    int decided;
    unsigned ow, oh;   // output (docked)
    int on;            // res_init wrote the table
    int scale;         // XC3_RENDER_SCALE in percent, 0 for the game's own
} R;

static int parse(const char *s, unsigned *w, unsigned *h) {
    char tail;
    return s && sscanf(s, "%ux%u%c", w, h, &tail) == 2 && *w >= 640 && *h >= 360 && *w <= 7680 && *h <= 4320;
}

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("XC3_OUTPUT_RES");
    unsigned w, h;
    if (parse(o, &w, &h)) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: XC3_OUTPUT_RES=%s is not <width>x<height> in 640x360..7680x4320; 1920x1080", o);
    }
    const char *s = cfg_value("XC3_RENDER_SCALE");
    int pct = s ? atoi(s) : 0;
    if (pct >= 25) {
        R.scale = pct > 100 ? 100 : pct;
    } else if (s && strcmp(s, "game")) {
        rt_log("resolution: XC3_RENDER_SCALE=%s is not game or a percentage from 25; the game's own", s);
    }
}

// Switches the scaled scene on for an XC3_RENDER_SCALE below 100; otherwise the game draws it whole.
static void scene_scale(void) {
    int set = rt_patch_set("scene");
    uint8_t *page = rt_patch_page();
    if (set < 0 || !page) {
        if (R.scale) rt_log("resolution: this build has no 3D render scale; the whole scene");
        R.scale = 0;
        return;
    }
    if (R.scale >= 100) R.scale = 0;
    float f[2] = {R.scale / 100.0f, R.scale / 100.0f};
    memcpy(page + 0x58, f, sizeof f);
    rt_patch_switch(set, R.scale > 0);
    if (R.scale) rt_log("resolution: 3D render scale %d %%", R.scale);
}

// World positions projected for the narrowed interface (patches-upd.txt, uifit) spread by the inverse of res_ui_fit.
static void ui_fit_init(void) {
    int set = rt_patch_set("uifit");
    uint8_t *page = rt_patch_page();
    float k = res_ui_fit(), f = 1 / k;
    if (set < 0 || !page) return;
    memcpy(page + 0x68, &f, sizeof f);
    rt_patch_switch(set, k < 1);
    rt_log("resolution: interface fit %.3f", k);
}

// TotK Recomp grows the texture heap through its HeapDef table; XC3 has no such table, and the 1.0.1 mods run at
// 3840x2160 without a bigger heap.
uint64_t res_heap_extra_kib(void) { return 0; }
uint64_t res_heap_extra_bytes(void) { return 0; }
int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    (void)in, (void)n, (void)out_n;
    *out = NULL;
    return 0;
}
void res_heapdef_served(void) {}

void res_init(void) {
    decide();
    scene_scale();
    rt_patch_switch(rt_patch_set("uifit"), 0);   // ui_fit_init turns it on once the resolution is in place
    uint64_t table_at = 0;
    for (size_t i = 0; i < sizeof TABLES / sizeof *TABLES && !table_at; i++) {
        uint32_t table[4];
        if (plat_mem_read(MOD_MAIN_BASE + TABLES[i], table, sizeof table) && table[0] == BASE_W && table[1] == BASE_H &&
            table[2] == 1280 && table[3] == 720)
            table_at = MOD_MAIN_BASE + TABLES[i];
    }
    if (!table_at) {
        rt_log("resolution: no {1920, 1080, 1280, 720} at a known place in main; the game keeps its own resolution");
        return;
    }
    unsigned bw, bh;
    int resbuf = rt_patch_set("resbuf");
    uint8_t *page = rt_patch_page();
    // patches.c switched the set on for any value but 0; only a size keeps it on.
    if (resbuf >= 0 && page && parse(cfg_get("XC3_RES_BUFFER"), &bw, &bh)) {
        uint32_t v[2] = {bw, bh};
        memcpy(page + 0x50, v, sizeof v);
        rt_patch_switch(resbuf, 1);
        rt_log("resolution: the buffers of main+0x9ccce0 at %ux%u", bw, bh);
    } else if (resbuf >= 0) {
        rt_patch_switch(resbuf, 0);
    }
    uint32_t want[4] = {R.ow, R.oh, R.ow, R.oh};
    uint64_t page_at = table_at & ~0xfffull;
    if (!plat_mem_protect(page_at, 0x1000, PLAT_PROT_RW)) {
        rt_log("resolution: main+%#llx cannot be made writable; the game keeps its own resolution",
               (unsigned long long)(table_at - MOD_MAIN_BASE));
        return;
    }
    memcpy((void *)(uintptr_t)table_at, want, sizeof want);
    plat_mem_protect(page_at, 0x1000, PLAT_PROT_READ);
    R.on = 1;
    rt_log("resolution: %ux%u", R.ow, R.oh);
    ui_fit_init();
}

// The scene's size at the fixed scale, as the game's view loop (main+0xed5b60) cuts it: the frame size times the factor,
// truncated.
void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ow : BASE_W, out[1] = R.on ? R.oh : BASE_H;
    float f = R.scale ? R.scale / 100.0f : 1.0f;
    scene[0] = (unsigned)(out[0] * f), scene[1] = (unsigned)(out[1] * f);
}

// The interface's horizontal scale on a frame wider than the game's 16:9 canvas (nvn_api_cmd.c ui_fit), 1 otherwise.
// XC3_UI_FIT=0 keeps the stretched interface.
float res_ui_fit(void) {
    static float k = -1;
    if (k < 0) {
        const char *s = cfg_get("XC3_UI_FIT");
        float wide = 16.0f / 9 * R.oh / R.ow;
        k = (!R.on || (s && !strcmp(s, "0")) || wide >= 1) ? 1 : wide;
    }
    return k;
}
