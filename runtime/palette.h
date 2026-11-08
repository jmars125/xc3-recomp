// Button diamonds (the battle palette and others) moved to the xbox pad layout's places (palette.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// 1 when name (an xbc1 container's name field, up to 28 bytes) is a layout palette_patch changes.
int palette_file(const char *name);
// Moves the diamonds of layout `name` (decompressed, size bytes) in place; the number of keys moved, 0 when the
// file is not laid out as expected (then nothing changed).
int palette_patch(uint8_t *file, size_t size, const char *name);
