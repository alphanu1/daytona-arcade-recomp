// build/daytona: the one file to start. Each ROM set is its own build of the
// game, both in this folder: daytona-93 (Deluxe '93, daytona93) and
// daytona-rev (Revision A, 1994, daytona). This starts the right one with the
// same arguments: with --rom, the game for the set that archive holds; else
// Deluxe '93's (its launcher remembers the last archive, and hands over to
// Revision A's game for that set), or Revision A's if it is the only one
// built. The launcher screen, settings and saves are the games' own.
#include "runtime/rom_import.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> // Windows: the WinMain entry point a WIN32 (GUI) program links against

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
constexpr const char *kExe = ".exe";
#else
constexpr const char *kExe = "";
#endif

std::string beside(const char *name) {
    const char *base = SDL_GetBasePath();
    return std::string(base ? base : "") + name + kExe;
}

bool built(const std::string &path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace

int main(int argc, char **argv) {
    const std::string deluxe = beside("daytona-93"), revision_a = beside("daytona-rev");
    std::string game = built(deluxe) ? deluxe : revision_a;
    for (int i = 1; i + 1 < argc; i++)
        if (!std::strcmp(argv[i], "--rom")) { // an archive named: its set's game
            try {
                const auto sets = rt::rom_sets_in(argv[i + 1]);
                if (!sets.empty()) {
                    const std::string &g = sets.front() == "daytona" ? revision_a : deluxe;
                    if (built(g)) game = g;
                }
            } catch (const std::exception &) {
                // not an archive it can read: the game says why
            }
        }
    if (!built(game)) {
        const char *message = "No game is built yet. Put your ROM set in roms/ (daytona93.zip or daytona.zip, or .7z) "
                              "and run setup again.";
        std::fprintf(stderr, "daytona: %s\n", message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Daytona USA", message, nullptr);
        return 1;
    }
    std::vector<const char *> args{game.c_str()};
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    args.push_back(nullptr);
#ifndef _WIN32
    execv(game.c_str(), const_cast<char *const *>(args.data())); // the game in this process: the terminal waits for it
    std::fprintf(stderr, "daytona: could not start %s\n", game.c_str());
    return 1;
#else
    if (SDL_Process *p = SDL_CreateProcess(args.data(), false)) {
        SDL_DestroyProcess(p); // it carries on alone
        return 0;
    }
    std::fprintf(stderr, "daytona: could not start %s (%s)\n", game.c_str(), SDL_GetError());
    return 1;
#endif
}
