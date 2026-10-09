#include "app/controls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace app {

namespace {
const struct {
    const char *name, *key;
} kActions[kNumActions] = {
    {"Steer left", "steer_left"}, {"Steer right", "steer_right"}, {"Accelerate", "accelerate"}, {"Brake", "brake"},
    {"Gear 1", "gear1"},          {"Gear 2", "gear2"},            {"Gear 3", "gear3"},           {"Gear 4", "gear4"},
    {"Shift up", "gear_up"},      {"Shift down", "gear_down"},    {"View 1 (red)", "view1"},    {"View 2 (blue)", "view2"},
    {"View 3 (yellow)", "view3"}, {"View 4 (green)", "view4"},    {"Coin", "coin"},              {"Start", "start"},
    {"Test", "test"},             {"Service", "service"},
};
} // namespace

const char *action_name(Action a) { return kActions[a].name; }
const char *action_key(Action a) { return kActions[a].key; }

std::string PadInput::describe() const {
    switch (kind) {
    case Button: {
        const char *n = SDL_GetGamepadStringForButton(SDL_GamepadButton(index));
        return std::string("Button ") + (n ? n : "?");
    }
    case Axis: {
        const char *n = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(index));
        const bool trigger = index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return std::string(trigger ? "Trigger " : "Axis ") + (n ? n : "?") + (trigger ? "" : (dir > 0 ? " +" : " -"));
    }
    default: return "-";
    }
}

std::string PadInput::save() const {
    switch (kind) {
    case Button: return std::string("button:") + SDL_GetGamepadStringForButton(SDL_GamepadButton(index));
    case Axis: return std::string("axis:") + SDL_GetGamepadStringForAxis(SDL_GamepadAxis(index)) + (dir > 0 ? ":+" : ":-");
    default: return "none";
    }
}

PadInput PadInput::parse(const std::string &s) {
    PadInput p;
    if (s.compare(0, 7, "button:") == 0) {
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.substr(7).c_str());
        if (b != SDL_GAMEPAD_BUTTON_INVALID) p.kind = Button, p.index = b;
    } else if (s.compare(0, 5, "axis:") == 0) {
        const auto c = s.find(':', 5);
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s.substr(5, c - 5).c_str());
        if (a != SDL_GAMEPAD_AXIS_INVALID) p.kind = Axis, p.index = a, p.dir = (c != std::string::npos && s[c + 1] == '-') ? -1 : 1;
    }
    return p;
}

void Devices::handle_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
        if (SDL_Joystick *j = SDL_OpenJoystick(e.jdevice.which)) { // our own reference (a gamepad holds another)
            if (std::find(joys.begin(), joys.end(), j) == joys.end()) joys.push_back(j);
            else SDL_CloseJoystick(j);
        }
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        for (auto it = joys.begin(); it != joys.end(); ++it)
            if (SDL_GetJoystickID(*it) == e.jdevice.which) {
                SDL_CloseJoystick(*it);
                joys.erase(it);
                break;
            }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && e.gdevice.which == SDL_GetGamepadID(pad)) SDL_CloseGamepad(pad), pad = nullptr;
        break;
    default: break;
    }
}

void Devices::close_all() {
    if (pad) SDL_CloseGamepad(pad), pad = nullptr;
    for (SDL_Joystick *j : joys) SDL_CloseJoystick(j);
    joys.clear();
}

namespace {
std::string guid_string(SDL_Joystick *j) {
    char b[64];
    SDL_GUIDToString(SDL_GetJoystickGUID(j), b, sizeof b);
    return b;
}
} // namespace

SDL_Joystick *Devices::find(const std::string &guid) const {
    for (SDL_Joystick *j : joys)
        if (guid_string(j) == guid) return j;
    return nullptr;
}

float JoyInput::value(const Devices &d, float deadzone) const {
    if (kind == None) return 0.f;
    SDL_Joystick *j = d.find(guid);
    if (!j) return 0.f;
    switch (kind) {
    case Button: return SDL_GetJoystickButton(j, index) ? 1.f : 0.f;
    case Hat: return (SDL_GetJoystickHat(j, index) & mask) ? 1.f : 0.f;
    case Axis: {
        if (full == rest) return 0.f;
        const float x = std::clamp(float(SDL_GetJoystickAxis(j, index) - rest) / float(full - rest), 0.f, 1.f);
        return x <= deadzone ? 0.f : (x - deadzone) / (1.f - deadzone);
    }
    default: return 0.f;
    }
}

