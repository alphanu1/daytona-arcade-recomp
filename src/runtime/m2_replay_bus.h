// Model 2 (original, `daytona93`) memory bus for the M1 lockstep harness
// (design doc, M1 plan): RAM and ROM as flat arrays, every tapped device
// range answered from a MAME trace, every device write checked against it.
#pragma once

#include "runtime/cpu.h"
#include "runtime/raster.h"
#include "trace/trace.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

// First difference from the MAME trace, thrown out of the bus.
struct Divergence : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A device the runtime models natively instead of replaying (M2: the TGP and
// buffer RAM). Its reads are either strict (must equal MAME's recorded value)
// or measured (a mismatch is counted and MAME's value used, for behaviour that
// depends on how far one chip has run relative to another).
class DeviceModel {
public:
    virtual ~DeviceModel() = default;
    virtual bool claims(uint32_t addr) const = 0;
    enum Check { Strict, Measured };
    virtual Check read(uint32_t addr, uint32_t mask, uint32_t &value) = 0;
    virtual void write(uint32_t addr, uint32_t data, uint32_t mask) = 0; // after the trace check
    virtual void measured_mismatch(uint32_t addr, uint32_t ours, uint32_t mame) = 0;
    // A region the model holds (buffer RAM), for the sample hashes; nullptr if not.
    virtual const uint8_t *region(uint32_t base) const { (void)base; return nullptr; }
    uint64_t region_samples = 0, region_mismatch = 0; // measured, as above
};

class M2ReplayBus : public Bus {
public:
    // program: 0x200000 bytes at 0x00000000; main_data: 0x2000000 at 0x02000000.
    M2ReplayBus(std::vector<uint8_t> program, std::vector<uint8_t> main_data, const std::string &trace_path);

    void attach(const Cpu *core) { core_ = core; }
    void set_model(DeviceModel *m) { model_ = m; }
    // A dword of RAM, ROM or untapped register space, bypassing the trace.
    uint32_t peek(uint32_t addr);
    // The memories the 3D rasterizer reads (palette, colour translation,
    // luma, texture RAM), current as of now.
    VideoMem video_mem() const;

    uint32_t fetch(uint32_t addr) override;
    uint8_t read_byte(uint32_t addr) override;
    uint16_t read_word(uint32_t addr) override;
    uint32_t read_dword(uint32_t addr) override;
    void write_byte(uint32_t addr, uint8_t data) override;
    void write_word(uint32_t addr, uint16_t data) override;
    void write_dword(uint32_t addr, uint32_t data) override;
    uint16_t flags(uint32_t addr) override { return page(addr).burst ? Cpu::BURST : 0; }

    uint64_t epochs_matched() const { return epoch_; }
    uint64_t events_matched() const { return events_; }
    bool trace_done() const { return done_; }
    // True when every recorded (non-stalled) event has been matched.
    bool all_events_consumed();
    std::string where() const; // epoch / event position, for reports

private:
    // Tex: model2o texture RAM. MAME's tex0_w/tex1_w keep only 16 bits of
    // each 32-bit write, packing two writes per stored dword; reads see the
    // packed store directly.
    enum Kind : uint8_t { Unmapped, Rom, Ram, Device, Tex };
    struct Page {
        Kind kind = Unmapped;
        bool burst = false;        // MAME maps the range with .flags(i960_cpu_device::BURST)
        uint8_t *base = nullptr;   // Rom/Ram: host address of guest (page start)
    };
    static constexpr unsigned kPageBits = 12; // 4 KiB pages
    Page &page(uint32_t addr) { return pages_[addr >> kPageBits]; }
    void map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror = 0, bool burst = true);
    void map_device(uint32_t start, uint32_t end);
    void set_burst(uint32_t start, uint32_t end);

    uint32_t device_read(uint32_t addr, uint32_t mask);
    void device_write(uint32_t addr, uint32_t data, uint32_t mask);
    const trace::Access &next_event(bool write, uint32_t addr, uint32_t mask);
    void end_of_epoch_check();
    uint8_t *sparse(uint32_t addr);
    void tex_write(const Page &p, uint32_t addr, uint32_t lane_data);

    std::vector<uint8_t> program_, main_data_;
    std::vector<uint8_t> ram_, work_, buffer_, cpuctl_, backup_, tile_, chr_, palette_, xlat_, tex0_, tex1_, luma_,
        fb_a_, fb_b_;
    std::vector<Page> pages_;
    std::unordered_map<uint32_t, std::unique_ptr<std::array<uint8_t, 1u << kPageBits>>> sparse_;

    trace::Reader reader_;
    trace::Epoch cur_;
    size_t pos_ = 0;       // next event in cur_ (stalled events skipped)
    uint64_t epoch_ = 0;   // epochs fully matched
    uint64_t events_ = 0;  // events matched in total
    bool done_ = false;
    const Cpu *core_ = nullptr;
    DeviceModel *model_ = nullptr;
};

} // namespace rt
