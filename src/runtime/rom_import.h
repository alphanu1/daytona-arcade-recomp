// Load the user's ROM set (a MAME-format zip or 7z) into the memory images
// the board runs from, in memory: every file CRC-checked against MAME's
// ROM_START for the set this build was recompiled from (M2_ROMSET:
// daytona93, the default, or daytona, Revision A), then laid out as MAME
// loads it (ROM_LOAD32_WORD interleave, ROM_COPY mirrors). The build's
// importer (tools/m2import) uses the same tables; scripts/m2import.py is the
// zip-only daytona93 reference they were checked against.
#pragma once

#include "runtime/m2_board.h"

// The ROM set this build is recompiled from (CMake's M2_ROMSET); builds that
// do not say, such as the Vita's, are daytona93's.
#ifndef M2_ROMSET
#define M2_ROMSET "daytona93"
#endif

#include <string>
#include <vector>

namespace rt {

struct RomCheck {
    std::string file;
    bool ok = false;
    std::string problem; // missing, wrong size, wrong CRC
};

// Check a zip without loading it (for the launcher's status line).
std::vector<RomCheck> check_rom_set(const std::string &zip_path);

// Load and lay out the images. Throws ZipError naming the first problem.
M2Board::Images import_rom_set(const std::string &zip_path);

// The MAME name of the set this build was recompiled from.
const char *rom_set_name();

// The MAME names of the sets an archive holds in full (every file there, at
// its size and CRC), this build's own first: an archive can hold both (MAME's
// merged daytona). For the launcher, whose other set is the other game's.
std::vector<std::string> rom_sets_in(const std::string &zip_path);

// The TGP program the i960 uploads at boot, cut from main_data (CRC-checked).
std::vector<uint8_t> tgp_program(const M2Board::Images &img);

} // namespace rt
