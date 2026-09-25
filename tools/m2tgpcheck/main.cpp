// m2tgpcheck: lockstep check of the recompiled TGP program against MAME.
//
//   m2tgpcheck IMAGES_DIR TGP.log [TGP.pc]
//
// TGP.log is MAME's TGP-side event log (M2TRACE_TGPLOG, patched MAME): the
// program upload, every word the TGP popped from its input FIFO and pushed
// to its output FIFO, its bank writes and its banked memory reads and writes,
// in order. The recompiled TGP code runs against it with one cursor: each
// input pop gets MAME's next "in" word, each banked read gets MAME's value
// (buffer RAM is shared with the i960 and the geometrizer), and every other
// event the TGP makes must be MAME's next event, bit for bit. TGP.pc
// (M2TRACE_TGPPC) adds a per-instruction check of PC, A, B, D, P and ST.

#include "runtime/tgp.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

struct Divergence : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Ev {
    enum Kind { Up, In, Out, Bank, Mr, Mw } kind;
    uint32_t a = 0, b = 0;
    std::vector<uint32_t> words; // Up
};
const char *kname[] = {"up", "in", "out", "bank", "mr", "mw"};

class LogBus : public rt::TgpBus {
public:
    std::vector<Ev> ev;
    size_t i = 0;
    uint64_t *count = nullptr;

    std::string next() const {
        if (i >= ev.size()) return "end of MAME's log";
        char b[96];
        std::snprintf(b, sizeof b, "MAME's event %zu is \"%s %x %x\"", i, kname[ev[i].kind], ev[i].a, ev[i].b);
        return b;
    }
    [[noreturn]] void diverge(const std::string &what) {
        char b[64];
        std::snprintf(b, sizeof b, " (TGP instruction %" PRIu64 ")", *count);
        throw Divergence(what + "; " + next() + b);
    }
    bool fifo_pop(uint32_t &v) override {
        if (i < ev.size() && ev[i].kind == Ev::In) { v = ev[i++].a; return true; }
        if (i >= ev.size() || ev[i].kind == Ev::Up) return false; // MAME's run ended here too, or reboots
        diverge("our TGP read its input FIFO");
    }
    void fifo_push(uint32_t v) override {
        if (i < ev.size() && ev[i].kind == Ev::Out && ev[i].a == v) { i++; return; }
        char b[48];
        std::snprintf(b, sizeof b, "our TGP output %08x", v);
        diverge(b);
    }
    uint32_t mem_r(uint32_t adr) override {
        if (i < ev.size() && ev[i].kind == Ev::Mr && ev[i].a == (adr & 0xffff)) return ev[i++].b;
        char b[64];
        std::snprintf(b, sizeof b, "our TGP read banked memory %06x", adr);
        diverge(b);
    }
    void mem_w(uint32_t adr, uint32_t v) override {
        if (i < ev.size() && ev[i].kind == Ev::Mw && ev[i].a == (adr & 0xffff) && ev[i].b == v) { i++; return; }
        char b[64];
        std::snprintf(b, sizeof b, "our TGP wrote %08x to banked memory %06x", v, adr);
        diverge(b);
    }
    void bank_w(uint32_t v) override {
        if (i < ev.size() && ev[i].kind == Ev::Bank && ev[i].a == v) { i++; return; }
        char b[48];
        std::snprintf(b, sizeof b, "our TGP set bank %08x", v);
        diverge(b);
    }
};

