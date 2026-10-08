// Policy, list bounds and observer isolation without ROM data.
#include "runtime/enhance.h"
#include "runtime/scenery.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char *message) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Memory : rt::Bus {
    std::map<uint32_t, uint8_t> bytes;
    rt::SceneryState state;
    int writes = 0;
    rt::SceneryState *scenery() override { return &state; }
    uint32_t fetch(uint32_t a) override { return read_dword(a); }
    uint8_t read_byte(uint32_t a) override { return bytes[a]; }
    uint16_t read_word(uint32_t a) override { return uint16_t(read_byte(a) | unsigned(read_byte(a + 1)) << 8); }
    uint32_t read_dword(uint32_t a) override { return read_word(a) | uint32_t(read_word(a + 2)) << 16; }
    void write_byte(uint32_t a, uint8_t v) override { bytes[a] = v; ++writes; }
    void write_word(uint32_t a, uint16_t v) override { write_byte(a, uint8_t(v)); write_byte(a + 1, uint8_t(v >> 8)); }
    void write_dword(uint32_t a, uint32_t v) override { write_word(a, uint16_t(v)); write_word(a + 2, uint16_t(v >> 16)); }
    void list(std::initializer_list<uint8_t> cells) {
        write_byte(0x5016c0, uint8_t(cells.size()));
        unsigned i = 0;
        for (auto cell : cells) write_byte(0x5016c1 + i++, cell);
    }
    std::vector<uint8_t> list() {
        std::vector<uint8_t> result;
        for (unsigned i = 0; i < read_byte(0x5016c0); ++i) result.push_back(read_byte(0x5016c1 + i));
        return result;
    }
};
}

int main() {
    check(rt::automatic_scenery_budget(496, 0) == 5000, "native default");
    check(rt::automatic_scenery_budget(614, 0) == 6190, "16:10 ceiling");
    check(rt::automatic_scenery_budget(682, 0) == 6875, "16:9 allowance");
    check(rt::automatic_scenery_budget(896, 0) == 9033, "21:9 ceiling");
    check(rt::automatic_scenery_budget(896, 2) == 15000, "distance floor");
    check(rt::automatic_scenery_budget(896, 1) == 10000, "do not multiply allowances");
    check(rt::automatic_scenery_budget(1366, 0) == 13771, "32:9 headless allowance");
    uint32_t parsed = 77;
    for (auto text : {"", "-1", "1x", "4294967296", "1000001", " 5"})
        check(!rt::parse_scenery_budget(text, parsed) && parsed == 77, "reject invalid budgets without changing value");
    check(rt::parse_scenery_budget("0", parsed) && parsed == 0, "Automatic parses");
    check(rt::parse_scenery_budget("1000000", parsed) && parsed == 1000000, "maximum parses");

    Memory bus;
    rt::Cpu cpu(&bus);
    cpu.m_r[8] = 122;
    bus.list({122, 138, 123});
    bus.writes = 0;
    rt::hook_draw_list(cpu);
    check(bus.writes == 0, "enhancements off does not write RAM");
    const auto original = bus.list();
    bus.state.width = 1366;
    // Exclude one otherwise eligible neighbour, while the directional mask
    // contains only the centre. Widescreen must widen direction, not exclusions.
    cpu.m_r[13] = 1u << 13;
    cpu.m_r[9] = 1u << 12;
    rt::hook_draw_list(cpu);
    auto cells = bus.list();
    check(std::equal(original.begin(), original.end(), cells.begin()), "preserve original prefix/order");
    check(cells.size() == 25, "original cells stay even if excluded by a supplied mask");
    bus.list({122});
    rt::hook_draw_list(cpu);
    cells = bus.list();
    check(cells.size() == 24 && std::find(cells.begin(), cells.end(), 123) == cells.end(), "retain course exclusions");
    check(bus.read_dword(0x5010f4) == 13771, "apply headless automatic budget");
    bus.state.custom_budget = 1;
    rt::Enhance::draw_distance = 2;
    rt::hook_draw_list(cpu);
    check(bus.read_dword(0x5010f4) == 1, "custom overrides aspect and distance");
    bus.state.custom_budget = 0;
    rt::hook_draw_list(cpu);
    check(bus.read_dword(0x5010f4) == 15000, "reset Automatic");
    rt::Enhance::draw_distance = 0;
    bus.state.width = 496;
    bus.list({122});
    rt::hook_draw_list(cpu);
    check(bus.read_dword(0x5010f4) == 5000 && bus.list() == std::vector<uint8_t>{122}, "return to original view");

    bus.state.width = 1366;
    cpu.m_r[13] = 0;
    for (int car = 0; car < 256; ++car) {
        for (int distance = -2; distance <= 2; ++distance) {
            rt::Enhance::draw_distance = distance;
            cpu.m_r[8] = uint32_t(car);
            bus.list({uint8_t(car)});
            rt::hook_draw_list(cpu);
            cells = bus.list();
            const int radius = distance < 0 ? distance + 2 : distance == 2 ? 3 : 2;
            const auto unique = std::set<uint8_t>(cells.begin(), cells.end());
            check(unique.size() == cells.size() && cells.size() <= 49, "bounded unique list across entire course grid");
            for (auto cell : cells)
                check(std::abs(int(cell & 15) - (car & 15)) <= radius &&
                      std::abs(int(cell >> 4) - (car >> 4)) <= radius, "no row wrapping or range extension");
        }
    }
    rt::Enhance::draw_distance = 0;
    Memory second;
    rt::Cpu other(&second);
    second.list({0}); second.writes = 0;
    rt::hook_draw_list(other);
    check(second.writes == 0, "one board's width and budget do not leak to another");
    bus.state.diagnostics = true;
    bus.state.measured = {};
    bus.writes = 0;
    cpu.m_r[4] = 5000; cpu.m_r[5] = 5000;
    rt::hook_scenery_budget_check(cpu);
    cpu.m_r[5] = 5001;
    rt::hook_scenery_budget_check(cpu);
    cpu.m_r[4] = 6000;
    rt::hook_scenery_cost(cpu);
    check(bus.state.measured.budget_checks == 2 && bus.state.measured.budget_rejections == 1 &&
          bus.state.measured.cost_peak == 6000 && bus.writes == 0, "observers follow unsigned strict comparison without writes");
    return failures ? 1 : 0;
}