std::string JoyInput::describe(const Devices &d) const {
    if (kind == None) return "-";
    SDL_Joystick *j = d.find(guid);
    std::string what;
    if (kind == Button) what = "Button " + std::to_string(index + 1);
    else if (kind == Hat) what = std::string("Hat ") + (mask == SDL_HAT_UP ? "up" : mask == SDL_HAT_DOWN ? "down" : mask == SDL_HAT_LEFT ? "left" : "right");
    else what = "Axis " + std::to_string(index + 1) + (full > rest ? " +" : " -");
    const char *name = j ? SDL_GetJoystickName(j) : nullptr;
    return what + (j ? std::string(" (") + (name ? name : "joystick") + ")" : " (not connected)");
}

std::string JoyInput::save() const {
    switch (kind) {
    case Button: return "button:" + guid + ":" + std::to_string(index);
    case Hat: return "hat:" + guid + ":" + std::to_string(index) + ":" + std::to_string(mask);
    case Axis: return "axis:" + guid + ":" + std::to_string(index) + ":" + std::to_string(rest) + ":" + std::to_string(full);
    default: return "none";
    }
}

JoyInput JoyInput::parse(const std::string &s) {
    JoyInput j;
    std::vector<std::string> f;
    for (size_t at = 0;;) {
        const size_t c = s.find(':', at);
        f.push_back(s.substr(at, c - at));
        if (c == std::string::npos) break;
        at = c + 1;
    }
    auto num = [&](size_t i) { return i < f.size() ? std::atoi(f[i].c_str()) : 0; };
    if (f.size() >= 3 && f[0] == "button") j.kind = Button, j.guid = f[1], j.index = num(2);
    else if (f.size() >= 4 && f[0] == "hat") j.kind = Hat, j.guid = f[1], j.index = num(2), j.mask = num(3);
    else if (f.size() >= 5 && f[0] == "axis") j.kind = Axis, j.guid = f[1], j.index = num(2), j.rest = num(3), j.full = num(4);
    return j;
}

