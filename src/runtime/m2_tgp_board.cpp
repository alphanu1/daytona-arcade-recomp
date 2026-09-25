// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// Model 2 geometry ports as the i960 sees them, transplanted from MAME's
// src/mame/sega/model2.cpp (copro_ctl1_w, copro_function_port_w,
// copro_fifo_r/w, geo_r/w, geo_prg_w, geo_ctl1_w, push_geo_data,
// copro_tgp_memory_r/w) at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notice above kept as the licence requires). MAME's device
// FIFOs, halts and stalls are replaced by running the recompiled TGP on
// demand. See THIRD_PARTY.md.

#include "runtime/m2_tgp_board.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

namespace {
uint32_t crc32_words(const std::vector<uint32_t> &w) {
    uint32_t c = 0xffffffffu;
    for (uint32_t v : w)
        for (int b = 0; b < 4; b++) {
            c ^= (v >> (8 * b)) & 0xff;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        }
    return ~c;
}

std::vector<uint32_t> words(const std::vector<uint8_t> &b) {
    std::vector<uint32_t> w(b.size() / 4);
    std::memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}
} // namespace

TgpBoard::TgpBoard(const std::vector<uint8_t> &tables, const std::vector<uint8_t> &copro_data)
    : tables_(words(tables)), copro_data_(words(copro_data)) {
    if (tables_.size() != 0x10000 || copro_data_.size() != 0x200000) throw TgpFatal("bad TGP table or copro data image");
    tgp_.bus = this;
    tgp_.tables = tables_.data();
    // MAME machine_reset: "initialize bufferram to a sane default".
    for (auto &w : buffer_) w = 0x07800f0f;
}

void TgpBoard::coproctl_w(uint32_t data, uint32_t mask) {
    if ((data ^ coproctl_) == 0x80000000) {
        if (data & 0x80000000) { // start upload: the TGP is held
            upload_.clear();
            booted_ = false;
        } else { // boot: the TGP starts from reset on the uploaded program
            if (upload_.size() != tgpgen::program_words || crc32_words(upload_) != tgpgen::program_crc32)
                throw TgpFatal("the i960 uploaded a TGP program other than the one recompiled");
            for (size_t i = 0; i < upload_.size(); i++) tgp_.prog[i] = upload_[i];
            tgp_.reset();
            booted_ = true;
        }
    }
    coproctl_ = (coproctl_ & ~mask) | (data & mask);
}

void TgpBoard::function_port_w(uint32_t offset, uint32_t data) {
    uint32_t d = data & 0x800fffff;
    const uint32_t a = (offset >> 2) & 0xff;
    d |= a << 23;
    in_.push_back(d);
}

void TgpBoard::fifo_w(uint32_t data) {
    if (coproctl_ & 0x80000000) {
        if (upload_.size() >= 0x1000) throw TgpFatal("TGP program upload past 4K words");
        upload_.push_back(data);
    } else {
        in_.push_back(data);
    }
}

void TgpBoard::run_tgp() {
    if (booted_) tgpgen::run(tgp_, UINT64_MAX); // until it waits on an empty input FIFO
}

uint32_t TgpBoard::fifo_r() {
    if (out_.empty()) run_tgp();
    if (out_.empty()) throw TgpFatal("the i960 read the TGP's output FIFO, but the TGP has consumed all its input and produced nothing");
    const uint32_t v = out_.front();
    out_.pop_front();
    return v;
}

bool TgpBoard::fifo_out_empty() {
    if (out_.empty()) run_tgp();
    return out_.empty();
}

bool TgpBoard::fifo_pop(uint32_t &v) {
    if (in_.empty()) return false;
    v = in_.front();
    in_.pop_front();
    return true;
}

uint32_t TgpBoard::mem_r(uint32_t adr) {
    if (adr & 0x800000) return copro_data_[adr & (copro_data_.size() - 1)];
    if (adr & 0x400000) return buffer_[adr & 0x7fff];
    return 0;
}

void TgpBoard::mem_w(uint32_t adr, uint32_t v) {
    if (adr & 0x400000) buffer_[adr & 0x7fff] = v;
}

void TgpBoard::buffer_w(uint32_t byte_offset, uint32_t data, uint32_t mask) {
    uint32_t &w = buffer_[(byte_offset >> 2) & 0x7fff];
    w = (w & ~mask) | (data & mask);
}

void TgpBoard::push_geo(uint32_t data) {
    const uint32_t i = geo_write_start_ / 4;
    if (i >= 0x8000) throw TgpFatal("geometrizer push past the end of buffer RAM");
    buffer_[i] = data;
    geo_write_start_ += 4;
}

void TgpBoard::geoctl_w(uint32_t data) { geoctl_ = data; }

