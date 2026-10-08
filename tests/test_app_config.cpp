// Persisted scenery settings round-trip through the same format as launcher.ini.
// Streams keep tests isolated from the user's live profile.
#include "app/config.h"
#include <cstdio>
#include <sstream>

int main() {
    int failures = 0;
    auto check = [&](bool value, const char *message) {
        if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    app::Config original;
    check(original.draw_budget == 0 && original.aspect_ratio() == 0, "new installs preserve original defaults");
    original.aspect = "21:9";
    original.draw_distance = 2;
    original.draw_budget = 5000;
    std::ostringstream output;
    original.write(output);
    app::Config loaded;
    std::istringstream input(output.str());
    loaded.read(input);
    check(loaded.aspect_ratio() == 21.0 / 9.0 && loaded.draw_budget == 5000 && loaded.draw_distance == 2,
          "custom budget independent of aspect and distance survives save/load");
    std::istringstream automatic("draw_budget=0\naspect=21:9\n");
    loaded.read(automatic);
    check(loaded.draw_budget == 0 && loaded.aspect_ratio() == 21.0 / 9.0, "return to Automatic survives load");
    loaded.aspect = "32:9";
    check(loaded.aspect_ratio() == 21.0 / 9.0, "saved future aspect is capped at the supported view");
    for (auto value : {"-1", "1000001", "4294967296", "123oops", ""}) {
        std::istringstream bad(std::string("draw_budget=") + value + "\n");
        loaded.draw_budget = 9000;
        loaded.read(bad);
        check(loaded.draw_budget == 0, "invalid budget falls back to Automatic");
    }
    std::istringstream legacy("aspect=16:9\ndraw_distance=1\n");
    app::Config old;
    old.read(legacy);
    check(old.draw_budget == 0 && old.draw_distance == 1, "old settings use Automatic");
    for (auto value : {"nan:9", "21:inf", "21:0", "-21:9"}) {
        loaded.aspect = value;
        check(loaded.aspect_ratio() == 0, "non-finite and invalid aspect rejected");
    }
    return failures ? 1 : 0;
}