// Per-instruction check against MAME's M2TRACE_TGPPC log.
std::ifstream *pclog = nullptr;
uint64_t pc_checked = 0;
void hook(rt::Tgp &t, uint16_t ppc) {
    std::string line;
    if (!std::getline(*pclog, line)) return; // MAME's log was capped
    unsigned mp, ma, mb, md, mpp, mst;
    std::sscanf(line.c_str(), "%x %x %x %x %x %x", &mp, &ma, &mb, &md, &mpp, &mst);
    if (mp != ppc || ma != t.a || mb != t.b || md != t.d || mpp != t.p || mst != t.st) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "instruction %" PRIu64 ": MAME pc %03x a %08x b %08x d %08x p %08x st %08x; ours pc %03x a %08x b %08x "
                      "d %08x p %08x st %08x",
                      t.count, mp, ma, mb, md, mpp, mst, ppc, t.a, t.b, t.d, t.p, t.st);
        throw Divergence(b);
    }
    ++pc_checked;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: m2tgpcheck IMAGES_DIR TGP.log [TGP.pc]\n");
        return 2;
    }
    const std::string dir = argv[1];
    LogBus bus;
    rt::Tgp t;
    bus.count = &t.count;
    try {
        std::ifstream lf(argv[2]);
        if (!lf) throw std::runtime_error(std::string("cannot open ") + argv[2]);
        std::string line;
        while (std::getline(lf, line)) {
            std::istringstream s(line);
            std::string k;
            s >> k >> std::hex;
            Ev e{};
            if (k == "up") {
                e.kind = Ev::Up;
                uint32_t n = 0;
                s >> std::dec >> n >> std::hex;
                e.words.resize(n);
                for (auto &w : e.words) s >> w;
            } else if (k == "in") e.kind = Ev::In, s >> e.a;
            else if (k == "out") e.kind = Ev::Out, s >> e.a;
            else if (k == "bank") e.kind = Ev::Bank, s >> e.a;
            else if (k == "mr") e.kind = Ev::Mr, s >> e.a >> e.b;
            else if (k == "mw") e.kind = Ev::Mw, s >> e.a >> e.b;
            else continue;
            bus.ev.push_back(std::move(e));
        }
        const auto tables = load(dir + "/copro_tables.bin");
        const auto prog = load(dir + "/tgp_program.bin");
        std::vector<uint32_t> tab(tables.size() / 4);
        std::memcpy(tab.data(), tables.data(), tab.size() * 4);
        t.bus = &bus;
        t.tables = tab.data();
        std::ifstream pcf;
        if (argc == 4) {
            pcf.open(argv[3]);
            pclog = &pcf;
            t.hook = hook;
        }

        const auto t0 = std::chrono::steady_clock::now();
        int boots = 0;
        while (bus.i < bus.ev.size()) {
            const Ev &e = bus.ev[bus.i];
            if (e.kind != Ev::Up) throw Divergence("our TGP stalled on its empty input FIFO; " + bus.next());
            if (e.words.size() * 4 != prog.size() || std::memcmp(e.words.data(), prog.data(), prog.size()) != 0)
                throw Divergence("the i960 uploaded a TGP program other than the one recompiled (tgp_program.bin)");
            for (size_t k = 0; k < e.words.size(); k++) t.prog[k] = e.words[k];
            t.reset();
            ++boots;
            ++bus.i;
            rt::tgpgen::run(t, UINT64_MAX); // returns when the TGP stalls on an empty input FIFO
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t in = 0, out = 0, mem = 0;
        for (auto &e : bus.ev) in += e.kind == Ev::In, out += e.kind == Ev::Out, mem += e.kind == Ev::Mr || e.kind == Ev::Mw;
        std::printf("m2tgpcheck: MATCH. %" PRIu64 " TGP instructions, all native (%u-word program, %d boot(s)); %zu input "
                    "words, %zu output words, %zu banked accesses identical to MAME%s; %.2f s\n",
                    t.count, rt::tgpgen::program_words, boots, in, out, mem,
                    pclog ? (", every instruction's registers checked (" + std::to_string(pc_checked) + ")").c_str() : "", s);
        return 0;
    } catch (const Divergence &d) {
        std::printf("m2tgpcheck: DIVERGED: %s\n", d.what());
        return 1;
    } catch (const rt::TgpFatal &f) {
        std::printf("m2tgpcheck: FATAL after %" PRIu64 " TGP instructions: %s\n", t.count, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2tgpcheck: %s\n", e.what());
        return 2;
    }
}
