// TEST ORACLE ONLY. Never linked into the game.
//
// MAME's i960 instruction interpreter (BSD-3-Clause, transplanted; see
// i960_ref.cpp) over the runtime's Cpu context. m2replay uses it to check the
// runtime and the device replay against MAME without the recompiler.
#pragma once

#include "runtime/cpu.h"

namespace rt {

class I960Ref : public Cpu {
public:
    using Cpu::Cpu;

    void execute_one(); // one instruction, as MAME's execute_run loop body

    // Kept so the transplanted code compiles unchanged; never set.
    bool m_stalled = false;
    struct {
        uint32_t t1 = 0, t2 = 0;
        int index = 0, size = 0;
        bool burst_mode = false;
        bool iswriteop = false;
    } m_stall_state;
    int m_icount = 0; // MAME cycle estimate; unused

private:
    uint32_t get_ea(uint32_t opcode);
    uint32_t get_1_ri(uint32_t opcode);
    uint32_t get_2_ri(uint32_t opcode);
    uint64_t get_2_ri64(uint32_t opcode);
    void set_ri(uint32_t opcode, uint32_t val);
    void set_ri2(uint32_t opcode, uint32_t val, uint32_t val2);
    void set_ri64(uint32_t opcode, uint64_t val);
    double get_1_rif(uint32_t opcode);
    double get_2_rif(uint32_t opcode);
    void set_rif(uint32_t opcode, double val);
    double get_1_rifl(uint32_t opcode);
    double get_2_rifl(uint32_t opcode);
    void set_rifl(uint32_t opcode, double val);
    uint32_t get_1_ci(uint32_t opcode);
    uint32_t get_2_ci(uint32_t opcode);
    uint32_t get_disp(uint32_t opcode);
    uint32_t get_disp_s(uint32_t opcode);
    void cmp_s(int32_t v1, int32_t v2);
    void cmp_u(uint32_t v1, uint32_t v2);
    void concmp_s(int32_t v1, int32_t v2);
    void concmp_u(uint32_t v1, uint32_t v2);
    void cmp_d(double v1, double v2);
    void bxx(uint32_t opcode, int mask);
    void bxx_s(uint32_t opcode, int mask);
    void fxx(uint32_t opcode, int mask);
    void test(uint32_t opcode, int mask);
    double round_to_int(double val);
    void execute_op(uint32_t opcode);
    void burst_stall_save(uint32_t t1, uint32_t t2, int index, int size, bool iswriteop);
};

} // namespace rt
