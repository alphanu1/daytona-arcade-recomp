// Trace format tests: hash, writer/reader round trip, and that compare()
// reports each kind of divergence at the right place.
//
//   test_trace                    run the tests
//   test_trace --reference PATH   write the reference trace the Lua
//                                 cross-check (tests/lua_core_test.py) must
//                                 reproduce byte-for-byte in meaning

#include "trace/trace.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

// Deterministic test "memory": bytes i*7+3, not game data.
std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 7 + 3 + seed);
    return v;
}

trace::Sample make_sample(uint32_t epoch, uint8_t seed) {
    trace::Sample s;
    s.epoch = epoch;
    s.frame = 1000 + epoch;
    const std::vector<uint8_t> mem = pattern(4096, seed);
    s.regions.push_back({0x00200000, 4096, trace::hash_bytes(mem.data(), mem.size())});
    s.regions.push_back({0x00500000, 16, trace::hash_bytes(mem.data(), 16)});
    for (int i = 0; i < trace::kNumRegs; ++i) s.regs[size_t(i)] = uint32_t(i * 0x01010101u + epoch);
    return s;
}

// The reference trace: two epochs, a note, reads and writes.
void write_reference(const std::string &path) {
    trace::Writer w(path, {"daytona", "reference", "vblank-ack"});
    w.access({true, 0x00884000, 0x3f800000, 0xffffffff});
    w.access({false, 0x00884000, 0xdeadbeef, 0xffffffff});
    w.note("reset");
    w.access({true, 0x00e80000, 0xfffffffe, 0xffffffff});
    w.sample(make_sample(0, 0));
    w.access({true, 0x01c80000, 0x00000041, 0x000000ff});
    w.sample(make_sample(1, 5));
}

void test_hash() {
    CHECK(trace::hash_words(nullptr, 0) == trace::kHashInit);
    // One word by hand: (init ^ 1) * prime mod 2^64.
    const uint32_t one = 1;
    CHECK(trace::hash_words(&one, 1) == (trace::kHashInit ^ 1) * trace::kHashPrime);
    // Bytes are little-endian words.
    const uint8_t b[8] = {0x78, 0x56, 0x34, 0x12, 0xff, 0, 0, 0x80};
    const uint32_t w[2] = {0x12345678, 0x800000ff};
    CHECK(trace::hash_bytes(b, 8) == trace::hash_words(w, 2));
    // Order matters (this is why FNV, not a sum).
    const uint32_t swapped[2] = {w[1], w[0]};
    CHECK(trace::hash_words(w, 2) != trace::hash_words(swapped, 2));
}

void test_round_trip(const std::string &dir) {
    const std::string path = dir + "/rt.m2tr";
    write_reference(path);
    trace::Reader r(path);
    CHECK(r.error().empty());
    CHECK(r.header().game == "daytona" && r.header().producer == "reference" &&
          r.header().trigger == "vblank-ack");
    trace::Epoch e;
    CHECK(r.next(e));
    CHECK(e.events.size() == 3 && e.notes.size() == 1 && e.notes[0] == "reset");
    CHECK(!e.events[1].write && e.events[1].data == 0xdeadbeef);
    CHECK(e.sample && e.sample->epoch == 0 && e.sample->frame == 1000 && e.sample->regions.size() == 2);
    CHECK(e.sample->regs[trace::kRegTc] == 35 * 0x01010101u);
    CHECK(r.next(e));
    CHECK(e.events.size() == 1 && e.events[0].mask == 0xff && e.sample && e.sample->epoch == 1);
    CHECK(!r.next(e) && r.error().empty());
}

void test_unknown_record_skipped(const std::string &dir) {
    const std::string path = dir + "/unk.m2tr";
    write_reference(path);
    // Append an unknown record type 0x7f with a 3-byte payload, then an epoch.
    FILE *f = std::fopen(path.c_str(), "ab");
    const uint8_t unk[] = {0x7f, 3, 0, 0, 0, 'x', 'y', 'z'};
    std::fwrite(unk, 1, sizeof unk, f);
    std::fclose(f);
    trace::Reader r(path);
    trace::Epoch e;
    int n = 0;
    while (r.next(e)) ++n;
    CHECK(r.error().empty());
    CHECK(n == 3); // two sampled epochs and a trailing unclosed one
    CHECK(!e.sample);
}

void test_truncated(const std::string &dir) {
    const std::string path = dir + "/trunc.m2tr";
    write_reference(path);
    FILE *f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> all(1 << 16);
    all.resize(std::fread(all.data(), 1, all.size(), f));
    std::fclose(f);
    f = std::fopen(path.c_str(), "wb");
    std::fwrite(all.data(), 1, all.size() - 5, f); // cut into the last sample
    std::fclose(f);
    trace::Reader r(path);
    trace::Epoch e;
    CHECK(r.next(e));
    CHECK(!r.next(e));
    CHECK(!r.error().empty());
}

void test_compare() {
    trace::Epoch a;
    a.events = {{true, 0x884000, 1, ~0u}, {false, 0x884000, 2, ~0u}};
    a.sample = make_sample(7, 1);
    const trace::CompareOptions opt;

    CHECK(trace::compare(a, a, 7, opt).kind == trace::Divergence::None);

    trace::Epoch b = a;
    b.events[1].data = 3;
    auto d = trace::compare(a, b, 7, opt);
    CHECK(d.kind == trace::Divergence::Event && d.index == 1 && d.epoch == 7);

    b = a;
    b.events.push_back({true, 0x1c80000, 0x41, 0xff});
    d = trace::compare(a, b, 7, opt);
    CHECK(d.kind == trace::Divergence::EventCount && d.index == 2);

    b = a;
    b.sample->regions[1].hash ^= 1;
    d = trace::compare(a, b, 7, opt);
    CHECK(d.kind == trace::Divergence::Region && d.index == 1);

    b = a;
    b.sample->regs[17] ^= 0x80000000; // g1
    d = trace::compare(a, b, 7, opt);
    CHECK(d.kind == trace::Divergence::Reg && d.index == 17);
    trace::CompareOptions skip;
    skip.skip_regs = uint64_t(1) << 17;
    CHECK(trace::compare(a, b, 7, skip).kind == trace::Divergence::None);

    // ip and frame are ignored unless asked for.
    b = a;
    b.sample->regs[trace::kRegIp] += 4;
    b.sample->frame += 1;
    CHECK(trace::compare(a, b, 7, opt).kind == trace::Divergence::None);
    trace::CompareOptions strict;
    strict.compare_ip = true;
    CHECK(trace::compare(a, b, 7, strict).kind == trace::Divergence::Reg);
    strict.compare_frame = true;
    CHECK(trace::compare(a, b, 7, strict).kind == trace::Divergence::Sample);

    b = a;
    b.sample.reset();
    CHECK(trace::compare(a, b, 7, opt).kind == trace::Divergence::Length);
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--reference") == 0) {
        write_reference(argv[2]);
        return 0;
    }
    const std::string dir = argc > 1 ? argv[1] : ".";
    test_hash();
    test_round_trip(dir);
    test_unknown_record_skipped(dir);
    test_truncated(dir);
    test_compare();
    std::printf("test_trace: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
