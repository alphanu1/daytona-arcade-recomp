#include "app/frame_batch.h"
#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
    for (int mode = 0; mode < 3; ++mode) for (uint64_t first = 0; first < 12; ++first)
        for (unsigned batch = 1; batch <= 4; ++batch) {
            int original_picture = -1, last_picture = -1, draws = 0;
            for (unsigned i = 0; i < batch; ++i) {
                const uint64_t frame = first + i;
                if (frame % unsigned(mode + 1) == 0) {
                    original_picture = int(frame);
                    if (app::keep_software_picture(frame, batch - i, mode)) { last_picture = int(frame); ++draws; }
                }
            }
            check(last_picture == original_picture, "catch-up changed the selected picture");
            check(draws <= 1, "catch-up rendered a superseded picture");
        }
    check(app::keep_software_picture(0, 2, 2), "keep early picture when final update is not scheduled to draw");
    check(!app::keep_software_picture(0, 4, 2), "discard early picture when a later scheduled picture replaces it");
    std::puts("PASS: last scheduled picture in all draw modes and batch sizes");
}
