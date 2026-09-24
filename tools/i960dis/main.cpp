// i960dis: linear-sweep disassembler for i960 program images.
//
//   i960dis [options] image.bin
//   i960dis [options] --interleave lo.bin hi.bin
//
// --interleave joins two ROMs loaded with MAME's ROM_LOAD32_WORD: the first
// file supplies bytes 0-1 of each 32-bit word, the second bytes 2-3 (Daytona's
// program pair is epr-16722a.12 / epr-16723a.13). Output never leaves the
// user's machine; nothing derived from the ROM belongs in the repository.
//
// Options:
//   --base ADDR    address of the image's first byte (default 0)
//   --start ADDR   first address to disassemble (default: base)
//   --count N      instructions to print (default: to the end)
//
// Lines are "addr: word [word2]  text" in MAME's syntax. A trailing marker
// flags what the recompiler must not take at face value:
//   !noexec        MAME's executor does not implement the opcode
//   !quirk=...     an encoding MAME's executor and disassembler read differently

#include "i960/decode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "i960dis: cannot open %s\n", path);
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

uint32_t parse_num(const char *s) { return uint32_t(std::strtoul(s, nullptr, 0)); }

void usage() {
    std::fprintf(stderr, "usage: i960dis [--base A] [--start A] [--count N] "
                         "(image.bin | --interleave lo.bin hi.bin)\n");
    std::exit(2);
}

} // namespace

int main(int argc, char **argv) {
    uint32_t base = 0, start = 0, count = UINT32_MAX;
    bool have_start = false;
    std::vector<uint8_t> img;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--base") base = parse_num(next());
        else if (a == "--start") { start = parse_num(next()); have_start = true; }
        else if (a == "--count") count = parse_num(next());
        else if (a == "--interleave") {
            const std::vector<uint8_t> lo = read_file(next());
            const std::vector<uint8_t> hi = read_file(next());
            if (lo.size() != hi.size() || lo.size() % 2) {
                std::fprintf(stderr, "i960dis: interleave halves must be equal, even sizes\n");
                return 2;
            }
            img.resize(lo.size() * 2);
            for (size_t w = 0; w < lo.size() / 2; ++w) {
                img[w * 4 + 0] = lo[w * 2 + 0];
                img[w * 4 + 1] = lo[w * 2 + 1];
                img[w * 4 + 2] = hi[w * 2 + 0];
                img[w * 4 + 3] = hi[w * 2 + 1];
            }
        } else if (a[0] != '-' && img.empty()) img = read_file(argv[i]);
        else usage();
    }
    if (img.empty()) usage();
    if (!have_start) start = base;

    auto word_at = [&](uint32_t addr) -> uint32_t {
        const uint64_t off = uint64_t(addr) - base;
        if (off + 4 > img.size()) return 0;
        return uint32_t(img[off]) | uint32_t(img[off + 1]) << 8 | uint32_t(img[off + 2]) << 16 |
               uint32_t(img[off + 3]) << 24;
    };

    const uint64_t end = uint64_t(base) + img.size();
    uint64_t addr = start & ~3u;
    for (uint32_t n = 0; n < count && addr + 4 <= end; ++n) {
        const i960::Insn in = i960::decode(uint32_t(addr), word_at(uint32_t(addr)), word_at(uint32_t(addr) + 4));
        const std::string text = i960::format_mame(in);
        std::string mark;
        if (in.valid() && !in.executable()) mark += " !noexec";
        if (in.quirks) {
            mark += " !quirk=";
            if (in.quirks & i960::kQuirkLowDispBits) mark += "dispbits,";
            if (in.quirks & i960::kQuirkMembBits56) mark += "membbits56,";
            if (in.quirks & i960::kQuirkMembScale) mark += "membscale,";
            mark.pop_back();
        }
        if (in.length == 8)
            std::printf("%08x: %08x %08x  %s%s\n", uint32_t(addr), in.word, in.disp, text.c_str(), mark.c_str());
        else
            std::printf("%08x: %08x           %s%s\n", uint32_t(addr), in.word, text.c_str(), mark.c_str());
        addr += in.length;
    }
    return 0;
}
