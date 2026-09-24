#include "trace.h"

#include <cinttypes>
#include <cstring>

namespace trace {

uint64_t hash_words(const uint32_t *w, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) {
        h ^= w[i];
        h *= kHashPrime;
    }
    return h;
}

uint64_t hash_bytes(const uint8_t *b, size_t len, uint64_t h) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        const uint32_t w = uint32_t(b[i]) | uint32_t(b[i + 1]) << 8 | uint32_t(b[i + 2]) << 16 |
                           uint32_t(b[i + 3]) << 24;
        h ^= w;
        h *= kHashPrime;
    }
    return h;
}

const char *reg_name(int i) {
    static const char *const names[kNumRegs] = {
        "pfp", "sp", "rip", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
        "r12", "r13", "r14", "r15", "g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7",
        "g8", "g9", "g10", "g11", "g12", "g13", "g14", "fp", "pc", "ac", "ip", "tc",
    };
    return (i >= 0 && i < kNumRegs) ? names[i] : "?";
}

// ---------------------------------------------------------------------------
// Encoding helpers.

namespace {

constexpr char kMagic[4] = {'M', '2', 'T', 'R'};
constexpr uint32_t kVersion = 1;

void put8(std::vector<uint8_t> &v, uint8_t x) { v.push_back(x); }
void put16(std::vector<uint8_t> &v, uint16_t x) {
    for (int i = 0; i < 2; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put64(std::vector<uint8_t> &v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void putstr(std::vector<uint8_t> &v, const std::string &s) {
    put16(v, uint16_t(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

struct Cursor {
    const uint8_t *p;
    const uint8_t *end;
    bool ok = true;

    bool need(size_t n) {
        if (size_t(end - p) < n) ok = false;
        return ok;
    }
    uint64_t le(int bytes) {
        if (!need(size_t(bytes))) return 0;
        uint64_t x = 0;
        for (int i = 0; i < bytes; ++i) x |= uint64_t(p[i]) << (8 * i);
        p += bytes;
        return x;
    }
    uint8_t u8() { return uint8_t(le(1)); }
    uint16_t u16() { return uint16_t(le(2)); }
    uint32_t u32() { return uint32_t(le(4)); }
    uint64_t u64() { return le(8); }
    std::string str() {
        const uint16_t n = u16();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char *>(p), n);
        p += n;
        return s;
    }
};

bool read_exact(FILE *f, void *dst, size_t n) { return std::fread(dst, 1, n, f) == n; }

} // namespace

// ---------------------------------------------------------------------------
// Writer.

Writer::Writer(const std::string &path, const Header &h) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return;
    std::vector<uint8_t> body;
    putstr(body, h.game);
    putstr(body, h.producer);
    putstr(body, h.trigger);
    std::vector<uint8_t> head(kMagic, kMagic + 4);
    put32(head, kVersion);
    put32(head, uint32_t(body.size()));
    head.insert(head.end(), body.begin(), body.end());
    std::fwrite(head.data(), 1, head.size(), f_);
}

Writer::~Writer() {
    if (f_) std::fclose(f_);
}

void Writer::record(RecordType t, const std::vector<uint8_t> &payload) {
    if (!f_) return;
    std::vector<uint8_t> r;
    put8(r, uint8_t(t));
    put32(r, uint32_t(payload.size()));
    r.insert(r.end(), payload.begin(), payload.end());
    std::fwrite(r.data(), 1, r.size(), f_);
}

void Writer::access(const Access &a) {
    std::vector<uint8_t> p;
    put32(p, a.addr);
    put32(p, a.data);
    put32(p, a.mask);
    record(a.write ? RecordType::Write : RecordType::Read, p);
}

void Writer::note(const std::string &text) {
    record(RecordType::Note, std::vector<uint8_t>(text.begin(), text.end()));
}

void Writer::sample(const Sample &s) {
    std::vector<uint8_t> p;
    put32(p, s.epoch);
    put64(p, s.frame);
    put8(p, uint8_t(s.regions.size()));
    for (const Region &r : s.regions) {
        put32(p, r.base);
        put32(p, r.bytes);
        put64(p, r.hash);
    }
    for (uint32_t v : s.regs) put32(p, v);
    record(RecordType::Sample, p);
}

// ---------------------------------------------------------------------------
// Reader.

Reader::Reader(const std::string &path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) {
        error_ = "cannot open " + path;
        return;
    }
    uint8_t fixed[12];
    if (!read_exact(f_, fixed, sizeof fixed) || std::memcmp(fixed, kMagic, 4) != 0) {
        error_ = path + ": not an M2TR trace";
        return;
    }
    Cursor c{fixed + 4, fixed + 12};
    const uint32_t version = c.u32();
    const uint32_t hbytes = c.u32();
    if (version != kVersion) {
        error_ = path + ": unsupported trace version " + std::to_string(version);
        return;
    }
    std::vector<uint8_t> body(hbytes);
    if (!read_exact(f_, body.data(), body.size())) {
        error_ = path + ": truncated header";
        return;
    }
    Cursor h{body.data(), body.data() + body.size()};
    header_.game = h.str();
    header_.producer = h.str();
    header_.trigger = h.str();
    if (!h.ok) error_ = path + ": malformed header";
}

Reader::~Reader() {
    if (f_) std::fclose(f_);
}

bool Reader::next(Epoch &out) {
    out = Epoch{};
    if (!f_ || !error_.empty()) return false;
    bool any = false;
    for (;;) {
        uint8_t rh[5];
        const size_t got = std::fread(rh, 1, sizeof rh, f_);
        if (got == 0) return any; // clean end of file
        if (got != sizeof rh) {
            error_ = "truncated record header";
            return false;
        }
        Cursor hc{rh, rh + 5};
        const auto type = RecordType(hc.u8());
        const uint32_t len = hc.u32();
        std::vector<uint8_t> payload(len);
        if (!read_exact(f_, payload.data(), len)) {
            error_ = "truncated record payload";
            return false;
        }
        Cursor c{payload.data(), payload.data() + payload.size()};
        any = true;
        switch (type) {
        case RecordType::Write:
        case RecordType::Read: {
            Access a;
            a.write = type == RecordType::Write;
            a.addr = c.u32();
            a.data = c.u32();
            a.mask = c.u32();
            if (!c.ok) {
                error_ = "short access record";
                return false;
            }
            out.events.push_back(a);
            break;
        }
        case RecordType::Note:
            out.notes.emplace_back(payload.begin(), payload.end());
            break;
        case RecordType::Sample: {
            Sample s;
            s.epoch = c.u32();
            s.frame = c.u64();
            const uint8_t n = c.u8();
            for (unsigned i = 0; i < n; ++i) {
                Region r;
                r.base = c.u32();
                r.bytes = c.u32();
                r.hash = c.u64();
                s.regions.push_back(r);
            }
            for (uint32_t &v : s.regs) v = c.u32();
            if (!c.ok) {
                error_ = "short sample record";
                return false;
            }
            out.sample = std::move(s);
            return true;
        }
        default:
            break; // unknown type: skipped by length
        }
    }
}

// ---------------------------------------------------------------------------
// Comparison.

namespace {

std::string access_str(const Access &a) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s %08x = %08x (mask %08x)", a.write ? "W" : "R", a.addr, a.data,
                  a.mask);
    return buf;
}

} // namespace

Divergence compare(const Epoch &a, const Epoch &b, uint64_t ei, const CompareOptions &opt) {
    Divergence d;
    d.epoch = ei;
    char buf[256];

    const size_t n = std::min(a.events.size(), b.events.size());
    for (size_t i = 0; i < n; ++i) {
        if (!(a.events[i] == b.events[i])) {
            d.kind = Divergence::Event;
            d.index = i;
            d.detail = "event " + std::to_string(i) + ": A " + access_str(a.events[i]) + " | B " +
                       access_str(b.events[i]);
            return d;
        }
    }
    if (a.events.size() != b.events.size()) {
        d.kind = Divergence::EventCount;
        d.index = n;
        const Epoch &longer = a.events.size() > b.events.size() ? a : b;
        std::snprintf(buf, sizeof buf, "event count A %zu, B %zu; first extra in %s: %s", a.events.size(),
                      b.events.size(), &longer == &a ? "A" : "B", access_str(longer.events[n]).c_str());
        d.detail = buf;
        return d;
    }

    if (a.sample.has_value() != b.sample.has_value()) {
        d.kind = Divergence::Length;
        d.detail = std::string("trace ") + (a.sample ? "B" : "A") + " ends before this epoch's sample";
        return d;
    }
    if (!a.sample) return d;
    const Sample &sa = *a.sample, &sb = *b.sample;

    if (opt.compare_frame && sa.frame != sb.frame) {
        d.kind = Divergence::Sample;
        std::snprintf(buf, sizeof buf, "frame A %" PRIu64 ", B %" PRIu64, sa.frame, sb.frame);
        d.detail = buf;
        return d;
    }
    if (sa.regions.size() != sb.regions.size()) {
        d.kind = Divergence::Sample;
        d.detail = "region count differs: A " + std::to_string(sa.regions.size()) + ", B " +
                   std::to_string(sb.regions.size());
        return d;
    }
    for (size_t i = 0; i < sa.regions.size(); ++i) {
        const Region &ra = sa.regions[i], &rb = sb.regions[i];
        if (ra.base != rb.base || ra.bytes != rb.bytes || ra.hash != rb.hash) {
            d.kind = Divergence::Region;
            d.index = i;
            std::snprintf(buf, sizeof buf, "region %08x+%x: A hash %016" PRIx64 ", B %08x+%x hash %016" PRIx64,
                          ra.base, ra.bytes, ra.hash, rb.base, rb.bytes, rb.hash);
            d.detail = buf;
            return d;
        }
    }
    for (int i = 0; i < kNumRegs; ++i) {
        if (opt.skip_regs >> i & 1) continue;
        if (i == kRegIp && !opt.compare_ip) continue;
        if (sa.regs[i] != sb.regs[i]) {
            d.kind = Divergence::Reg;
            d.index = size_t(i);
            std::snprintf(buf, sizeof buf, "%s: A %08x, B %08x", reg_name(i), sa.regs[i], sb.regs[i]);
            d.detail = buf;
            return d;
        }
    }
    return d;
}

} // namespace trace
