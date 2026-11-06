// Adapted from Lucky7 (lucky7-xc3) runtime/hle/xc2.c xc3_tour and xc3_world_shown @ 8b596bc (MIT, Copyright (c) 2026
// Peiikai, Copyright (c) 2026 XCDE Recomp contributors): the Skip Travel request, the world state checks and the
// tour's phases. The event and fight tours are left out.
//
// Map-jump tour, a test tool: XC3_TOUR=<list from tools/tour_list.py> jumps the party to each landmark in turn, as the
// game's Skip Travel does, so a run reaches the field of every region without anyone playing. Per line: the jump, the
// load (XC3_TOUR_WAIT s at least, default 25, then until the world is shown, 3 minutes at most), a capture
// (<XC3_TOUR_OUT>/tour_<jump id>.png, default folder tour), then the right stick held for 8 s while the frame rate of
// that turn is logged ("tour: ... fps while turning"). The first jump waits until XC3_TOUR_START s after the first
// tick (default 60) and for the world with no story event playing: load a save past the opening first (Continue with
// tools/input/load_save.txt). XC3_TOUR_FROM and XC3_TOUR_COUNT pick lines. The game exits after the last line.
//
// The tick: the set tour (recomp/patches-upd.txt) calls tour_tick from the world tick (main+0xf3c0), once a frame on
// the main thread, with the tick's object in x0. A landmark's Skip Travel builds a request and calls
// main+0xd65f00(world, request), world = that object + 0x208: +0 1, +4 the map, +0x30 the formation's hash
// (SYS_MapJumpList), +0x58 1.0, +0x74 0x204, +0x90 and +0x94 0.5. main+0xe89538(&world) returns the world's state:
// +0xea non-zero on the title and during a load, the word at +0xe8 has its low bits 3 once the world is there. The
// word at main+0x1ba61fc is 0 from a jump until the world is on screen. The request and the world pointer live on
// the patch page (+0x800, +0x7f8), in guest memory. A story event runs while the play report's object
// (*[main+0x1b4d688], read in main+0x325708) holds its id at +0x1e0: the tour does not jump then. A jump during the
// opening's story chain did nothing, and the next one crashed the renderer, so the tour starts from a save past it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define TOUR_HOOK 0xf00         // patch page offset of tour_tick (patches-upd.txt, @call:f00)
#define TOUR_WORLD 0x7f8        // patch page: the world pointer main+0xe89538 reads
#define TOUR_REQ 0x800          // patch page: the jump request, 0x180 bytes
#define FN_JUMP 0xd65f00ull     // main offsets
#define FN_STATE 0xe89538ull
#define WORLD_SHOWN 0x1ba61fcull
#define PLAY_REPORT 0x1b4d688ull
#define TOUR_MAX 2048
#define TURN_NS 8000000000ull
#define LOAD_MAX_NS 180000000000ull
#define TURN_RX 24000

static uint32_t ids[TOUR_MAX], maps[TOUR_MAX], hashes[TOUR_MAX], n, idx, end_idx;
static int phase;
static uint64_t t0, f0, wait_ns, start_ns;
static char out_dir[512];

static uint64_t env_u(const char *name, uint64_t def) {
    const char *v = cfg_get(name);
    return v && *v ? strtoull(v, NULL, 10) : def;
}

