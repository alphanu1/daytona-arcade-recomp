#include "runtime/m2_replay_bus.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

namespace {

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

// Hashed regions, as the MAME plugin samples them (docs/trace-format.md).
constexpr struct {
    uint32_t base, bytes;
} kRegions[] = {
    {0x00200000, 0x20000}, {0x00500000, 0x100000}, {0x00900000, 0x20000},
    {0x01d00000, 0x4000},  {0x01000000, 0x10000},  {0x01800000, 0x4000},
};

// Device ranges, mirroring the plugin's taps (tools/mame-plugins/m2trace):
// reads are answered from the trace everywhere below; writes are checked
// against it only where the plugin taps writes. Elsewhere a write goes to
// scratch memory, since MAME recorded nothing to check it against.
constexpr struct {
    uint32_t start, end;
} kReadTaps[] = {
    {0x00900000, 0x0097ffff}, // buffer RAM: the geometrizer and TGP write it too
    {0x00800000, 0x00807fff}, {0x00880000, 0x00887fff}, {0x00980000, 0x0098003f}, {0x00e80000, 0x00e80007},
    {0x00f00000, 0x00f0000f}, {0x01a00000, 0x01a1ffff}, {0x01c00000, 0x01c00fff}, {0x01c80000, 0x01c80003},
    {0x10000000, 0x105fffff},
};
constexpr struct {
    uint32_t start, end;
} kWriteTaps[] = {
    {0x00900000, 0x0097ffff},
    {0x00800000, 0x00803fff}, {0x00804000, 0x00807fff}, {0x00880000, 0x00883fff}, {0x00884000, 0x00887fff},
    {0x00980000, 0x0098000f}, {0x00e80000, 0x00e80007}, {0x00f00000, 0x00f0000f}, {0x01c00000, 0x01c00fff},
    {0x01c80000, 0x01c80003},
};

bool write_tapped(uint32_t a) {
    for (const auto &t : kWriteTaps)
        if (a >= t.start && a <= t.end) return true;
    return false;
}

} // namespace

M2ReplayBus::M2ReplayBus(std::vector<uint8_t> program, std::vector<uint8_t> main_data, const std::string &trace_path)
    : program_(std::move(program)), main_data_(std::move(main_data)), ram_(0x20000), work_(0x100000),
      buffer_(0x20000), cpuctl_(0x1000), backup_(0x4000, 0xff), tile_(0x10000), chr_(0x80000), palette_(0x4000),
      xlat_(0xc000), tex0_(0x200000), tex1_(0x200000), luma_(0x20000), fb_a_(0x80000), fb_b_(0x80000),
      pages_(size_t(1) << (32 - kPageBits)), reader_(trace_path) {
    if (!reader_.error().empty()) throw std::runtime_error(reader_.error());

    // model2o memory map (MAME model2.cpp: model2_base_mem, model2_tgp_mem, model2o_mem).
    map(0x00000000, 0x001fffff, Rom, program_.data());
    map(0x00200000, 0x0021ffff, Ram, ram_.data());
    map(0x00220000, 0x0023ffff, Rom, program_.data() + 0x20000);
    map(0x00500000, 0x005fffff, Ram, work_.data());
    // Buffer RAM (0x00900000, mirror 0x60000) is a device range here: see kReadTaps.
    map(0x00e00000, 0x00e00fff, Ram, cpuctl_.data(), 0, false); // CPU control (wait states): no BURST
    map(0x01000000, 0x0100ffff, Ram, tile_.data(), 0x110000);
    map(0x01080000, 0x010fffff, Ram, chr_.data(), 0x100000);
    map(0x01800000, 0x01803fff, Ram, palette_.data());
    map(0x01810000, 0x0181bfff, Ram, xlat_.data());
    map(0x01d00000, 0x01d03fff, Ram, backup_.data());
    map(0x02000000, 0x03ffffff, Rom, main_data_.data());
    map(0x06000000, 0x06ffffff, Rom, main_data_.data() + 0x1000000);
    map(0x11600000, 0x1167ffff, Ram, fb_a_.data());
    map(0x11680000, 0x116fffff, Ram, fb_b_.data());
    map(0x12000000, 0x121fffff, Ram, tex0_.data(), 0x200000);
    map(0x12400000, 0x125fffff, Ram, tex1_.data(), 0x200000);
    map(0x12800000, 0x1281ffff, Ram, luma_.data());
    for (const auto &d : kReadTaps) map_device(d.start, d.end);
    // Device ranges MAME flags BURST: geometrizer program port, TGP function
    // port, comm board shared RAM.
    set_burst(0x00804000, 0x00807fff);
    set_burst(0x00880000, 0x00883fff);
    set_burst(0x01a00000, 0x01a1ffff);
    set_burst(0x00900000, 0x0097ffff);

    reader_.next(cur_);
}

void M2ReplayBus::map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror, bool burst) {
    // Every combination of mirror bits maps to the same storage.
    for (uint32_t m = 0;; m = (m - mirror) & mirror) {
        for (uint64_t a = start; a <= end; a += (1u << kPageBits)) {
            Page &p = pages_[uint32_t(a | m) >> kPageBits];
            p.kind = k;
            p.burst = burst;
            p.base = base + (a - start);
        }
        if (((m - mirror) & mirror) == 0) break;
    }
}

