// m2import_silent: the game's images from a ROM set, for the asset recipe and
// m2segment, which run without the sound board: the sound ROMs are not read
// (rom_import.cpp under M2_DC_NATIVE_HOST) and no sound image is written. The
// set's other ROMs are checked as m2import checks them.
//
//   m2import_silent ROMS.zip|ROMS.7z OUTDIR
//
// Output is game data: OUTDIR must be git-ignored (build/ is).

#include "runtime/rom_import.h"
#include "runtime/zip.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
void write(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    if (!f) throw std::runtime_error("cannot write " + path);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: m2import_silent ROMS.zip|ROMS.7z OUTDIR\n");
        return 2;
    }
    try {
        const rt::M2Board::Images img = rt::import_rom_set(argv[1]);
        const std::string out = argv[2];
        std::filesystem::create_directories(out);
        write(out + "/program.bin", img.program);
        write(out + "/main_data.bin", img.main_data);
        write(out + "/copro_data.bin", img.copro_data);
        write(out + "/copro_tables.bin", img.copro_tables);
        write(out + "/polygons.bin", img.polygons);
        write(out + "/textures.bin", img.textures);
        write(out + "/tgp_program.bin", rt::tgp_program(img));
        std::printf("m2import_silent: wrote the game's verified images to %s\n", out.c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2import_silent: %s\n", e.what());
        return 1;
    }
}
