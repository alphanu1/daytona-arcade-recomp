// Control bindings: each arcade control is bound to a key, a gamepad input (a
// button, or one half of an axis) and a joystick input: any connected
// joystick, so a wheel, its pedals and a shifter, even as separate devices
// (by device GUID: an axis, a button or a hat direction). Axis bindings are
// analogue: triggers give the accelerator and brake their full travel, a stick
// or a wheel gives steering. A joystick axis is calibrated when it is bound:
// where it rests and how far it was moved (pedals that rest at either end, or
// short of full range; how far a wheel turns to full lock).
#pragma once

#include "runtime/m2_board.h"

#include <SDL3/SDL.h>

#include <string>
#include <array>
#include <vector>

namespace app {

enum Action {
    SteerLeft, SteerRight, Accelerate, Brake,
    Gear1, Gear2, Gear3, Gear4, GearUp, GearDown,
    View1, View2, View3, View4,
    Coin, Start, Test, Service,
    kNumActions
};
const char *action_name(Action a);   // for the UI
const char *action_key(Action a);    // for the config file

struct PadInput {
    enum Kind { None, Button, Axis } kind = None;
    int index = 0; // SDL_GamepadButton or SDL_GamepadAxis
    int dir = 1;   // axis: +1 the positive half, -1 the negative half
    std::string describe() const;
    std::string save() const;
    static PadInput parse(const std::string &s);
};

// The connected input devices: the gamepad (the first one) and every joystick.
struct Devices {
    SDL_Gamepad *pad = nullptr;
    std::vector<SDL_Joystick *> joys;
    // SDL device events: opens and closes devices as they come and go.
    void handle_event(const SDL_Event &e);
    void close_all();
    SDL_Joystick *find(const std::string &guid) const; // the first joystick with this GUID
};

struct JoyInput {
    enum Kind { None, Button, Axis, Hat } kind = None;
    std::string guid; // the device (SDL_GUIDToString)
    int index = 0;    // button, axis or hat number
    int rest = 0, full = 32767; // axis: raw value at rest and at full travel
    int mask = 0;     // hat: the SDL_HAT_ direction
    float value(const Devices &d, float deadzone) const; // 0..1
    std::string describe(const Devices &d) const;
    std::string save() const;
    static JoyInput parse(const std::string &s);
};

struct Binding {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    PadInput pad;
    JoyInput joy;
};

struct Controls {
    std::array<float, kNumActions> touch{};
    bool touch_steering = false;
    Binding bind[kNumActions];
    float deadzone = 0.08f;       // stick and trigger dead zone (fraction of travel)
    float joy_deadzone = 0.02f;   // joystick (wheel, pedal) axis dead zone
    bool steer_invert = false;

    void set_defaults();
    // Current value of an action, 0..1 (keys and buttons are 0 or 1).
    float value(Action a, const bool *keys, const Devices &d) const;
    bool analog_source(Action a, const Devices &d) const; // an axis binding is in use

    // Build this frame's I/O board inputs. Keyboard steering ramps; analogue
    // steering and pedals are direct.
    rt::Inputs sample(const bool *keys, const Devices &d);

    // Live values for the UI
    float steer = 0, accel = 0, brake = 0;
    bool slow_steer = false; // menu screen: one push selects one step and the steering stays there (see Controls::sample)
    int menu_sel = -1, menu_dir = 0; // menus: the selected side (-1 left, 0 centre, 1 right) and the last pushed direction
    int gear = 1;
private:
    bool held_[kNumActions] = {};
};

} // namespace app
