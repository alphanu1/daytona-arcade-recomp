// Optional JSONL measurements shared by software and hardware capture tools.
#pragma once
#include "runtime/game_loop.h"
#include <fstream>

namespace tools {
class SceneryLog {
public:
    explicit SceneryLog(const std::string &path) {
        if (path.empty()) return;
        stream_.open(path);
        if (!stream_) throw std::runtime_error("cannot open scenery log: " + path);
    }
    void attach(rt::GameLoop &game) { game.board().scenery()->diagnostics = stream_.is_open(); }
    void write(rt::GameLoop &game) {
        if (!stream_.is_open()) return;
        auto &board = game.board();
        const auto &m = board.scenery()->measured;
        const unsigned count = board.read_byte(0x5016c0);
        stream_ << "{\"frame\":" << board.frame() << ",\"budget\":" << board.read_dword(0x5010f4)
                << ",\"cost_peak\":" << m.cost_peak << ",\"budget_checks\":" << m.budget_checks
                << ",\"budget_rejections\":" << m.budget_rejections << ",\"list_updates\":" << m.list_updates
                << ",\"original_cells\":" << m.original_cells << ",\"selected_cells\":" << m.selected_cells
                << ",\"exclusion_mask\":" << m.exclusion_mask << ",\"direction_mask\":" << m.direction_mask
                << ",\"course\":" << unsigned(board.read_byte(0x501460))
                << ",\"cell_count\":" << count << ",\"cells\":[";
        for (unsigned i = 0; i < std::min(count, 63u); ++i) {
            if (i) stream_ << ',';
            stream_ << unsigned(board.read_byte(0x5016c1 + i));
        }
        stream_ << "]}\n";
        if (!stream_) throw std::runtime_error("cannot write scenery log");
    }
private:
    std::ofstream stream_;
};
} // namespace tools
