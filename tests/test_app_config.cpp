// Persisted settings round-trip through the same format as launcher.ini.
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
    original.aspect = "32:9";
    original.draw_distance = 2;
    original.draw_budget = 5000;
    std::ostringstream output;
    original.write(output);
    app::Config loaded;
    std::istringstream input(output.str());
    loaded.read(input);
    check(loaded.aspect_ratio() == 32.0 / 9.0 && loaded.draw_budget == 5000 && loaded.draw_distance == 2,
          "custom budget independent of aspect and distance survives save/load");
    std::istringstream automatic("draw_budget=0\naspect=21:9\n");
    loaded.read(automatic);
    check(loaded.draw_budget == 0 && loaded.aspect_ratio() == 21.0 / 9.0, "return to Automatic survives load");
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
    for (auto value : {"0", "1"}) {
        app::Config migrated;
        std::istringstream settings(std::string("stretch_backdrop=") + value +
            "\naspect=32:9\nhud_edges=1\ndraw_budget=9000\n");
        migrated.read(settings);
        std::ostringstream saved;
        migrated.write(saved);
        check(saved.str().find("stretch_backdrop") == std::string::npos,
              "obsolete stretch preference is omitted on save");
        app::Config restored;
        std::istringstream reload(saved.str());
        restored.read(reload);
        check(restored.aspect_ratio() == 32.0 / 9.0 && restored.hud_edges && restored.draw_budget == 9000,
              "migrating either stretch preference preserves other settings");
    }
    loaded.aspect = "40:9";
    check(loaded.aspect_ratio() == 32.0 / 9.0, "larger saved aspect is capped at 32:9");
    for (auto value : {"nan:9", "32:inf", "32:0", "-32:9"}) {
        loaded.aspect = value;
        check(loaded.aspect_ratio() == 0, "non-finite and invalid aspect rejected");
    }
    return failures ? 1 : 0;
}
