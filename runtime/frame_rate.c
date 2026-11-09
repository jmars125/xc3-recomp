// Adapted from MacNativeGames games/totk/runtime/game_patches.c @ ebe621d (MIT, Copyright (c) 2026 MacNativeGames
// contributors): runtime_frame_step_update (2338bb0: no 1/120 s floor) and the speed survey's player chain
// (world_survey.c player_actor).
// Frame rate: the frame limit and the real-time frame step, the host half of the patch set realtime
// (recomp/patches-upd.txt).
//
// XC3 2.2.1 counts its frame interval in 60 Hz vsyncs, an int at main+0x1b42db0 (boot sets 2: 30 fps). Its main loop's
// limiter (main+0x124fa34) waits for that many vsync periods, and its frame-step update (main+0xf3c0) makes 60 / the
// count the frame rate and its reciprocal the step. With the set realtime the step update reads the patch page instead:
// +4 the step in seconds, the moving average of the last 5 display intervals, at most 1/15 s (slower frames slow the
// game down, as on the console), +8 the frame rate it makes, rounded; and the game's limiter returns at once, so the
// host pacer (nvn_api_obj.c present_period) holds frames to the limit (XC3_FPS_LIMIT), 30 to 360 fps or the display's
// rate. Without the set (XC3_REALTIME=0) vsyncs_store writes the count, 1 for a limit of 60 or more, 2 below, and the
// game steps whole vsyncs.
//
// The set menus (XC3_MENU_TIME) scales the interface's per-frame animation step by the patch page's +0 ratio (the
// step in 30 fps frames), so menus and the HUD move at the console's speed at any frame rate. Without the real-time
// step, frame_step_present writes the whole-vsync ratio there instead (vsyncs / 2).
//
// The sample is the display time the pacer gives each present (nvnQueuePresentTexture), so the step follows the
// frames as shown, pacing waits included. One 64-bit and one 32-bit store per present; the guest reads them on its own
// threads, and each stays finite and in range whichever store it sees.
//
// The world clock (frame_step_world_us) sums the steps from the first present that finds the player actor away from
// the origin, where the title screen keeps it: the end of the first save load. Recorded and scripted input
// (input.c) runs on it, so a replay presses at the same game time at any frame rate and after a first load of any
// length; a later load (fast travel) still shifts what follows by its length.
//
// XC3_TRACE=perf adds a line per 120 presents: fps, the game seconds stepped against wall seconds, and with
// XC3_SPEED_PROBE=1 the player's position and ground speed (an independent check: the player runs at the same speed
// at any frame rate when game time follows wall time).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define STEP_SAMPLES 5
#define STEP_MAX_S (1.0 / 15.0)    // longest step; a longer frame slows the game down
#define STEP_RESET_S 0.25          // a gap this long (loading, a debugger) restarts the average
#define STEP_FIRST_S (1.0 / 60.0)

static _Atomic uint64_t *step_word;   // patch page +0: ratio (low float), seconds (high float)
static double step_now = STEP_FIRST_S;   // the step the patch page holds: what the game steps each frame
static _Atomic int32_t *step_fps;   // patch page +8: the frame rate the step makes, rounded (patches-upd.txt realtime)
static float *menu_ratio;   // patch page +0 for the set menus when step_store does not write it
static uint64_t last_ns;
static double samples[STEP_SAMPLES];
static unsigned sample_count, sample_next;

static void step_store(float seconds) {
    step_now = seconds;
    float ratio = seconds * 30.0f;
    uint32_t lo, hi;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    atomic_store_explicit(step_word, (uint64_t)lo | (uint64_t)hi << 32, memory_order_release);
    atomic_store_explicit(step_fps, (int32_t)(1.0f / seconds + 0.5f), memory_order_release);
}

static void frame_limit_live(void);
static int cut_now;   // a cutscene or title menu runs (cutscene below)
static int limit_hz;

// The game's vsync count per frame (main+0x1b42db0; setter main+0x124a528, getter main+0x1249b6c).
static _Atomic uint32_t *vsyncs_word;