void Controls::set_defaults() {
    auto b = [&](Action a, SDL_Scancode k, PadInput::Kind kind, int index, int dir = 1) {
        bind[a].key = k;
        bind[a].pad.kind = kind;
        bind[a].pad.index = index;
        bind[a].pad.dir = dir;
        bind[a].joy = JoyInput{};
    };
    b(SteerLeft, SDL_SCANCODE_LEFT, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFTX, -1);
    b(SteerRight, SDL_SCANCODE_RIGHT, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFTX, +1);
    b(Accelerate, SDL_SCANCODE_UP, PadInput::Axis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    b(Brake, SDL_SCANCODE_DOWN, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    b(Gear1, SDL_SCANCODE_1, PadInput::None, 0);
    b(Gear2, SDL_SCANCODE_2, PadInput::None, 0);
    b(Gear3, SDL_SCANCODE_3, PadInput::None, 0);
    b(Gear4, SDL_SCANCODE_4, PadInput::None, 0);
    b(GearUp, SDL_SCANCODE_W, PadInput::Button, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    b(GearDown, SDL_SCANCODE_Q, PadInput::Button, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    b(View1, SDL_SCANCODE_A, PadInput::Button, SDL_GAMEPAD_BUTTON_SOUTH);
    b(View2, SDL_SCANCODE_S, PadInput::Button, SDL_GAMEPAD_BUTTON_EAST);
    b(View3, SDL_SCANCODE_D, PadInput::Button, SDL_GAMEPAD_BUTTON_WEST);
    b(View4, SDL_SCANCODE_F, PadInput::Button, SDL_GAMEPAD_BUTTON_NORTH);
    b(Coin, SDL_SCANCODE_5, PadInput::Button, SDL_GAMEPAD_BUTTON_BACK);
    b(Start, SDL_SCANCODE_RETURN, PadInput::Button, SDL_GAMEPAD_BUTTON_START);
    b(Test, SDL_SCANCODE_F2, PadInput::None, 0);
    b(Service, SDL_SCANCODE_F3, PadInput::None, 0);
    deadzone = 0.08f;
    joy_deadzone = 0.02f;
    steer_invert = false;
}

float Controls::value(Action a, const bool *keys, const Devices &d) const {
    const Binding &b = bind[a];
    SDL_Gamepad *pad = d.pad;
    float v = (b.key != SDL_SCANCODE_UNKNOWN && keys && keys[b.key]) ? 1.f : 0.f;
    if (pad && b.pad.kind == PadInput::Button && SDL_GetGamepadButton(pad, SDL_GamepadButton(b.pad.index))) v = 1.f;
    if (pad && b.pad.kind == PadInput::Axis) {
        float x = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(b.pad.index)) / 32767.f * float(b.pad.dir);
        x = std::clamp(x, 0.f, 1.f);
        x = x <= deadzone ? 0.f : (x - deadzone) / (1.f - deadzone); // rescale past the dead zone: full travel still reaches 1
        v = std::max(v, x);
    }
    return std::max({v, b.joy.value(d, joy_deadzone), touch[a]});
}

bool Controls::analog_source(Action a, const Devices &d) const {
    return (touch_steering && (a == SteerLeft || a == SteerRight)) ||
           (d.pad && bind[a].pad.kind == PadInput::Axis) || (bind[a].joy.kind == JoyInput::Axis && d.find(bind[a].joy.guid));
}

rt::Inputs Controls::sample(const bool *keys, const Devices &d) {
    rt::Inputs in;
    // Steering: an analogue stick sets the position directly; keys ramp toward full lock and back.
    const float l = value(SteerLeft, keys, d), r = value(SteerRight, keys, d);
    float target = r - l;
    if (steer_invert) target = -target;
    const bool analog = (analog_source(SteerLeft, d) || analog_source(SteerRight, d)) &&
                        !(keys && (keys[bind[SteerLeft].key] || keys[bind[SteerRight].key]));
    if (slow_steer) {
        // Menus: one push moves the selection one step and the steering stays there (left / centre / right) until the
        // next push, as on the Saturn and Dreamcast ports. A menu opens on its first entry.
        const int dir = target > 0.5f ? 1 : target < -0.5f ? -1 : 0;
        if (dir != 0 && dir != menu_dir) menu_sel = std::clamp(menu_sel + dir, -1, 1);
        menu_dir = dir;
        steer = float(menu_sel);
    } else {
        menu_sel = -1;
        menu_dir = 0;
        steer = analog ? target : steer + std::clamp(target - steer, -0.12f, 0.12f);
    }
    accel = value(Accelerate, keys, d);
    brake = value(Brake, keys, d);
    // ADC ranges (MAME's daytona ports): steering 0x20-0xe0 centred on 0x80, pedals 0x20 (up) to 0xe0 (floored)
    const float s = std::clamp(steer, -1.f, 1.f);
    if (slow_steer) {
        // Menus (circuit select) hold the highlight only while the wheel reads 0x86-0x96, step right above that and
        // left below it: a centred stick or D-pad (0x80) would keep pushing left. Centre on 0x8e, keep the full range.
        in.steer = uint8_t(std::lround(s >= 0 ? 0x8e + s * (0xe0 - 0x8e) : 0x8e + s * (0x8e - 0x20)));
    } else {
        in.steer = uint8_t(std::lround(0x80 + s * 0x60));
    }
    in.accel = uint8_t(std::lround(0x20 + std::clamp(accel, 0.f, 1.f) * 0xc0));
    in.brake = uint8_t(std::lround(0x20 + std::clamp(brake, 0.f, 1.f) * 0xc0));

    // Gears: direct selection, or sequential shifts on the press
    for (int g = 0; g < 4; g++)
        if (value(Action(Gear1 + g), keys, d) > 0.5f) gear = g + 1;
    for (Action a : {GearUp, GearDown}) {
        const bool now = value(a, keys, d) > 0.5f;
        if (now && !held_[a]) gear = std::clamp(gear + (a == GearUp ? 1 : -1), 1, 4);
        held_[a] = now;
    }
    static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r: neutral, 1-4
    in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[gear] << 4));

    auto low = [&](uint8_t &port, uint8_t bit, Action a) { if (value(a, keys, d) > 0.5f) port &= uint8_t(~bit); };
    low(in.in0, 0x01, Coin);
    low(in.in0, 0x04, Test);
    low(in.in0, 0x08, Service);
    low(in.in0, 0x10, Start);
    low(in.in0, 0x20, View1);
    low(in.in0, 0x40, View2);
    low(in.in0, 0x80, View3);
    low(in.in1, 0x01, View4);
    return in;
}

} // namespace app