void M2ReplayBus::map_device(uint32_t start, uint32_t end) {
    for (uint64_t a = start & ~0xfffu; a <= end; a += (1u << kPageBits)) {
        pages_[uint32_t(a) >> kPageBits].kind = Device;
        pages_[uint32_t(a) >> kPageBits].burst = false;
    }
}

void M2ReplayBus::set_burst(uint32_t start, uint32_t end) {
    for (uint64_t a = start & ~0xfffu; a <= end; a += (1u << kPageBits)) pages_[uint32_t(a) >> kPageBits].burst = true;
}

uint8_t *M2ReplayBus::sparse(uint32_t addr) {
    auto &slot = sparse_[addr >> kPageBits];
    if (!slot) slot = std::make_unique<std::array<uint8_t, 1u << kPageBits>>(), slot->fill(0);
    return slot->data() + (addr & ((1u << kPageBits) - 1));
}

std::string M2ReplayBus::where() const {
    char b[128];
    std::snprintf(b, sizeof b, "epoch %" PRIu64 ", event %zu of %zu (%" PRIu64 " events matched), PIP %08x", epoch_, pos_,
                  cur_.events.size(), events_, core_ ? core_->m_PIP : 0);
    return b;
}

const trace::Access &M2ReplayBus::next_event(bool write, uint32_t addr, uint32_t mask) {
    while (pos_ < cur_.events.size() && cur_.events[pos_].stalled) ++pos_;
    if (done_ || pos_ >= cur_.events.size())
        throw Divergence("extra " + std::string(write ? "write" : "read") + " at " + hex(addr) + " mask " + hex(mask) +
                         ": the trace has no more events in this epoch; " + where());
    const trace::Access &e = cur_.events[pos_];
    if (e.write != write || (e.addr & ~3u) != (addr & ~3u) || e.mask != mask)
        throw Divergence(std::string("expected ") + (e.write ? "W " : "R ") + hex(e.addr) + " mask " + hex(e.mask) +
                         ", got " + (write ? "W " : "R ") + hex(addr) + " mask " + hex(mask) + "; " + where());
    return e;
}

void M2ReplayBus::end_of_epoch_check() {
    while (pos_ < cur_.events.size() && cur_.events[pos_].stalled) ++pos_;
    if (pos_ != cur_.events.size() || !cur_.sample) return; // sample comes after the last event
    const trace::Sample &s = *cur_.sample;
    // Region hashes from our memory, as the plugin computes them.
    for (size_t i = 0; i < s.regions.size() && i < std::size(kRegions); ++i) {
        const auto &r = kRegions[i];
        // Buffer RAM is written by the geometrizer port and the TGP as well
        // as the i960. Replayed (M1), only the i960's accesses to it are
        // checked, event by event. Modelled (M2), its hash is measured: the
        // TGP's mailbox words depend on how far the TGP has run.
        if (r.base == 0x00900000) {
            if (const uint8_t *m = model_ ? model_->region(r.base) : nullptr) {
                ++model_->region_samples;
                if (trace::hash_bytes(m, r.bytes) != s.regions[i].hash) ++model_->region_mismatch;
            }
            continue;
        }
        std::vector<uint8_t> bytes(r.bytes);
        for (uint32_t o = 0; o < r.bytes; o += 4) {
            const uint32_t a = r.base + o;
            const Page &p = pages_[a >> kPageBits];
            std::memcpy(&bytes[o], p.base + (a & ((1u << kPageBits) - 1)), 4);
        }
        const uint64_t h = trace::hash_bytes(bytes.data(), bytes.size());
        if (h != s.regions[i].hash) {
            // Leave our copy next to MAME's dump (M2TRACE_DUMP_EPOCH) for a byte diff.
            if (const char *dir = std::getenv("M2REPLAY_DUMP_DIR")) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/ours_e%" PRIu64 "_%08x.bin", dir, epoch_, r.base);
                if (FILE *f = std::fopen(path, "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), f);
                    std::fclose(f);
                }
            }
            throw Divergence("region " + hex(r.base) + " hash differs at the sample closing " + where());
        }
    }
    if (core_) {
        for (int i = 0; i < 32; ++i)
            if (core_->m_r[i] != s.regs[size_t(i)])
                throw Divergence(std::string("register ") + trace::reg_name(i) + ": ours " + hex(core_->m_r[i]) +
                                 ", MAME " + hex(s.regs[size_t(i)]) + " at the sample closing " + where());
        if (core_->m_PC != s.regs[trace::kRegPc])
            throw Divergence("pc: ours " + hex(core_->m_PC) + ", MAME " + hex(s.regs[trace::kRegPc]) + "; " + where());
        if (core_->m_AC != s.regs[trace::kRegAc])
            throw Divergence("ac: ours " + hex(core_->m_AC) + ", MAME " + hex(s.regs[trace::kRegAc]) + "; " + where());
    }
    ++epoch_;
    pos_ = 0;
    if (!reader_.next(cur_)) done_ = true;
}