static void vsyncs_store(void) {
    uint32_t want = cut_now || (!step_word && limit_hz && limit_hz < 60) ? 2 : 1;
    if (atomic_load_explicit(vsyncs_word, memory_order_relaxed) != want)
        atomic_store_explicit(vsyncs_word, want, memory_order_release);
}

static int vsyncs_find(void) {
    uint32_t v;
    if (!plat_mem_read(MOD_MAIN_BASE + 0x1b42db0, &v, sizeof v) || (v != 0xffffffffu && v != 2)) return 0;
    vsyncs_word = (_Atomic uint32_t *)(uintptr_t)(MOD_MAIN_BASE + 0x1b42db0);
    return 1;
}

void frame_step_init(void) {
    int realtime = rt_patch_set("realtime");
    if (!vsyncs_find()) {
        rt_log("frame rate: the vsync count is not at main+0x1b42db0; shipping 30 fps whole-vsync steps");
        return;
    }
    if (realtime >= 0 && rt_patch_enabled(realtime)) {
        step_word = (_Atomic uint64_t *)rt_patch_page();
        step_fps = (_Atomic int32_t *)((uint8_t *)rt_patch_page() + 8);
        step_store((float)STEP_FIRST_S);
    }
    int menus = rt_patch_set("menus");
    if (!step_word && rt_patch_enabled(menus)) {
        menu_ratio = (float *)rt_patch_page();
        *menu_ratio = 1.0f;
    }
    frame_limit_live();
    rt_log("frame rate: %s", step_word ? "real-time step (5-frame average of display intervals, at most 1/15 s)"
                                       : "whole-vsync steps, vsync count 1 at a limit of 60 or more");
}

int frame_step_realtime(void) { return step_word != NULL; }

// The frame limit (menu, XC3_FPS_LIMIT, live): 30, 60, 120, 144, any whole rate from 30 to 360, or display (the
// refresh rate of the window's monitor). Unset, the older XC3_FPS60=0 still means 30. Without the real-time step the
// limit stops at 60.
#define LIMIT_MIN_HZ 30
#define LIMIT_MAX_HZ 360
static unsigned limit_seen;
static int limit_read;
static int limit_hz = -1;   // 0: display; -1 before the first read (declared above vsyncs_store)

static int limit_parse(void) {
    const char *e = cfg_get("XC3_FPS_LIMIT"), *old = cfg_get("XC3_FPS60");
    if (!e || !*e) return old && !strcmp(old, "0") ? 30 : 60;
    if (!strcmp(e, "display")) return 0;
    char *end;
    long n = strtol(e, &end, 10);
    if (!*end && n >= LIMIT_MIN_HZ && n <= LIMIT_MAX_HZ) return (int)n;
    rt_log("frame rate: XC3_FPS_LIMIT=%s is not 30..360 or display; 60", e);
    return 60;
}

static void frame_limit_live(void) {
    unsigned v = cfg_version();
    if (limit_read && v == limit_seen) return;
    limit_read = 1;
    limit_seen = v;
    int hz = limit_parse();
    if (hz == limit_hz) return;
    limit_hz = hz;
    if ((hz > 60 || hz == 0) && !step_word)
        rt_log("frame rate: limits above 60 need the real-time step, which this build lacks; 60 fps");
    else if (hz) rt_log("frame rate: limit %d fps", hz);
    else if (gfx_window_refresh_hz() > 0) rt_log("frame rate: limit at the monitor's refresh rate (%.3f Hz now)", gfx_window_refresh_hz());
    else rt_log("frame rate: limit at the monitor's refresh rate (60 until the window measures it)");
}

// Shown frames per drawn frame; the Vulkan back end's frame generation (vk_fg.c) overrides this.
__attribute__((weak)) int gfx_frame_gen_factor(void) { return 1; }

uint64_t frame_step_cap_ns(void) {
    frame_limit_live();
    if (cut_now) return (uint64_t)(1e9 / 30 + 0.5);
    double hz = limit_hz ? limit_hz : gfx_window_refresh_hz();
    if (hz < LIMIT_MIN_HZ) hz = 60;   // display rate unknown (no window yet)
    if (hz > LIMIT_MAX_HZ) hz = LIMIT_MAX_HZ;
    // With frame generation running (vk_fg.c) the limit counts the frames shown: a drawn and a generated one per
    // drawn frame, so the game draws half of it.
    hz /= gfx_frame_gen_factor();
    return (uint64_t)(1e9 / hz + 0.5);
}

