// Dynamic resolution (XC3_DYNAMIC_RES) through the game's own.
//
// XC3 lowers the size its scene draws at by itself (960x544 at 1080p down to 672x384 in steps; the game then draws its
// targets at that size and our upscalers follow it, vk_tup.c vk_tup_record) when its GPU time runs over its budget. It
// reads that time from timestamp counter reports it makes four of per frame (vk_exec_xfer.c report_counter, at
// +0x00, +0x20, +0x40 and +0x60 of its report buffer) and measures between the first two and the last two. Our reports
// are written when the frame's fence signals, so both of a pair carry the same time: the game sees no GPU time and
// keeps the whole size.
//
// Here the second report of the first pair is written later by the GPU time we want the game to see: the real time the
// GPU was busy for the frame (vk_sync.c, timestamps around each submission), scaled from our budget (the time a drawn
// frame has at the frame limit, frame_rate.c frame_step_cap_ns) to the game's (GAME_BUDGET_NS). Over our budget the game
// lowers the scene step by step (to 70 % per side), back under it raises it again, with its own steps and hysteresis.
#include <stdint.h>

#include "cfg.h"
#include "host.h"

// The game's budget: its 60 fps frame. With XC3_GPU_TIME_TEST the scene stayed whole at 12 ms per frame and went
// down at 20 ms. HEADROOM: the share of our budget the GPU may fill before the game is told it is over.
#define GAME_BUDGET_NS (1e9 / 60)
#define HEADROOM 0.9

static struct {
    unsigned cfg_seen;
    int on;
    uint64_t last_total;   // perf_gpu_busy_total at the last frame
    double ema_ns;         // GPU busy time per frame, smoothed over about 8 frames
} D = {.cfg_seen = ~0u};

int dynres_on(void) {
    unsigned v = cfg_version();
    if (v != D.cfg_seen) {
        D.cfg_seen = v;
        int on = cfg_bool("XC3_DYNAMIC_RES");
        if (on != D.on) rt_log("dynamic resolution %s", on ? "on: the game's own, from the GPU's time per frame" : "off");
        D.on = on;
    }
    return D.on;
}

// The completion thread, at the frame's first-pair report: the GPU time the game is to read for this frame, in ns.
uint64_t dynres_report_ns(void) {
    uint64_t total = perf_gpu_busy_total();
    double frame = D.last_total && total >= D.last_total ? (double)(total - D.last_total) : 0;
    D.last_total = total;
    if (frame > 0 && frame < 1e9) D.ema_ns = D.ema_ns ? D.ema_ns + (frame - D.ema_ns) / 8 : frame;
    double budget = (double)frame_step_cap_ns();
    if (budget < 1e6) return 0;
    double show = D.ema_ns / (budget * HEADROOM) * GAME_BUDGET_NS;
    return show > 2e8 ? 200000000u : (uint64_t)show;
}