void TgpBoard::geo_prg_w(uint32_t data) {
    if (geoctl_ & 0x80000000) return; // geometrizer program upload: its own microcode, not modelled
    push_geo(data);
}

uint32_t TgpBoard::geo_r(uint32_t offset) const {
    const uint32_t address = offset * 4;
    if (address == 0x2008) return geo_write_start_;
    if (address == 0x3008) return geo_read_start_;
    return 0;
}

void TgpBoard::geo_w(uint32_t offset, uint32_t data) {
    const uint32_t address = offset * 4;
    if (address < 0x1000) {
        if (data & 0x80000000) {
            push_geo((data & 0x800fffff) | (((address >> 4) & 0x3f) << 23));
        } else if ((address & 0xf) == 0) {
            uint32_t r = (data & 0x000fffff) | (((address >> 4) & 0x3f) << 23);
            if (((address >> 4) & 0xc0) && ((address >> 4) & 0x3f) == 1) r |= ((address >> 10) & 3) << 29;
            push_geo(r);
        }
    } else if (address == 0x1008) {
        geo_write_start_ = data & 0xfffff;
    } else if (address == 0x3008) {
        geo_read_start_ = data & 0xfffff;
    } else {
        throw TgpFatal("geometrizer write to an unknown register");
    }
}

} // namespace rt

namespace rt {

namespace {
bool buffer_ram(uint32_t a) { return a >= 0x00900000 && a <= 0x0097ffff; }
} // namespace

bool TgpBoardModel::claims(uint32_t a) const {
    return (a >= 0x00800000 && a <= 0x00807fff) || (a >= 0x00884000 && a <= 0x00887fff) ||
           (a >= 0x00880000 && a <= 0x00883fff) || buffer_ram(a) || (a >= 0x00980000 && a <= 0x0098000b);
}

DeviceModel::Check TgpBoardModel::read(uint32_t addr, uint32_t mask, uint32_t &value) {
    if (buffer_ram(addr)) {
        ++buffer_reads;
        value = b_.buffer_r(addr & 0x1ffff);
        return Measured;
    }
    if (addr >= 0x00884000 && addr <= 0x00887fff) {
        // MAME's copro_fifo_r is a 32-bit handler: a 16-bit read still pops a
        // whole word and the bus keeps the lane asked for (races do this).
        (void)mask;
        ++fifo_words;
        value = b_.fifo_r();
        return Strict;
    }
    if (addr >= 0x00800000 && addr <= 0x00803fff) { value = b_.geo_r((addr - 0x00800000) >> 2); return Strict; }
    if (addr >= 0x00804000 && addr <= 0x00807fff) { value = 0xffffffffu; return Strict; } // geo_prg_r
    if (addr == 0x00980000) { value = b_.coproctl_r(); return Strict; }
    if (addr == 0x00980004) {
        ++status_reads;
        value = b_.fifo_out_empty() ? 1 : 0;
        return Measured;
    }
    value = 0; // write-only ports (function port, geoctl) read as unmapped
    return Strict;
}

void TgpBoardModel::write(uint32_t addr, uint32_t data, uint32_t mask) {
    if (buffer_ram(addr)) { b_.buffer_w(addr & 0x1ffff, data, mask); return; }
    if (addr == 0x00980000) { b_.coproctl_w(data, mask); return; }
    // The other ports are MAME 32-bit handlers without a mask: a narrower
    // write reaches them as the whole dword, lanes not written being zero.
    data &= mask;
    if (addr >= 0x00884000 && addr <= 0x00887fff) b_.fifo_w(data);
    else if (addr >= 0x00880000 && addr <= 0x00883fff) b_.function_port_w((addr - 0x00880000) >> 2, data);
    else if (addr >= 0x00804000 && addr <= 0x00807fff) b_.geo_prg_w(data);
    else if (addr >= 0x00800000 && addr <= 0x00803fff) b_.geo_w((addr - 0x00800000) >> 2, data);
    else if (addr == 0x00980008) b_.geoctl_w(data);
    // 0x00980004 (read-only status) ignores writes
}

void TgpBoardModel::measured_mismatch(uint32_t addr, uint32_t ours, uint32_t mame) {
    char b[96];
    std::snprintf(b, sizeof b, "%08x: ours %08x, MAME %08x (TGP at %llu instructions)", addr, ours, mame,
                  (unsigned long long)b_.tgp_instructions());
    static FILE *log = std::getenv("M2NATIVE_MISMATCH_LOG") ? std::fopen(std::getenv("M2NATIVE_MISMATCH_LOG"), "w") : nullptr;
    if (log) std::fprintf(log, "%s\n", b);
    if (buffer_ram(addr)) {
        if (!buffer_mismatch++) first_buffer_mismatch = b;
    } else {
        if (!status_mismatch++) first_status_mismatch = b;
    }
}

} // namespace rt
