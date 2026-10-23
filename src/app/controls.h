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
    float wheel_sensitivity = 1.0f; // response curve, 0.25..3; 1 is linear
    bool steer_invert = false;
    // Gamepad steering fix: on = the pad steers like the arcade wheel feels (see docs/gamepad-steering.md), off = the plain direct mapping.
    bool gamepad_fix = false;
    float wheel_weight = 1.0f;  // D-pad: the lock a held push keeps as the spring stiffens (1 = the arcade's 100 / 78 / 64 / 54%)
    float wheel_turnin = 0.06f; // D-pad: lock gained per frame at the lightest spring
    float wheel_return = 0.08f; // D-pad: lock returned per frame at the lightest spring
    float stick_range = 0.6f;   // pad stick: share of the lock reached by normal travel (full lock only at the very end)
    float stick_expo = 2.6f;    // pad stick: 1 = linear, higher = small pushes turn less

    void set_defaults();
    // Current value of an action, 0..1 (keys and buttons are 0 or 1).
    float value(Action a, const bool *keys, const Devices &d) const;
    bool analog_source(Action a, const Devices &d) const; // an axis binding is in use
    // Pad-only helpers for the steering fix: a key / D-pad button push (no axes), and whether the steering is a pad's
    // stick (a gamepad axis, or a joystick axis on a device that is a gamepad) and not a real wheel.
    float digital_value(Action a, const bool *keys, const Devices &d) const;
    bool pad_stick_bound(const Devices &d) const;

    // Build this frame's I/O board inputs. Keyboard steering ramps; analogue
    // steering and pedals are direct.
    rt::Inputs sample(const bool *keys, const Devices &d);

    // Live values for the UI
    float steer = 0, accel = 0, brake = 0;
    int gear = 1;
    // Set by the game loop each frame, from the game's memory and the drive board (see main.cpp):
    float speed = 0.f;         // car speed, 0..~1 (HUD km/h / 215)
    float spring = 7.f / 31.f; // the wheel's centring spring as the game commands it: 0.23 (slow) .. 0.42 (above ~255 km/h)
    bool seen_analog = false;  // the pad stick has read a value between 0 and full: a real analogue stick, not a digital one
    int mid_frames = 0;        // consecutive frames the pad stick has read between 0 and full
private:
    bool held_[kNumActions] = {};
};

} // namespace app
