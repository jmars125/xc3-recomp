// Button prompts for Xbox and PlayStation pads: the game's glyph atlases patched as RomFS reads them (prompts.c).
#pragma once
#include <stdint.h>

// romfs_read's hooks; file tells the RomFS files apart (their image offset). prompts_payload, before a read at offset:
// 1 when the read is the payload after a patched atlas header, served from the patched copy (*out set).
// prompts_header, after a read through handle: a glyph atlas's xbc1 header gets the patched file's compressed size;
// raw reads the image without the hooks.
int prompts_payload(uint64_t file, int64_t offset, void *buffer, uint64_t size, uint64_t *out);
void prompts_header(uint64_t handle, uint64_t file, int64_t offset, void *buffer, uint64_t size,
                    uint32_t (*raw)(uint64_t *, uint64_t, int64_t, void *, uint64_t));
