// The i960's side of the Model 2 geometry hardware: the TGP's FIFOs and
// control register, the geometrizer's command port, and buffer RAM, which
// all three write (MAME model2.cpp: copro_*, geo_*, push_geo_data). The TGP
// itself is the recompiled program (rt::tgpgen), run only when the i960
// needs its output: clockless, it returns when its input FIFO is empty.
#pragma once

#include "runtime/tgp.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace rt {

class TgpBoard : public TgpBus {
public:
    // tables: copro_tgp_tables (0x40000 bytes); copro_data: 0x800000 bytes.
    TgpBoard(const std::vector<uint8_t> &tables, const std::vector<uint8_t> &copro_data);

    // i960 accesses (dword offsets within each range, MAME's handlers).
    void function_port_w(uint32_t offset, uint32_t data);  // 0x00880000
    void fifo_w(uint32_t data);                           // 0x00884000
    uint32_t fifo_r();                                    // 0x00884000
    bool fifo_out_empty();                                // 0x00980004 (after running the TGP)
    void coproctl_w(uint32_t data, uint32_t mask);        // 0x00980000
    uint32_t coproctl_r() const { return coproctl_; }
    void geo_w(uint32_t offset, uint32_t data);           // 0x00800000
    uint32_t geo_r(uint32_t offset) const;
    void geo_prg_w(uint32_t data);                        // 0x00804000
    void geoctl_w(uint32_t data);                         // 0x00980008
    uint32_t buffer_r(uint32_t byte_offset) const { return buffer_[(byte_offset >> 2) & 0x7fff]; }
    void buffer_w(uint32_t byte_offset, uint32_t data, uint32_t mask);

    const uint32_t *buffer() const { return buffer_; }
    uint32_t *buffer_data() { return buffer_; }             // the geometrizer reads its display list here
    uint32_t geo_read_start() const { return geo_read_start_; }
    uint64_t tgp_instructions() const { return tgp_.count; }
    bool booted() const { return booted_; }

    // TgpBus (the TGP's side)
    bool fifo_pop(uint32_t &v) override;
    void fifo_push(uint32_t v) override { out_.push_back(v); }
    uint32_t mem_r(uint32_t adr) override;
    void mem_w(uint32_t adr, uint32_t v) override;

private:
    void run_tgp();
    void push_geo(uint32_t data);

    Tgp tgp_;
    std::vector<uint32_t> tables_, copro_data_;
    std::deque<uint32_t> in_, out_;
    std::vector<uint32_t> upload_;
    uint32_t coproctl_ = 0, geoctl_ = 0, geo_write_start_ = 0, geo_read_start_ = 0;
    bool booted_ = false;
    uint32_t buffer_[0x8000];
};

} // namespace rt

#include "runtime/m2_replay_bus.h"

#include <string>

namespace rt {

// TgpBoard behind the replay bus: the i960's accesses to the geometry ports,
// TGP FIFOs and buffer RAM go to the native model. TGP output words are
// strict (must equal MAME's). Buffer RAM reads and FIFO-status polls depend
// on how far the TGP has run relative to the i960, so they are measured.
class TgpBoardModel : public DeviceModel {
public:
    explicit TgpBoardModel(TgpBoard &b) : b_(b) {}
    bool claims(uint32_t a) const override;
    Check read(uint32_t addr, uint32_t mask, uint32_t &value) override;
    void write(uint32_t addr, uint32_t data, uint32_t mask) override;
    void measured_mismatch(uint32_t addr, uint32_t ours, uint32_t mame) override;
    const uint8_t *region(uint32_t base) const override {
        return base == 0x00900000 ? reinterpret_cast<const uint8_t *>(b_.buffer()) : nullptr;
    }

    uint64_t fifo_words = 0, buffer_reads = 0, status_reads = 0;
    uint64_t buffer_mismatch = 0, status_mismatch = 0;
    std::string first_buffer_mismatch, first_status_mismatch;

private:
    TgpBoard &b_;
};

} // namespace rt
