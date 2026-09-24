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
//   --mirror A:O:L guest range A..A+L-1 reads image offset O.. (repeatable);
//                  model2o maps 0x00220000 to program offset 0x20000
//   --follow       recursive descent from the boot record (reset IP,
//                  interrupt table, system procedure table) plus any --seed;
//                  prints only reachable instructions, summary on stderr
//   --seed ADDR    extra entry point for --follow (repeatable)
//   --words FILE   decode "addr word0 word1" hex lines from FILE (no image);
//                  prints "addr length exec target canon mnemonic text", tab-separated,
//                  target "-" when none; canon 0 when the word sets bits the
//                  i960KB reserves (any quirk flag) (for cross-checking
//                  other decoders)
//
// Lines are "addr: word [word2]  text" in MAME's syntax. A trailing marker
// flags what the recompiler must not take at face value:
//   !noexec        MAME's executor does not implement the opcode
//   !quirk=...     an encoding MAME's executor and disassembler read differently

#include "i960/decode.h"
#include "i960/reach.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
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
    bool have_start = false, follow = false;
    std::vector<uint8_t> img;
    struct Mirror { uint32_t addr, off, len; };
    std::vector<Mirror> mirrors;
    std::vector<uint32_t> extra_seeds;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--words" && i + 1 < argc) {
            FILE *wf = std::fopen(argv[++i], "r");
            if (!wf) {
                std::fprintf(stderr, "i960dis: cannot open %s\n", argv[i]);
                return 2;
            }
            unsigned wa, w0, w1;
            while (std::fscanf(wf, "%x %x %x", &wa, &w0, &w1) == 3) {
                const i960::Insn in = i960::decode(wa, w0, w1);
                char tgt[16] = "-";
                if (in.has_target) std::snprintf(tgt, sizeof tgt, "%08x", in.target);
                const bool canon = in.quirks == 0;
                std::printf("%08x\t%u\t%d\t%s\t%d\t%s\t%s\n", wa, in.valid() ? in.length : 0, in.executable() ? 1 : 0,
                            tgt, canon ? 1 : 0, in.op ? in.op->mnem : "?", i960::format_mame(in).c_str());
            }
            std::fclose(wf);
            return 0;
        }
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--base") base = parse_num(next());
        else if (a == "--start") { start = parse_num(next()); have_start = true; }
        else if (a == "--count") count = parse_num(next());
        else if (a == "--follow") follow = true;
        else if (a == "--seed") extra_seeds.push_back(parse_num(next()));
        else if (a == "--mirror") {
            const std::string m = next();
            Mirror mr{};
            if (std::sscanf(m.c_str(), "%i:%i:%i", (int *)&mr.addr, (int *)&mr.off, (int *)&mr.len) != 3) usage();
            mirrors.push_back(mr);
        }
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

    // Image offset for a guest address, if mapped.
    auto offset_of = [&](uint32_t addr) -> std::optional<uint64_t> {
        for (const Mirror &m : mirrors)
            if (addr >= m.addr && addr - m.addr < m.len) return uint64_t(m.off) + (addr - m.addr);
        const uint64_t off = uint64_t(addr) - base;
        if (addr >= base && off + 4 <= img.size()) return off;
        return std::nullopt;
    };
    auto read = [&](uint32_t addr) -> std::optional<uint32_t> {
        const auto off = offset_of(addr);
        if (!off || *off + 4 > img.size()) return std::nullopt;
        const uint64_t o = *off;
        return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 | uint32_t(img[o + 3]) << 24;
    };
    auto word_at = [&](uint32_t addr) -> uint32_t { return read(addr).value_or(0); };

    auto print = [&](const i960::Insn &in) {
        const std::string text = i960::format_mame(in);
        std::string mark;
        if (in.valid() && !in.executable()) mark += " !noexec";
        if (in.quirks) {
            mark += " !quirk=";
            if (in.quirks & i960::kQuirkLowDispBits) mark += "dispbits,";
            if (in.quirks & i960::kQuirkMembBits56) mark += "membbits56,";
            if (in.quirks & i960::kQuirkMembScale) mark += "membscale,";
            if (in.quirks & i960::kQuirkSfr) mark += "sfr,";
            if (in.quirks & i960::kQuirkLiteralDst) mark += "literaldst,";
            if (in.quirks & i960::kQuirkFpLiteral) mark += "fpliteral,";
            if (in.quirks & i960::kQuirkTestFields) mark += "testfields,";
            mark.pop_back();
        }
        if (in.length == 8)
            std::printf("%08x: %08x %08x  %s%s\n", in.addr, in.word, in.disp, text.c_str(), mark.c_str());
        else
            std::printf("%08x: %08x           %s%s\n", in.addr, in.word, text.c_str(), mark.c_str());
    };

    if (follow) {
        i960::BootSeeds bs = i960::boot_seeds(read);
        std::vector<uint32_t> seeds = bs.all();
        seeds.insert(seeds.end(), extra_seeds.begin(), extra_seeds.end());
        const i960::ReachResult r = i960::reach(seeds, read);
        unsigned quirk_any = 0;
        for (const auto &[addr, in] : r.insns) {
            print(in);
            quirk_any += in.quirks != 0;
        }
        std::fprintf(stderr,
                     "seeds: reset ip %08x, %zu interrupt handlers, %zu system procedures, %zu extra\n"
                     "reachable: %zu instructions; indirect sites %zu; paths stopped on invalid/noexec %zu; "
                     "unmapped targets %zu\n"
                     "instructions with any quirk in reachable code: %u\n",
                     bs.reset_ip, bs.interrupt_handlers.size(), bs.system_procedures.size(), extra_seeds.size(),
                     r.insns.size(), r.indirect_sites.size(), r.stops.size(), r.unmapped_targets.size(),
                     quirk_any);
        for (uint32_t a : r.stops) std::fprintf(stderr, "  stop @%08x\n", a);
        for (uint32_t a : r.unmapped_targets) std::fprintf(stderr, "  unmapped target %08x\n", a);
        return 0;
    }

    const uint64_t end = uint64_t(base) + img.size();
    uint64_t addr = start & ~3u;
    for (uint32_t n = 0; n < count && addr + 4 <= end; ++n) {
        const i960::Insn in = i960::decode(uint32_t(addr), word_at(uint32_t(addr)), word_at(uint32_t(addr) + 4));
        print(in);
        addr += in.length;
    }
    return 0;
}
