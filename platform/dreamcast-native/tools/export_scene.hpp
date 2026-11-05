// Host only: translate a 2D reference state to source art and native rectangles.
// No board registers or machine runtime are needed by the resulting scene.
#pragma once
#include "dreamcast_assets.h"
#include "system24.hpp"
#include "../include/title.h"

namespace daytona::native {
// No-input first-title oracle. The reference supplies evidence, never target
// frame data. Use the same native pose function against immutable source art.
struct TitleCheck {
    std::vector<int16_t> motion;
    std::vector<uint64_t> hashes;
    std::vector<uint32_t> background, foreground;
    void load(const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
        if (bytes.size() != 88) throw std::runtime_error("expected 44 prepared title positions");
        for (size_t i = 0; i < bytes.size(); i += 2)
            motion.push_back(int16_t(unsigned(bytes[i]) | unsigned(bytes[i + 1]) << 8));
        if (motion.back() != 0) throw std::runtime_error("title motion does not settle at zero");
    }
    void sample(const rt::Video &video, uint64_t frame) {
        if (frame >= 2948 && frame <= 3362) {
            if (!video.gpu_polys().empty()) throw std::runtime_error("3D in title oracle range");
            hashes.push_back(video.screen_hash());
        }
        if (frame == 3120) {
            for (unsigned y = 0; y < 384; ++y) for (unsigned x = 0; x < 512; ++x) {
                background.push_back(video.system24_pen(video.system24_pixel(2, x, y)));
                const unsigned pen = video.system24_pixel(0, x, y);
                foreground.push_back((pen & 15) ? video.system24_pen(pen) : 0);
            }
        }
    }
    void check() const {
        if (hashes.size() != 415 || background.empty())
            throw std::runtime_error("title oracle requires at least 3362 no-input frames");
        for (size_t tick = 0; tick < hashes.size(); ++tick) {
            const auto pose = title_pose(motion.data(), motion.size(), tick);
            uint64_t hash = 0xcbf29ce484222325ULL;
            for (int y = 0; y < 384; ++y) for (int x = 0; x < 496; ++x) {
                uint32_t pixel = background[y * 512 + x];
                const int sx = y < 304 ? x - pose.logo_x : x;
                if (sx >= 0 && sx < 512 && (y < 304 || (y >= 328 && pose.footer) ||
                    (y >= 304 && y < 328 && x >= 96 && x < 400 && pose.prompt))) {
                    const uint32_t front = foreground[y * 512 + sx];
                    if (front >> 24) pixel = front;
                }
                for (unsigned b = 0; b < 4; ++b) {
                    hash ^= (pixel >> (8 * b)) & 255;
                    hash *= 0x100000001b3ULL;
                }
            }
            if (hash != hashes[tick])
                throw std::runtime_error("native title differs at reference frame " + std::to_string(tick + 2948));
        }
        std::printf("native_title_oracle=PASS frames=%zu intro/footer/prompt exact before VQ\n", hashes.size());
    }
};

// Host-only change inventory. Bounds/timestamps are evidence, not animation data.
struct SceneProbe {
    std::vector<uint32_t> previous;
    void sample(const rt::Video &video, uint64_t frame) {
        unsigned left = rt::Video::W, top = rt::Video::H, right = 0, bottom = 0, changed = 0;
        unsigned prompt_ink = 0;
        const auto &screen = video.screen();
        for (unsigned y = 0; y < rt::Video::H; ++y) for (unsigned x = 0; x < rt::Video::W; ++x) {
            const size_t at = y * rt::Video::W + x;
            if (previous.empty() || screen[at] != previous[at]) {
                ++changed;
                left = std::min(left, x); top = std::min(top, y);
                right = std::max(right, x + 1); bottom = std::max(bottom, y + 1);
            }
            if (x >= 96 && x < 400 && y >= 304 && y < 328 && (screen[at] & 0xffffff) != 0xffffff)
                ++prompt_ink;
        }
        if (changed) std::printf("scene_probe frame=%llu polygons=%zu hash=%016llx changed=%u bounds=%u,%u,%u,%u prompt_ink=%u h0=%u v0=%u row100=%u row310=%u\n",
            (unsigned long long)frame, video.gpu_polys().size(), (unsigned long long)video.screen_hash(),
            changed, left, top, right, bottom, prompt_ink, video.system24_word(0x5000),
            video.system24_word(0x5004), video.system24_word(0x4000 + 100), video.system24_word(0x4000 + 310));
        previous = screen;
    }
};

inline void export_scene(const std::filesystem::path &directory, const rt::Video &video) {
    if (!video.gpu_polys().empty())
        throw std::runtime_error("2D scene export refuses a frame with 3D geometry");
    tools::dreamcast::export_system24(directory, video);
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(directory / "rectangles.txt");
    out << "# layer opaque x y sx sy width height; source art, painter order\n";
    std::vector<uint32_t> composed(rt::Video::W * rt::Video::H, video.system24_pen(0));
    for (bool front : {false, true}) system24_rectangles(video, front, [&](const auto &r) {
        out << r.layer << ' ' << r.opaque << ' ' << r.x << ' ' << r.y << ' '
            << r.sx << ' ' << r.sy << ' ' << r.width << ' ' << r.height << '\n';
        for (unsigned y = 0; y < r.height; ++y) for (unsigned x = 0; x < r.width; ++x) {
            const unsigned pen = video.system24_pixel(r.layer, r.sx + x, r.sy + y);
            if (r.opaque || (pen & 15))
                composed[(r.y + y) * rt::Video::W + r.x + x] = video.system24_pen(pen);
        }
    });
    out.close();
    if (composed != video.screen())
        throw std::runtime_error("native 2D rectangles differ from the CPU reference screen");
    tools::dreamcast::write_tga(directory / "reference.tga",
        {rt::Video::W, rt::Video::H, false, composed});
    std::printf("native_2d_source_geometry=exact (static layout only; no gameplay imported)\n");
}
}