// The frame rate the limit gives, for the performance overlay: the setting, the monitor's rate for "display", 60 at
// most without the real-time step; 0 before the first frame read the setting.
double frame_step_limit_hz(void) {
    int hz = limit_hz;
    if (hz < 0) return 0;
    if (cut_now) return 30;
    double r = hz ? hz : gfx_window_refresh_hz();
    if (r < LIMIT_MIN_HZ) r = 60;
    return !step_word && r > 60 ? 60 : r > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : r;
}

// ---- XC3_TRACE=perf speed lines
static uint64_t win_t0, win_frames, win_hist[4];   // win_hist: frames by present interval 1, 2, 3+ ([0] unused)
static double win_game;
static int probe = -1;
static float last_pos[3];
static uint64_t last_pos_ns;

static int read_guest(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t read_ptr(uint64_t a) {
    uint64_t p = 0;
    return read_guest(a, &p, 8) ? p : 0;
}
// 1.2.1: [main+0x4728538] (1.4.3: [main+0x3add238]) +0x1e8 scene, its component 13 lists the resident actors; the one
// named "Player" has its position at +0x2b4 (MacNativeGames world_survey.c, game_version.h XC3_GAME_V121/V143).
static int player_pos(float xyz[3]) {
    uint64_t module = read_ptr(MOD_MAIN_BASE + (RT_V143 ? 0x3add238 : 0x4728538)), scene = module ? read_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !read_guest(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t comps = read_ptr(scene + 0x58), resident = comps ? read_ptr(comps + 13 * 8) : 0;
    if (!resident || !read_guest(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = read_ptr(resident + 0x28);
    for (int i = 0; links && i < count; i++) {
        uint64_t data = read_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? read_ptr(data + 0x40) : 0;
        uint64_t name = actor ? read_ptr(actor + 0x218) : 0;
        char text[7];
        if (name && read_guest(name, text, 7) && !memcmp(text, "Player", 7))
            return read_guest(actor + 0x2b4, xyz, 12) && isfinite(xyz[0]) && isfinite(xyz[2]);
    }
    return 0;
}

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    if (probe < 0) {
        const char *e = cfg_get("XC3_SPEED_PROBE");
        probe = e && !strcmp(e, "1");
    }
    double wall = (double)(now - win_t0) * 1e-9;
    char pos[160] = "";
    float p[3];
    if (probe && player_pos(p)) {
        double d = last_pos_ns ? hypot(p[0] - last_pos[0], p[2] - last_pos[2]) : 0,
               s = last_pos_ns ? (double)(now - last_pos_ns) * 1e-9 : 0;
        snprintf(pos, sizeof pos, "; player %.1f %.1f %.1f, %.2f m in %.3f s = %.2f m/s", p[0], p[1], p[2], d, s,
                 s > 0 ? d / s : 0);
        memcpy(last_pos, p, sizeof p);
        last_pos_ns = now;
    }
    if (step_word)
        rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
               (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    else
        rt_log("fps: %llu frames in %.3f s (%.1f fps), whole-vsync steps; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, (unsigned long long)win_hist[1],
               (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    win_t0 = now;
    win_frames = 0;
    win_game = 0;
    memset(win_hist, 0, sizeof win_hist);
}

static _Atomic int64_t world_us = -1;
static _Atomic uint64_t game_us;   // game time stepped since the first present (frame_step_ticks)
static uint64_t presents;

int64_t frame_step_world_us(void) { return atomic_load_explicit(&world_us, memory_order_relaxed); }
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&game_us, memory_order_relaxed) * 60 / 1000000; }

static void world_clock(double dt, double step) {
    presents++;
    int64_t w = atomic_load_explicit(&world_us, memory_order_relaxed);
    if (w < 0) {
        // XC3: XC3's player lookup (player_pos) reads XC3's scene, so the clock starts at the first present instead.
        rt_log("frame step: world clock starts at present %llu, %.2f s after the guest started",
               (unsigned long long)presents, rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0);
        atomic_store_explicit(&world_us, 0, memory_order_relaxed);
        return;
    }
    if (!step_word) step = dt > STEP_MAX_S ? STEP_MAX_S : dt;   // whole-vsync steps: the display time stands in
    atomic_store_explicit(&world_us, w + (int64_t)(step * 1e6 + 0.5), memory_order_relaxed);
}

// Cutscenes and the title menus run at 30 fps, as on the console: their animation advances per frame, so at 120 fps a
// cutscene ran ahead of its voices. The step update's object (main+0xf3c0, its address stored to the patch page's +0x70
// by the realtime site at main+0xf3dc) has bit 2 of +0x1fc set while the game takes a fixed step instead of the
// measured one: the title menus and every cutscene, clear in play. While it is set the step is 1/30 s, the frame rate
// 30, the vsync count 2 and the pacer holds 30 fps. XC3_CUTSCENE_30=0 keeps the limit.
static int cut_on = -1;

static int cutscene(void) {
    if (cut_on < 0) cut_on = !(cfg_get("XC3_CUTSCENE_30") && !cfg_bool("XC3_CUTSCENE_30"));
    if (!cut_on || !step_word) return 0;
    uint64_t obj = 0;
    uint32_t flags = 0;
    memcpy(&obj, (uint8_t *)rt_patch_page() + 0x70, 8);
    if (obj && plat_mem_read(obj + 0x1fc, &flags, 4) && (flags & 4)) return 1;
    // A story event playing: the play report's object (*[*main+0x1b4d688], read in main+0x325708, as Lucky7's
    // runtime/hle/xc2.c xc3_event_tour reads it) holds the event's id at +0x1e0 until the event ends. Events between
    // cutscenes (fades, staged moves) step without the fixed-step flag; at 120 fps the end of event 10005 waited for
    // minutes and went on at once at 30.
    uint64_t rep = 0;
    uint32_t id = 0;
    return plat_mem_read(MOD_MAIN_BASE + 0x1b4d688, &rep, 8) && rep && plat_mem_read(rep, &rep, 8) && rep &&
           plat_mem_read(rep + 0x1e0, &id, 4) && id;
}

void frame_step_present(uint64_t display_ns, int interval) {
    frame_limit_live();
    int cut = cutscene();
    if (cut != cut_now) {
        cut_now = cut;
        sample_count = sample_next = 0;   // the average starts again at the new rate
        rt_log("frame rate: %s", cut ? "cutscene, story event or title menu, 30 fps" : "play, the limit again");
    }
    if (vsyncs_word) vsyncs_store();
    double step = 0, dt = last_ns ? (double)(display_ns - last_ns) * 1e-9 : 0;
    if (cut_now) {
        step_store(1.0f / 30.0f);
    } else if (step_word && last_ns) {
        if (dt > STEP_RESET_S || dt < 0.001) {
            sample_count = sample_next = 0;
        } else {
            if (dt > STEP_MAX_S) dt = STEP_MAX_S;
            samples[sample_next++ % STEP_SAMPLES] = dt;
            if (sample_count < STEP_SAMPLES) sample_count++;
            double sum = 0;
            for (unsigned i = 0; i < sample_count; i++) sum += samples[i];
            step = sum / sample_count;
            step_store((float)step);
        }
    }
    if (menu_ratio) {
        float r = (interval > 0 ? interval : 1) * 0.5f;
        uint32_t w;
        memcpy(&w, &r, 4);
        __atomic_store_n((uint32_t *)menu_ratio, w, __ATOMIC_RELAXED);
    }
    last_ns = display_ns;
    // The game clock of untimed script lines: what the game stepped this frame, the patch page's step with the
    // real-time step (kept through a reset), whole vsyncs without it.
    double game = step_word ? step_now : (interval > 0 ? interval : 1) / 60.0;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, step);
    if (rt_trace & RT_TRACE_PERF) speed_report(display_ns, step, interval);
}
