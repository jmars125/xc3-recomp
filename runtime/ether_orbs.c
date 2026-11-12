// Ether orbs of an Ether Channel (patch set orbs, XC3_ETHER_ORBS; recomp/patches-upd.txt explains the list): the
// first site of the set loads the list's count into x8 and calls orbs_push. It picks the slot the new orb goes to,
// min(count, 7), and logs what the list held, so a log shows how close a harvest came to the 8 slots: each new highest
// count once, and each time the list was full and its newest orb gave way (issue 30 crashed there before the cap).
#include <stdint.h>

#include "dispatch.h"
#include "host.h"

#define ORBS_HOOK 0xf10   // patch page offset of orbs_push (patches-upd.txt, @call:f10)
#define ORBS_SLOTS 8

static uint64_t most, full;

static void orbs_push(Cpu *c) {
    uint64_t n = c->x[8];   // the count, read from list +0x190 by the site
    uint64_t prev = __atomic_load_n(&most, __ATOMIC_RELAXED);
    while (n + 1 > prev && !__atomic_compare_exchange_n(&most, &prev, n + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (n + 1 > prev && n + 1 <= ORBS_SLOTS) rt_log("ether orbs: %llu at once (the most so far)", (unsigned long long)n + 1);
    if (n >= ORBS_SLOTS) {
        uint64_t k = __atomic_add_fetch(&full, 1, __ATOMIC_RELAXED);
        if (k <= 16 || !(k & (k - 1)))
            rt_log("ether orbs: the list at %#llx held %llu, its newest orb gave way to a new one (%llu times)",
                   (unsigned long long)c->x[19], (unsigned long long)n, (unsigned long long)k);
        n = ORBS_SLOTS - 1;
    }
    c->x[8] = n;
}

void ether_orbs_init(void) {
    if (!rt_patch_enabled(rt_patch_set("orbs"))) return;
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + ORBS_HOOK, orbs_push);
}
