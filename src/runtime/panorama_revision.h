// Read-only game addresses for the original panorama observer. The artwork
// tables and tile-register writes are shared; these RAM/descriptor locations
// differ. Verified against each local ROM; see docs/original-panorama.md.
#pragma once
#include <cstdint>
#include <string_view>

namespace rt {
struct PanoramaRevision {
    uint32_t phase, selector, descriptor_base;
    uint32_t descriptor(unsigned course) const {
        return course < 3 ? descriptor_base + (course + 1) * 0x20 : 0;
    }
};

inline const PanoramaRevision *panorama_revision(std::string_view romset) {
    static constexpr PanoramaRevision revision_a{0x5fe11a, 0x5fe5e4, 0x2600000};
    static constexpr PanoramaRevision deluxe_93{0x53e11a, 0x53e5d4, 0x2800000};
    if (romset == "daytona") return &revision_a;
    if (romset == "daytona93") return &deluxe_93;
    return nullptr;
}
} // namespace rt
