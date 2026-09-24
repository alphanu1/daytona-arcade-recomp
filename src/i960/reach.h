// Recursive-descent reachability over an i960 image (design doc, i960 static
// recompiler, pipeline step 3). Follows direct control flow only; every
// indirect transfer is recorded as a site, since its targets come from the
// MAME harvest or seeds.toml, not from here.
#pragma once

#include "decode.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace i960 {

// Returns the 32-bit word at a guest address, or nullopt if unmapped.
using ReadWord = std::function<std::optional<uint32_t>(uint32_t addr)>;

struct ReachResult {
    std::map<uint32_t, Insn> insns;         // every reachable instruction, by address
    std::vector<uint32_t> indirect_sites;   // bx/balx/callx/calls, by address
    std::vector<uint32_t> stops;            // paths ended on an invalid or non-executable word
    std::vector<uint32_t> unmapped_targets; // direct targets outside the image
};

ReachResult reach(const std::vector<uint32_t> &seeds, const ReadWord &read);

// Seeds from the i960 boot structures, read the way MAME's i960 core reads
// them: reset IP (word 12), interrupt handlers (PRCB+20 -> table + 36 +
// (vector-8)*4, vectors 8-255), system procedures (SAT+152 -> table + 48 +
// 4*i, low two bits masked). Zero entries (unused slots) and entries that
// point outside the image are dropped.
struct BootSeeds {
    uint32_t reset_ip = 0;
    std::vector<uint32_t> interrupt_handlers; // unique
    std::vector<uint32_t> system_procedures;  // unique
    std::vector<uint32_t> all() const;
};
BootSeeds boot_seeds(const ReadWord &read, unsigned max_system_procedures = 260);

} // namespace i960
