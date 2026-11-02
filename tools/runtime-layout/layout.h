// What libxc3rt.a sees of the module layout. The release builds that library with no game files: the real layout.h is
// written per install by recomp/gen.py, and modules.c and the generated code compile against it. So the runtime outside
// INSTALL_SIDE (tools/build_game.py) may read only these two, which no module size changes; any other MOD_ macro fails
// the release build here. MOD_MAIN_BASE is FIRST_BASE of recomp/layout.py; MOD_COUNT is the modules of exefs_sha256 in
// tools/data/expected-xc3-2.2.1.json that recomp/layout.py places. --prove compares both with generated/upd/layout.h.
#pragma once

#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_COUNT 3
