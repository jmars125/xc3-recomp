// Loose mod files in place of archive entries (patch set modfiles, XC3_MOD_ARCHIVE; recomp/patches-upd.txt explains
// the site). Almost every game file is an entry of an arh1/ard archive pair: bf3.arh in the RomFS and bf3_dlc0<n>.arh
// in each add-on's folder. Emulator mods ship the replacements as loose files under romfs/ and rely on the Skyline
// plugin xc3-file-loader, which makes the game's archive lookup miss for those paths; the game then reads the loose
// file through nn::fs, with the file's own size. romfs.c already adds the mods' files to the RomFS; the patch site
// calls mod_archive_hook on every lookup, which makes it miss for an added path the same way.
//
// mod_archive_init reads the arh1 indexes once at the start only to report what the mods replace (the hook does not
// need them). arh1 layout (tools/tour_list.py documents the same): after the magic, u32 string table length, node
// count, string table offset and padded length, node table offset and length, file table offset, file count, key. The
// string and node tables are XORed u32 by u32 with key ^ 0xF3F35353. The node table is a double-array trie of
// (next, prev) i32 pairs, as the game walks it in ml::DevFileArchiver::getFileInfo (main+0x1257798): from node i the
// child for byte c is next ^ c when its prev is i; next < 0 is a leaf whose rest of the path is a NUL-terminated
// string at string offset -next, followed by the u32 file index. File entries (0x18 bytes): offset u64, stored size,
// uncompressed size, flags (2: an xbc1 block), id.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dispatch.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

#define MODFILES_HOOK 0xf20   // patch page offset of mod_archive_hook (patches-upd.txt, @call:f20)
#define AOC_MAX 256           // add-on folders looked at, as fs_host.c's AOC_MAX

static int set = -1;   // the modfiles patch set, -1 when this build's code has none
static int ready;      // mod_archive_init ran: the RomFS's added files are final

// x2 holds the path the game looks up ("/bdat/gb/..."), x9 the archive list's length in bytes; 0 makes the lookup
// return "not found" for this media.
static void mod_archive_hook(Cpu *c) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return;
    const char *p = arg_p(c, 2);
    if (!p) return;
    if (*p == '/') p++;
    if (romfs_added_has(p)) c->x[9] = 0;
}

void mod_archive_boot(void) {
    set = rt_patch_set("modfiles");
    if (!rt_patch_enabled(set)) return;
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + MODFILES_HOOK, mod_archive_hook);
}

// ---- arh1 indexes, for the report
typedef struct {
    char label[64];
    uint8_t *b;
    uint32_t str_off, str_len, node_off, node_count, file_off, file_count;
    size_t replaced;
} Arh;

static uint32_t u32_at(const uint8_t *b, size_t o) {
    uint32_t v;
    memcpy(&v, b + o, 4);
    return v;
}

// Checks the header against the size and decrypts the string and node tables in place; 0 if b is no arh1.
static int arh_open(Arh *a, uint8_t *b, uint64_t n) {
    if (n < 0x28 || memcmp(b, "arh1", 4)) return 0;
    uint32_t str_off = u32_at(b, 12), str_len = u32_at(b, 16), node_off = u32_at(b, 20), node_len = u32_at(b, 24),
             file_off = u32_at(b, 28), file_count = u32_at(b, 32), key = u32_at(b, 36) ^ 0xF3F35353u;
    if ((uint64_t)str_off + str_len > n || (uint64_t)node_off + node_len > n ||
        (uint64_t)file_off + (uint64_t)file_count * 0x18 > n || node_len < 8)
        return 0;
    uint32_t offs[2] = {str_off, node_off}, lens[2] = {str_len & ~3u, node_len & ~7u};
    for (int t = 0; t < 2; t++)
        for (uint32_t i = 0; i < lens[t]; i += 4) {
            uint32_t v = u32_at(b, offs[t] + i) ^ key;
            memcpy(b + offs[t] + i, &v, 4);
        }
    a->b = b;
    a->str_off = str_off;
    a->str_len = str_len;
    a->node_off = node_off;
    a->node_count = node_len / 8;
    a->file_off = file_off;
    a->file_count = file_count;
    return 1;
}

static void node(const Arh *a, uint32_t i, int32_t *next, int32_t *prev) {
    memcpy(next, a->b + a->node_off + 8 * (size_t)i, 4);
    memcpy(prev, a->b + a->node_off + 8 * (size_t)i + 4, 4);
}

// The file index of a leaf whose rest of the path must be rest, or -1.
static int64_t arh_leaf(const Arh *a, int32_t next, const char *rest) {
    uint64_t o = (uint64_t)(-(int64_t)next);
    if (o >= a->str_len) return -1;
    const char *s = (const char *)a->b + a->str_off + o;
    const char *e = memchr(s, 0, a->str_len - o);
    if (!e || strcmp(s, rest) || (uint64_t)(e + 5 - s) > a->str_len - o) return -1;
    uint32_t id = u32_at((const uint8_t *)e, 1);
    return id < a->file_count ? id : -1;
}

