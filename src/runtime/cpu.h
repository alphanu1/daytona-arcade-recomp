// i960KB context and runtime services (MAME's semantics, BSD-3-Clause,
// transplanted; see cpu.cpp). Recompiled game code runs on this: registers,
// the register cache, call/return, interrupt entry, memory access. There is
// no instruction interpreter in the runtime; the reference interpreter in
// src/refcore is a test oracle and is never linked into the game.
#pragma once

#include <bit>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace rt {

// Memory as the i960 sees it. Byte addresses, little-endian.
class Bus {
public:
    virtual ~Bus() = default;
    virtual uint32_t fetch(uint32_t addr) = 0; // instruction fetch
    virtual uint8_t read_byte(uint32_t addr) = 0;
    virtual uint16_t read_word(uint32_t addr) = 0;
    virtual uint32_t read_dword(uint32_t addr) = 0;
    virtual void write_byte(uint32_t addr, uint8_t data) = 0;
    virtual void write_word(uint32_t addr, uint16_t data) = 0;
    virtual void write_dword(uint32_t addr, uint32_t data) = 0;

    // Handler flags at an address. MAME's ldl/ldt/ldq (and stores) advance the
    // address only where the region is flagged Cpu::BURST; elsewhere
    // (device FIFOs) they access the same address repeatedly.
    virtual uint16_t flags(uint32_t addr) { (void)addr; return 0; }

    std::pair<uint32_t, uint16_t> read_dword_flags(uint32_t a) { return {read_dword(a), flags(a)}; }
    std::pair<uint8_t, uint16_t> read_byte_flags(uint32_t a) { return {read_byte(a), flags(a)}; }
    uint16_t write_dword_flags(uint32_t a, uint32_t d) { write_dword(a, d); return flags(a); }
    uint16_t write_byte_flags(uint32_t a, uint8_t d) { write_byte(a, d); return flags(a); }
};

// Raised where MAME would fatalerror (unimplemented op, bad operand form...).
struct Fatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum { I960_IRQ0 = 0, I960_IRQ1 = 1, I960_IRQ2 = 2, I960_IRQ3 = 3 };
enum { I960_PFP = 0, I960_SP = 1, I960_RIP = 2, I960_FP = 31 }; // register indices (MAME i960.h)

// MAME integer names and helpers used by the transplanted code.
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
template <typename T, typename U> constexpr T BIT(T x, U n) noexcept { return (x >> n) & T(1); }
constexpr uint64_t mulu_32x32(uint32_t a, uint32_t b) { return uint64_t(a) * uint64_t(b); }

class Cpu {
public:
    static constexpr uint16_t BURST = 0x0001;

    explicit Cpu(Bus *b) : bus(b) {}

    void reset();                               // MAME device_reset
    void execute_set_input(int irqline, int state);
    void check_immediate_irqs();                // take an immediate interrupt if one is waiting

    // Architectural state (MAME's names, so the transplanted code is unchanged).
    Bus *bus;
    uint32_t m_r[0x20]{};
    enum { I960_RCACHE_SIZE = 4 };
    uint32_t m_rcache[I960_RCACHE_SIZE][0x10]{};
    uint32_t m_rcache_frame_addr[I960_RCACHE_SIZE]{};
    int32_t m_rcache_pos = 0;
    double m_fp[4]{};
    uint32_t m_SAT = 0, m_PRCB = 0, m_PC = 0, m_AC = 0, m_IP = 0, m_PIP = 0, m_ICR = 0;
    int m_immediate_irq = 0, m_immediate_vector = 0, m_immediate_pri = 0;
    int8_t m_irq_line_state[4]{};

    // Interrupt observer: called at every interrupt taken (vector, IP, from
    // the pending table or not). The lockstep harness checks these against
    // MAME's log.
    void (*on_take)(void *ctx, int vector, uint32_t ip, bool pending) = nullptr;
    void *on_take_ctx = nullptr;

    // Services called by recompiled code.
    uint32_t i960_read_dword_unaligned(uint32_t address);
    std::pair<uint32_t, uint16_t> i960_read_dword_unaligned_flags(uint32_t address);
    uint16_t i960_read_word_unaligned(uint32_t address);
    void i960_write_dword_unaligned(uint32_t address, uint32_t data);
    uint16_t i960_write_dword_unaligned_flags(uint32_t address, uint32_t data);
    void i960_write_word_unaligned(uint32_t address, uint16_t data);
    void send_iac(uint32_t adr);
    void take_interrupt(int vector, int lvl);
    void check_pending_irqs();
    void do_call(uint32_t adr, int type, uint32_t stack);
    void do_ret_0();
    void do_ret();
    void standard_irq_callback(int, uint32_t) {}
};

// Shims for the transplanted MAME code.
template <typename T> constexpr std::make_signed_t<T> sext(T value, unsigned bits) {
    using S = std::make_signed_t<T>;
    const unsigned shift = sizeof(T) * 8 - bits;
    return S(value << shift) >> shift;
}
inline float u2f(uint32_t v) { return std::bit_cast<float>(v); }
inline uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }
inline double u2d(uint64_t v) { return std::bit_cast<double>(v); }
inline uint64_t d2u(double d) { return std::bit_cast<uint64_t>(d); }
#define DWORD_ALIGNED(a) (((a) & 3) == 0)
#define WORD_ALIGNED(a) (((a) & 1) == 0)

[[noreturn]] void fatalerror(const char *fmt, ...);
inline void logerror(const char *, ...) {}

} // namespace rt
