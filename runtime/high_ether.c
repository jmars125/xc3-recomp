// High Ether orbs (patch set highether, XC3_HIGH_ETHER; recomp/patches-upd.txt explains the sphere's state machine
// and its list): the orbs a sphere sends while it is harvested leave at the times they left at 30 fps, so no more of
// them fly at once at a higher frame rate, and a push to a full list ends its newest orb in place of writing past it.
#include <math.h>
#include <stdint.h>

#include "cpu.h"
#include "dispatch.h"
#include "host.h"
#include "layout.h"

#define WAIT_HOOK 0xf30    // patch page offsets (patches-upd.txt, @call:f30, f40 and f50)
#define START_HOOK 0xf40
#define PUSH_HOOK 0xf50
#define SLOTS 8
#define FRAME (1.0f / 30.0f)   // the frame of the game's own 30 fps

static uint64_t most, full;

// State 6 waits while the parameter in s0 is at least the timer. At 30 fps it leaves on the first frame whose timer
// passes the parameter, so s0 becomes that frame's time, a little less to absorb the sum of the steps.
static void ether_high_wait(Cpu *c) {
    float p = arg_s(c, 0);
    if (p >= 0.0f && p < 60.0f) ret_s(c, (floorf(p * 30.0f) + 1.0f) * FRAME - 1e-4f);
}

// State 5 starts state 6's wait; its timer (s0) holds this frame's step, the second since state 3 sent the orb. At 30
// fps those two frames took 2/30 s, so the wait starts that much less what they took now. Longer frames keep the 0.
static void ether_high_start(Cpu *c) {
    float dt = arg_s(c, 0), t = 2.0f * dt - 2.0f * FRAME;
    ret_s(c, dt > 0.0f && t < 0.0f ? t : 0.0f);
}

#define RELEASE_HANDLE 0x32e118   // main: releases the effect handle at x0 (the list update's release of an ended orb)
#define RELEASE_ID 0x4b92d0       // main: releases the effect id w0 (the same; the update then stores -1)

// x8 holds the count the push read from the list (+0x180 of x19); it becomes the slot, min(count, 7). Only a full
// list's newest slot is released first, as the list update releases an orb that ended: the slots past the count are
// not cleared (a sphere's first push found a stale handle there and crashed in main+0x32e118 above 120 fps).
static void ether_high_push(Cpu *c) {
    uint64_t n = c->x[8];
    uint64_t prev = __atomic_load_n(&most, __ATOMIC_RELAXED);
    while (n + 1 > prev && !__atomic_compare_exchange_n(&most, &prev, n + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (n + 1 > prev && n + 1 <= SLOTS) rt_log("high ether: %llu orbs at once (the most so far)", (unsigned long long)n + 1);
    if (n >= SLOTS) {
        uint64_t k = __atomic_add_fetch(&full, 1, __ATOMIC_RELAXED);
        if (k <= 16 || !(k & (k - 1)))
            rt_log("high ether: the list at %#llx held %llu, its newest orb gave way to a new one (%llu times)",
                   (unsigned long long)c->x[19], (unsigned long long)n, (unsigned long long)k);
        n = SLOTS - 1;
        uint64_t slot = c->x[19] + n * 0x30;
        rt_call(c, MOD_MAIN_BASE + RELEASE_HANDLE, 1, (uint64_t)slot);
        rt_call(c, MOD_MAIN_BASE + RELEASE_HANDLE, 1, (uint64_t)(slot + 8));
        uint32_t id = LD32(slot + 0x10);
        if (id != 0xffffffffu) {
            rt_call(c, MOD_MAIN_BASE + RELEASE_ID, 1, (uint64_t)id);
            ST32(slot + 0x10, 0xffffffffu);
        }
    }
    c->x[8] = n;
}

void high_ether_init(void) {
    if (!rt_patch_enabled(rt_patch_set("highether"))) return;
    uint64_t page = (uint64_t)(uintptr_t)rt_patch_page();
    rt_register_host(page + WAIT_HOOK, ether_high_wait);
    rt_register_host(page + START_HOOK, ether_high_start);
    rt_register_host(page + PUSH_HOOK, ether_high_push);
}