// The file index of path ("/bdat/..."), or -1.
static int64_t arh_find(const Arh *a, const char *path) {
    uint32_t cur = 0;
    for (size_t i = 0;; i++) {
        int32_t next, prev;
        node(a, cur, &next, &prev);
        if (next < 0) return arh_leaf(a, next, path + i);
        unsigned char ch = (unsigned char)path[i];
        uint32_t child = (uint32_t)next ^ ch;
        if (child >= a->node_count) return -1;
        node(a, child, &next, &prev);
        if (prev != (int32_t)cur) return -1;
        if (!ch) return next < 0 ? arh_leaf(a, next, "") : -1;
        cur = child;
    }
}

static Arh *arhs;
static size_t narh;
static size_t unreadable;

static void arh_add(const char *label, uint8_t *b, uint64_t n) {
    if (!b) {
        rt_log("mods: cannot read %s; its entries are left out of the count", label);
        unreadable++;
        return;
    }
    Arh a = {0};
    if (!arh_open(&a, b, n)) {
        rt_log("mods: %s is not an arh1 index; its entries are left out of the count", label);
        free(b);
        unreadable++;
        return;
    }
    snprintf(a.label, sizeof a.label, "%s", label);
    Arh *grown = realloc(arhs, (narh + 1) * sizeof *arhs);
    if (!grown) rt_fatal("mods: archive table");
    arhs = grown;
    arhs[narh++] = a;
}

typedef struct { const char *dir, *index; } AocList;

static int aoc_arh(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    const AocList *l = ctx;
    size_t n = strlen(name);
    if (kind != PLAT_FS_FILE || n < 5 || strcasecmp(name + n - 4, ".arh")) return 1;
    char path[2400], label[64];
    if (snprintf(path, sizeof path, "%s/%s", l->dir, name) >= (int)sizeof path) return 1;
    snprintf(label, sizeof label, "aoc%s:/%s", l->index, name);
    uint64_t sz = 0;
    arh_add(label, plat_file_read(path, &sz), sz);
    return 1;
}

// The archive indexes the game opens: bf3.arh in the RomFS, and the .arh files of each add-on folder
// (fs_host.c mount_aoc: <romfs>/../aoc/<index>).
static void arh_load_all(void) {
    uint64_t n = 0;
    arh_add("rom:/bf3.arh", romfs_read_original("bf3.arh", &n), n);
    for (int i = 1; i < AOC_MAX; i++) {
        char dir[2100], index[8];
        if (snprintf(dir, sizeof dir, "%s/../aoc/%d", rt_config()->romfs, i) >= (int)sizeof dir) break;
        if (!plat_dir_exists(dir)) continue;
        snprintf(index, sizeof index, "%d", i);
        AocList l = {dir, index};
        plat_fs_list(dir, aoc_arh, &l);
    }
}

static void arh_free_all(void) {
    for (size_t k = 0; k < narh; k++) free(arhs[k].b);
    free(arhs);
    arhs = NULL;
    narh = 0;
}

void mod_archive_init(void) {
    size_t added = romfs_added_count();
    if (!added) {
        // no mod: the site keeps running the original code, without the call
        if (set >= 0) rt_patch_switch(set, 0);
        return;
    }
    if (set < 0) {
        rt_log("mods: this build's code has no modfiles patch set (generate the C again): mod files with an archive "
               "entry's path stay unread");
        return;
    }
    if (!rt_patch_enabled(set)) {
        rt_log("mods: archive overlay off (mod_archive = 0): the game keeps its archive entries, and the %zu mod files "
               "replace only plain RomFS files", added);
        return;
    }
    double t0 = (double)plat_now_ns() / 1e9;
    arh_load_all();
    size_t in_rom = 0, in_aoc = 0, loose = 0;
    char path[4200];
    for (size_t k = 0; k < added; k++) {
        const char *file;
        uint64_t size;
        const char *name = romfs_added(k, &file, &size);
        if (snprintf(path, sizeof path, "/%s", name) >= (int)sizeof path) continue;
        int hit = 0;
        for (size_t j = 0; j < narh; j++) {
            int64_t id = arh_find(&arhs[j], path);
            if (id < 0) continue;
            hit = 1;
            arhs[j].replaced++;
            if (j == 0 && !strcmp(arhs[j].label, "rom:/bf3.arh")) in_rom++;
            else in_aoc++;
            if (rt_trace & RT_TRACE_FS) {
                const uint8_t *e = arhs[j].b + arhs[j].file_off + 0x18 * (size_t)id;
                uint32_t stored = u32_at(e, 8), unpacked = u32_at(e, 12), flags = u32_at(e, 16);
                rt_log("mods: %s %s (%u bytes%s) <- %s (%llu bytes)", arhs[j].label, path,
                       flags == 2 ? unpacked : stored, flags == 2 ? ", xbc1" : "", file, (unsigned long long)size);
            }
        }
        if (!hit) {
            loose++;
            if (rt_trace & RT_TRACE_FS) rt_log("mods: %s has no archive entry; it stays a RomFS file (%s)", path, file);
        }
    }
    for (size_t j = 0; j < narh; j++)
        if (arhs[j].replaced) rt_log("mods: %s: %zu entries replaced", arhs[j].label, arhs[j].replaced);
    rt_log("mods: archive overlay: %zu entries replaced in bf3.arh, %zu in add-on archives, %zu loose files kept, "
           "%zu archives unreadable, %.2f s", in_rom, in_aoc, loose, unreadable, (double)plat_now_ns() / 1e9 - t0);
    arh_free_all();
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
}
