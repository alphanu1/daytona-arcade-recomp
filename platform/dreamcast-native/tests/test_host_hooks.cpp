// ROM-free checks of the runtime's M2_DC_NATIVE_HOST hooks.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime/cpu.h"
#include "runtime/video.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    // Observation must precede register-cache/stack mutation, including IRQ entry.
    rt::Cpu plain(nullptr), observed(nullptr);
    struct Observation { const rt::Cpu *before; unsigned calls; uint32_t target; int type; } observation{&plain,0,0,0};
    observed.on_call_ctx = &observation;
    observed.on_call = [](void *context, const rt::Cpu &current, uint32_t target, int type) {
        auto &o = *static_cast<Observation *>(context);
        assert(target == o.target && type == o.type);
        assert(current.m_IP == o.before->m_IP && current.m_rcache_pos == o.before->m_rcache_pos);
        assert(!std::memcmp(current.m_r,o.before->m_r,sizeof(current.m_r)));
        ++o.calls;
    };
    for (unsigned i = 0; i < 4; ++i) {
        observation.target = 0x1000+4*i; observation.type = i == 3 ? 7 : 0;
        observed.do_call(observation.target,observation.type,0x8000);
        plain.do_call(observation.target,observation.type,0x8000);
        assert(observation.calls == i+1 && observed.m_IP == plain.m_IP && observed.m_rcache_pos == plain.m_rcache_pos);
        assert(!std::memcmp(observed.m_r,plain.m_r,sizeof(plain.m_r)));
        assert(!std::memcmp(observed.m_rcache,plain.m_rcache,sizeof(plain.m_rcache)));
        assert(!std::memcmp(observed.m_rcache_frame_addr,plain.m_rcache_frame_addr,sizeof(plain.m_rcache_frame_addr)));
    }
    // Remove the simulated IRQ frame without invoking its bus-dependent exit.
    plain.do_ret_0(); observed.do_ret_0();
    observation.calls = 0;
    observed.on_return_ctx = &observation;
    observed.on_return = [](void *context, const rt::Cpu &current, uint32_t source) {
        auto &o = *static_cast<Observation *>(context);
        assert(source == o.target && current.m_IP == o.before->m_IP);
        assert(current.m_rcache_pos == o.before->m_rcache_pos);
        assert(!std::memcmp(current.m_r,o.before->m_r,sizeof(current.m_r)));
        ++o.calls;
    };
    for (unsigned i = 0; i < 3; ++i) {
        observation.target = observed.m_IP;
        plain.do_ret(); observed.do_ret();
        assert(observation.calls == i+1 && observed.m_IP == plain.m_IP);
        assert(!std::memcmp(observed.m_r,plain.m_r,sizeof(plain.m_r)));
    }
    // Raw character decoding must match every decoded tile, including packed
    // nibble/word order, palette selection and wrapping of tile coordinates.
    std::vector<uint8_t> tiles(0x10000), chars(0x80000);
    for (size_t i = 0; i < tiles.size(); ++i) tiles[i] = uint8_t(i * 37 + i / 17);
    for (size_t i = 0; i < chars.size(); ++i) chars[i] = uint8_t(i * 13 + i / 31);
    rt::Video video(tiles.data(), chars.data());
    std::vector<rt::GeoPoly> polys;
    const rt::VideoMem mem{};
    video.screen_update(polys, 0, mem);
    for (int layer = 0; layer < 4; ++layer)
        for (unsigned y = 0; y < 512; ++y)
            for (unsigned x = 0; x < 512; ++x)
                assert(video.system24_pixel(layer, x + 512, y + 512) == video.system24_pixels(layer)[y * 512 + x]);
    std::puts("PASS: call and return observers change nothing, raw/decoded tile parity");
}
