// Apply the playable game's video policy at a frame boundary. Capture tools
// keep their individual controls for original-output comparisons.
#pragma once
#include "app/config.h"
#include "runtime/game_loop.h"

namespace app {
inline void apply_video_settings(rt::GameLoop &game, const Config &cfg, bool hardware_available) {
    game.set_aspect(cfg.aspect_ratio());
    game.set_hud_edges(cfg.hud_edges);
    game.set_frame_skip(cfg.draw_mode);
    auto &video = game.board().video();
    video.set_external_3d(cfg.renderer == "hardware" && hardware_available, true);
    video.panorama().original = game.screen_width() > rt::GameLoop::kWidth;
    game.set_stretch_backdrop(false);
    rt::GameLoop::set_draw_distance(cfg.draw_distance);
    game.set_draw_budget(cfg.draw_budget);
}
} // namespace app
