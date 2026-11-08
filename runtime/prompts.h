// Button prompts for Xbox and PlayStation pads: the game's glyph atlases patched as RomFS reads them (prompts.c).
#pragma once
#include <stdint.h>

// The read hooks of romfs_read and of add-on files (fs_host.c, read_file); file tells the files apart (a RomFS file's
// image offset, an add-on file's key with the top bit set). prompts_payload, before a read at offset: 1 when the read is
// the payload after a patched header, served from the patched copy (*out set). prompts_header, after a read through
// handle: the xbc1 header of a glyph atlas or a palette layout (palette.c) gets the patched file's compressed size; raw
// reads the file without the hooks.
int prompts_payload(uint64_t file, int64_t offset, void *buffer, uint64_t size, uint64_t *out);
void prompts_header(uint64_t handle, uint64_t file, int64_t offset, void *buffer, uint64_t size,
                    uint32_t (*raw)(uint64_t *, uint64_t, int64_t, void *, uint64_t));
