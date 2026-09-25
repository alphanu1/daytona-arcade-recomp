// Lockstep trace format (docs/trace-format.md): writer, reader and the
// epoch-by-epoch comparison used by tracediff.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace trace {

// FNV-1a-64 over 32-bit little-endian words (docs/trace-format.md).
constexpr uint64_t kHashInit = 0xcbf29ce484222325ull;
constexpr uint64_t kHashPrime = 0x100000001b3ull;
uint64_t hash_words(const uint32_t *words, size_t count, uint64_t h = kHashInit);
// Bytes are read as little-endian words; `bytes` must be a multiple of 4.
uint64_t hash_bytes(const uint8_t *bytes, size_t len, uint64_t h = kHashInit);

enum class RecordType : uint8_t {
    Sample = 0x01, Write = 0x10, Read = 0x11, ReadStalled = 0x12, WriteStalled = 0x13, Note = 0x20
};

constexpr int kNumRegs = 36;
// Names for Sample::regs, in order.
const char *reg_name(int i);
constexpr int kRegPc = 32, kRegAc = 33, kRegIp = 34, kRegTc = 35;

struct Region {
    uint32_t base = 0;
    uint32_t bytes = 0;
    uint64_t hash = 0;
};

struct Sample {
    uint32_t epoch = 0;
    uint64_t frame = 0;
    std::vector<Region> regions;
    std::array<uint32_t, kNumRegs> regs{};
};

struct Access {
    bool write = true;
    uint32_t addr = 0, data = 0, mask = 0;
    bool stalled = false; // the i960 stalled on it and repeated it later (MAME only)
    bool operator==(const Access &) const = default;
};

struct Header {
    std::string game, producer, trigger;
};

// Everything between two samples, and the sample that closes it.
struct Epoch {
    std::vector<Access> events;
    std::vector<std::string> notes;
    std::optional<Sample> sample; // empty for a trailing, unclosed epoch
};

class Writer {
public:
    // Opens `path` and writes the header; ok() reports failure.
    Writer(const std::string &path, const Header &h);
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    bool ok() const { return f_ != nullptr; }
    void access(const Access &a);
    void note(const std::string &text);
    void sample(const Sample &s);

private:
    void record(RecordType t, const std::vector<uint8_t> &payload);
    FILE *f_ = nullptr;
};

class Reader {
public:
    explicit Reader(const std::string &path);
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    // Empty on success, else what is wrong with the file.
    const std::string &error() const { return error_; }
    const Header &header() const { return header_; }

    // Next epoch; false at end of file (or on a malformed record, see error()).
    bool next(Epoch &out);

private:
    FILE *f_ = nullptr;
    Header header_;
    std::string error_;
};

// ---------------------------------------------------------------------------
// Comparison.

struct CompareOptions {
    bool compare_ip = false;    // ip is post-increment in MAME samples
    bool compare_frame = false; // our build has no MAME screen frame number
    uint64_t skip_regs = 0;     // bit i set: ignore regs[i]
    bool skip_stalled = true;   // drop stalled accesses before comparing (a native build never stalls)
};

struct Divergence {
    enum Kind { None, EventCount, Event, Sample, Region, Reg, Length } kind = None;
    uint64_t epoch = 0;     // index of the first divergent epoch
    size_t index = 0;       // event index / region index / register index
    std::string detail;     // human-readable
};

// First difference between two epochs, or kind None.
Divergence compare(const Epoch &a, const Epoch &b, uint64_t epoch_index, const CompareOptions &opt);

} // namespace trace
