// Dynamic resolution from the GPU's own timestamps: the 3D scene's size follows how long the GPU takes per frame, so a
// heavy view draws fewer pixels and keeps the frame time, and a light one goes back to the player's render scale. It
// moves in the steps DLSS upscales from: DLAA, quality, balanced, performance and ultra performance, 100, 67, 58, 50
// and 33 % of the game resolution on each axis (vk_sl.cpp vk_sl_evaluate picks the DLSS mode by that share; FSR 3
// takes any size).
//
// XC3_DYN_RES=1 turns it on (off by default: it changes the picture's sharpness; live). It acts only on the scaled
// scene (resolution.c: render_scale below 100, or upscaler = dlaa) with a temporal upscaler, whose render scale is then
// the sharpest step it uses; the lowest is performance, or ultra performance when the render scale is 50 or less.
// While it runs it asks for GPU timestamps (perf_gpu_timing_want; vk_sync.c times every submission) and reads their
// running total (perf_gpu_busy_total) once per drawn frame. It averages the busy time and moves one step at a time:
// down when the average is over the target by 8 %, up when it is under 75 % of it, and holds each step for
// XC3_DYN_RES_STEP frames (default 45), since a size that changes every few frames is worse to look at than a steady one.
// The target is XC3_DYN_RES_TARGET milliseconds, or with auto (the default) the frame time the frame limit gives a
// drawn frame (frame_rate.c frame_step_cap_ns: half the shown rate under frame generation). The step goes to the
// game through resolution.c res_scene_live, which writes the factors the game reads at every frame's setup; the
// upscaler's images keep their largest size (vk_tup.c ensure_images), so a step costs no reallocation and no history.
//
// Ported from NX2navi (runtime/dynres.c, dynres_frame; MIT, Copyright 2026 Valiantsin Dzerakh), which steps NGX's DLSS
// modes; here the steps are render scales.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

#define LEVELS_MAX 5
static const int STEP_PCT[] = {100, 67, 58, 50, 33};

static struct {
    int on;               // dynamic resolution acts now
    int levels[LEVELS_MAX], n;   // the scales it uses, sharpest first: the player's, then the DLSS steps below it
    int level;            // the step in use (index into levels)
    int settle;           // frames the step has been held
    int step;             // frames to hold a step
    double avg_ns;        // moving average of the GPU's busy time per drawn frame
    double target_ms;     // XC3_DYN_RES_TARGET; 0 auto
    uint64_t last_busy;   // perf_gpu_busy_total at the previous frame
    unsigned cfg_seen;    // cfg_version the settings were read at
} D = {.cfg_seen = ~0u};

static int temporal_upscaler(void) {
    const char *u = cfg_value("XC3_UPSCALER");
    return u && (!strcmp(u, "fsr3") || !strcmp(u, "dlss") || !strcmp(u, "dlaa"));
}

// The settings, again whenever one changed (the menu switches them live).
static void dynres_read(void) {
    D.cfg_seen = cfg_version();
    int base = res_scene_base();
    int want = cfg_bool("XC3_DYN_RES");
    const char *t = cfg_value("XC3_DYN_RES_TARGET");
    D.target_ms = t && strcmp(t, "auto") ? atof(t) : 0;
    if (D.target_ms < 1) D.target_ms = 0;
    D.step = cfg_int("XC3_DYN_RES_STEP");
    if (D.step < 8) D.step = 8;
    int on = want && base > 0 && temporal_upscaler();
    if (on && !D.on) {   // start at the player's scale
        D.n = 0;
        D.levels[D.n++] = base;
        int low = base > 50 ? 50 : 33;
        for (size_t i = 0; i < sizeof STEP_PCT / sizeof *STEP_PCT && D.n < LEVELS_MAX; i++)
            if (STEP_PCT[i] < base && STEP_PCT[i] >= low) D.levels[D.n++] = STEP_PCT[i];
        D.level = 0, D.settle = 0, D.avg_ns = 0, D.last_busy = perf_gpu_busy_total();
        if (D.target_ms) rt_log("resolution: dynamic resolution on: target %.1f ms, %d %% down to %d %%", D.target_ms, base,
                                D.levels[D.n - 1]);
        else rt_log("resolution: dynamic resolution on: target the frame limit's, %d %% down to %d %%", base, D.levels[D.n - 1]);
    } else if (!on && D.on) {
        res_scene_live(base);
        rt_log("resolution: dynamic resolution off: %d %% as set", base);
    } else if (want && !on) {
        static int said;
        if (!said++) rt_log("resolution: dynamic resolution needs render_scale below 100 (or upscaler = dlaa) and a "
                            "temporal upscaler (fsr3, dlss or dlaa)");
    }
    D.on = on;
    perf_gpu_timing_want(on);
}

int dynres_scale(void) { return D.on ? D.levels[D.level] : 0; }

// Once per drawn frame (vk_backend.c present_image).
void dynres_frame(void) {
    if (cfg_version() != D.cfg_seen) dynres_read();
    if (!D.on) return;
    uint64_t now = perf_gpu_busy_total(), d = now - D.last_busy;
    D.last_busy = now;
    if (!d || d > 1000000000ull) return;   // no timing this frame yet, or a stall that says nothing about the scene
    D.avg_ns = D.avg_ns > 0 ? D.avg_ns * 0.9 + (double)d * 0.1 : (double)d;
    double target_ns = D.target_ms ? D.target_ms * 1e6 : (double)frame_step_cap_ns();
    static int frames, said;
    if (++frames % 600 == 0 && said < 12) {
        said++;
        rt_log("resolution: dynamic resolution: GPU %.2f ms a frame, %d %%, target %.2f ms", D.avg_ns / 1e6,
               D.levels[D.level], target_ns / 1e6);
    }
    if (++D.settle < D.step) return;
    int to = D.level;
    if (D.avg_ns > target_ns * 1.08 && D.level < D.n - 1) to = D.level + 1;
    else if (D.avg_ns < target_ns * 0.75 && D.level > 0) to = D.level - 1;
    if (to == D.level) return;
    rt_log("resolution: dynamic resolution: GPU %.2f ms a frame against %.2f ms: %d %% -> %d %%", D.avg_ns / 1e6,
           target_ns / 1e6, D.levels[D.level], D.levels[to]);
    D.level = to, D.settle = 0;
    res_scene_live(D.levels[to]);
}
