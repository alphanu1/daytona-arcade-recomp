// The launcher: a Dear ImGui interface in the game's window. Game tab: the
// ROM set (browse, per-file verification), graphics API, fullscreen.
// Controls tab: every arcade control's key, gamepad and wheel/joystick
// binding (press to bind; a joystick axis is moved fully and let go, which
// calibrates it), live steering and pedal meters, dead zones. Settings are
// saved as they change.
#pragma once

#include "app/config.h"
#include "runtime/rom_import.h"

#include <SDL3/SDL.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace app {

// Fullscreen in Config::fullscreen_mode ("WxH@Hz", "*density" when not 1):
// the display's closest exclusive mode, or borderless at the desktop's ("").
void apply_fullscreen_mode(SDL_Window *window, const std::string &mode);

// One launcher for both games: each ROM set is its own build, and setup puts
// both executables in one folder with the launcher (daytona): daytona-93
// (Deluxe '93, daytona93) and daytona-rev (Revision A, 1994, daytona). The
// other set's executable beside this one, or "" where there is none: not
// built, or a one-set app (mobile, consoles).
std::string game_for_set(const std::string &set);
const char *set_label(const std::string &set); // "Deluxe '93", "Revision A (1994)"

class Launcher {
public:
    Launcher(Config &cfg, SDL_Window *window);

    enum Result { Stay, StartGame, Resume, Reset, Quit, OtherGame }; // OtherGame: start the other set's game
    // Returns true if the event was consumed (binding capture).
    bool handle_event(const SDL_Event &e);
    // Draw the interface (between ImGui::NewFrame and ImGui::Render).
    Result draw(bool game_running, const Devices &devices);

    bool rom_ok() const { return rom_ok_; }
    // The chosen archive is the other set, and its game is built: start that
    // (other_game()) with this archive (other_rom()).
    bool other_game_ready() const { return !rom_ok_ && !other_game_.empty(); }
    const std::string &other_game() const { return other_game_; }
    const std::string &other_rom() const { return other_rom_; }
    void set_error(const std::string &e) { error_ = e; }
    void set_ffb_device(const char *kind) { ffb_device_ = kind; } // what force feedback is playing on
    void set_link_status(const std::string &s) { link_status_ = s; } // link play, for the Game tab
    void set_pacing_status(const std::string &s) { pacing_status_ = s; } // frame pacing now, for the Game tab

private:
    void check_rom();
    void browse();
    static void SDLCALL dialog_done(void *self, const char *const *files, int filter);

    Config &cfg_;
    SDL_Window *window_;
    std::vector<rt::RomCheck> checks_;
    bool rom_ok_ = false;
    std::string rom_message_, error_;
    std::string other_set_, other_game_, other_rom_; // the archive is the other set: its name, game, path
    const char *ffb_device_ = "none";
    std::string link_status_ = "off";
    std::string pacing_status_;
    char link_next_buf_[256] = {};
    char path_buf_[1024] = {};

    std::mutex dialog_mutex_;
    std::string dialog_result_;
    bool dialog_pending_ = false;

    int capture_action_ = -1; // binding being captured
    enum CaptureKind { CaptureKey, CapturePad, CaptureJoy } capture_kind_ = CaptureKey;
    // joystick axis capture: every axis's value when the capture began; the
    // axis being moved, its rest value and the furthest it has gone
    std::map<std::pair<SDL_JoystickID, int>, int> joy_rest_;
    bool joy_tracking_ = false;
    SDL_JoystickID joy_id_ = 0;
    int joy_axis_ = 0, joy_from_ = 0, joy_extreme_ = 0;
    void start_capture(int action, CaptureKind kind, const Devices &devices);
};

} // namespace app