static uint64_t frames(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

// The world's state block, or NULL before the world exists.
static const uint8_t *world_state(Cpu *c, uint64_t self) {
    uint8_t *page = rt_patch_page();
    uint64_t world = self + 0x208;
    memcpy(page + TOUR_WORLD, &world, 8);
    return (const uint8_t *)(uintptr_t)rt_call(c, MOD_MAIN_BASE + FN_STATE, 1, (uint64_t)(uintptr_t)(page + TOUR_WORLD));
}

// The id of the story event playing, 0 for none.
static uint32_t event_playing(void) {
    uint64_t rep = *(volatile uint64_t *)(uintptr_t)(MOD_MAIN_BASE + PLAY_REPORT);
    rep = rep ? *(volatile uint64_t *)(uintptr_t)rep : 0;
    return rep ? *(volatile uint32_t *)(uintptr_t)(rep + 0x1e0) : 0;
}

// On the title, in a load, before the world is drawn, or in a story event.
static int world_busy(const uint8_t *st) {
    if (!st) return 1;
    uint32_t word;
    memcpy(&word, st + 0xe8, 4);
    return st[0xea] || (word & 3) != 3 || !*(volatile uint32_t *)(uintptr_t)(MOD_MAIN_BASE + WORLD_SHOWN) ||
           event_playing();
}

static void state_log(const char *what, const uint8_t *st) {
    uint32_t word = 0;
    if (st) memcpy(&word, st + 0xe8, 4);
    rt_log("tour: %s: state %08x %02x, shown %u, event %u", what, word, st ? st[0xea] : 0xff,
           *(volatile uint32_t *)(uintptr_t)(MOD_MAIN_BASE + WORLD_SHOWN), event_playing());
}

static void tour_tick(Cpu *c) {
    if (!n) return;
    uint64_t self = c->x[0], now = plat_now_ns(), frame = frames();
    if (phase == 0) {
        if (now - t0 < start_ns) return;
        phase = 1;
    }
    if (phase == 1) {
        if (idx >= end_idx) {
            input_tour(0, 0);
            rt_log("tour: done, %u jumps", end_idx);
            n = 0;
            gfx_window_closed();   // saves the pipeline cache and the profile, then exits
            return;
        }
        const uint8_t *st = world_state(c, self);
        static uint64_t waited;
        if (world_busy(st)) {
            if (now - waited >= 30000000000ull) waited = now, state_log("waiting to jump", st);
            return;
        }
        uint8_t *req = (uint8_t *)rt_patch_page() + TOUR_REQ;
        uint32_t w[0x60] = {0};
        w[0] = 1, w[1] = maps[idx], w[0x30 / 4] = hashes[idx], w[0x58 / 4] = 0x3f800000, w[0x74 / 4] = 0x204;
        w[0x90 / 4] = w[0x94 / 4] = 0x3f000000;
        memcpy(req, w, sizeof w);
        rt_log("tour: line %u, jump %u to map %u at frame %llu", idx, ids[idx], maps[idx], (unsigned long long)frame);
        rt_call(c, MOD_MAIN_BASE + FN_JUMP, 2, self + 0x208, (uint64_t)(uintptr_t)req);
        t0 = now, phase = 2;
    } else if (phase == 2) {
        const uint8_t *st = world_state(c, self);
        int busy = world_busy(st);
        if (now - t0 < wait_ns || (busy && now - t0 < LOAD_MAX_NS)) return;
        if (busy) state_log("still busy after 3 minutes", st);
        rt_log("tour: line %u, loaded after %llu s%s", idx, (unsigned long long)((now - t0) / 1000000000ull),
               busy ? " (still busy at the limit)" : "");
        char path[600];
        snprintf(path, sizeof path, "%s/tour_%u.png", out_dir, ids[idx]);
        gfx_shot_request(path);
        input_tour(0, TURN_RX);
        t0 = now, f0 = frame, phase = 3;
    } else if (phase == 3) {
        if (now - t0 < TURN_NS) return;
        input_tour(0, 0);
        rt_log("tour: line %u, jump %u: %.1f fps while turning", idx, ids[idx],
               (double)(frame - f0) * 1e9 / (double)(now - t0));
        idx++, phase = 1;
    }
}

void tour_init(void) {
    const char *list = cfg_get("XC3_TOUR");
    if (!list || !*list || !strcmp(list, "0") || !rt_patch_enabled(rt_patch_set("tour"))) return;
    FILE *f = fopen(list, "r");
    if (!f) {
        rt_log("tour: cannot open %s", list);
        return;
    }
    while (n < TOUR_MAX && fscanf(f, "%u %u %x", &ids[n], &maps[n], &hashes[n]) == 3) n++;
    fclose(f);
    if (!n) {
        rt_log("tour: %s has no lines", list);
        return;
    }
    idx = (uint32_t)env_u("XC3_TOUR_FROM", 0);
    end_idx = cfg_get("XC3_TOUR_COUNT") ? idx + (uint32_t)env_u("XC3_TOUR_COUNT", n) : n;
    if (end_idx > n) end_idx = n;
    wait_ns = env_u("XC3_TOUR_WAIT", 25) * 1000000000ull;
    start_ns = env_u("XC3_TOUR_START", 60) * 1000000000ull;
    const char *o = cfg_get("XC3_TOUR_OUT");
    snprintf(out_dir, sizeof out_dir, "%s", o && *o ? o : "tour");
    plat_file_mkdir_p(out_dir);
    t0 = plat_now_ns();
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + TOUR_HOOK, tour_tick);
    rt_log("tour: %u destinations from %s, lines %u..%u, captures in %s", n, list, idx, end_idx - 1, out_dir);
}