bool M2ReplayBus::all_events_consumed() {
    if (done_) return true;
    while (pos_ < cur_.events.size() && cur_.events[pos_].stalled) ++pos_;
    if (pos_ < cur_.events.size() || cur_.sample) return false; // unmatched events, or a sample we never reached
    trace::Epoch more;
    return !reader_.next(more);                                 // nothing after this trailing epoch
}

uint32_t M2ReplayBus::device_read(uint32_t addr, uint32_t mask) {
    const trace::Access &e = next_event(false, addr, mask);
    uint32_t v = e.data;
    if (model_ && model_->claims(addr)) {
        uint32_t ours = 0;
        const auto check = model_->read(addr, mask, ours);
        if ((ours & mask) != (e.data & mask)) {
            if (check == DeviceModel::Strict)
                throw Divergence("read " + hex(addr) + ": ours " + hex(ours & mask) + ", MAME " + hex(e.data & mask) + "; " +
                                 where());
            model_->measured_mismatch(addr, ours & mask, e.data & mask);
        }
    }
    ++pos_;
    ++events_;
    return v;
}

void M2ReplayBus::device_write(uint32_t addr, uint32_t data, uint32_t mask) {
    const trace::Access &e = next_event(true, addr, mask);
    if ((e.data & mask) != (data & mask))
        throw Divergence("write " + hex(addr) + ": ours " + hex(data & mask) + ", MAME " + hex(e.data & mask) + "; " +
                         where());
    ++pos_;
    ++events_;
    if (model_ && model_->claims(addr)) model_->write(addr, data, mask);
    end_of_epoch_check();
}

// Sub-word device accesses reach the tap as the containing dword with a lane
// mask, as in MAME's 32-bit little-endian space.
uint32_t M2ReplayBus::peek(uint32_t addr) {
    addr &= ~3u;
    const Page &p = page(addr);
    uint32_t v;
    if (p.kind == Rom || p.kind == Ram) std::memcpy(&v, p.base + (addr & 0xfff), 4);
    else if (p.kind == Unmapped) std::memcpy(&v, sparse(addr), 4);
    else throw Divergence("peek at device address " + hex(addr));
    return v;
}

uint32_t M2ReplayBus::fetch(uint32_t addr) {
    const Page &p = page(addr);
    if (p.kind != Rom && p.kind != Ram) throw Divergence("instruction fetch from non-memory " + hex(addr));
    uint32_t v;
    std::memcpy(&v, p.base + (addr & 0xffc), 4);
    return v;
}

uint8_t M2ReplayBus::read_byte(uint32_t addr) {
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Rom:
    case Ram: return p.base[addr & 0xfff];
    case Device: return uint8_t(device_read(addr & ~3u, 0xffu << sh) >> sh);
    default: return *sparse(addr);
    }
}

uint16_t M2ReplayBus::read_word(uint32_t addr) {
    addr &= ~1u;
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Rom:
    case Ram: {
        uint16_t v;
        std::memcpy(&v, p.base + (addr & 0xfff), 2);
        return v;
    }
    case Device: return uint16_t(device_read(addr & ~3u, 0xffffu << sh) >> sh);
    default: {
        uint16_t v;
        std::memcpy(&v, sparse(addr), 2);
        return v;
    }
    }
}

uint32_t M2ReplayBus::read_dword(uint32_t addr) {
    addr &= ~3u;
    const Page &p = page(addr);
    switch (p.kind) {
    case Rom:
    case Ram: {
        uint32_t v;
        std::memcpy(&v, p.base + (addr & 0xfff), 4);
        return v;
    }
    case Device: return device_read(addr, 0xffffffffu);
    default: {
        uint32_t v;
        std::memcpy(&v, sparse(addr), 4);
        return v;
    }
    }
}

void M2ReplayBus::write_byte(uint32_t addr, uint8_t data) {
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Rom: return; // nopw
    case Ram: p.base[addr & 0xfff] = data; return;
    case Device:
        if (write_tapped(addr)) { device_write(addr & ~3u, uint32_t(data) << sh, 0xffu << sh); return; }
        [[fallthrough]];
    default: *sparse(addr) = data;
    }
}

void M2ReplayBus::write_word(uint32_t addr, uint16_t data) {
    addr &= ~1u;
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Rom: return;
    case Ram: std::memcpy(p.base + (addr & 0xfff), &data, 2); return;
    case Device:
        if (write_tapped(addr)) { device_write(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh); return; }
        [[fallthrough]];
    default: std::memcpy(sparse(addr), &data, 2);
    }
}

void M2ReplayBus::write_dword(uint32_t addr, uint32_t data) {
    addr &= ~3u;
    const Page &p = page(addr);
    switch (p.kind) {
    case Rom: return;
    case Ram: std::memcpy(p.base + (addr & 0xfff), &data, 4); return;
    case Device:
        if (write_tapped(addr)) { device_write(addr, data, 0xffffffffu); return; }
        [[fallthrough]];
    default: std::memcpy(sparse(addr), &data, 4);
    }
}

} // namespace rt
