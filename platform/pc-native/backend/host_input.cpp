/* Daytona USA PC: Maple controller emulation on SDL3 gamepads and wheels.
 *
 * Derived from enDjinn's host-common/host_input.cpp (SDL2), rewritten for
 * SDL3.  On Windows SDL3 reads Xbox pads through XInput (the hint is forced
 * on below), elsewhere through HIDAPI/evdev.  Every pad or wheel is a
 * Dreamcast controller on ports A-D; the keyboard is merged into port A.
 *
 * Pad (Daytona's controls, platform/dreamcast-native/code/practice.c):
 *   left stick X   steering            RT / LT        gas / brake
 *   A (south)      shift up            X (west)       shift down / transmission at start
 *   Y (north)      next view           B (east)       B
 *   D-pad          VR1..VR4 views      Start          start / credit
 * Keyboard:
 *   Left/Right steer, Up gas, Down brake, A shift up, Z shift down,
 *   1-4 views VR1..VR4, V next view, Enter start, Backspace B,
 *   I/K stick up/down (menus), Esc quit, F11 or Alt+Enter fullscreen.
 * Wheel (a joystick SDL reports as a wheel, or any joystick that is not a
 * gamepad): its steering axis goes to the game as it is (pc_steering.cpp),
 * the pedals calibrate themselves from where they rest.  Defaults are the
 * Logitech G29/G920/G923 layout; override with
 *   DAYTONA_WHEEL_STEER, DAYTONA_WHEEL_GAS, DAYTONA_WHEEL_BRAKE   (axis numbers)
 *   DAYTONA_WHEEL_UP, DAYTONA_WHEEL_DOWN, DAYTONA_WHEEL_START,
 *   DAYTONA_WHEEL_VIEW                                             (button numbers)
 * The numbers a wheel reports are printed when it connects.
 */
#include <kos.h>

#include "host_audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>

extern "C" int pc_input_steer_source(unsigned port);

namespace {

enum { STEER_PAD = 0, STEER_KEYS = 1, STEER_WHEEL = 2 };

struct Wheel {
    SDL_Joystick *joystick = nullptr;
    int axes = 0, buttons = 0;
    int steer = 0, gas = 2, brake = 3;
    int up = 4, down = 5, start = 9, view = 3;
    std::array<int, 16> rest{}; /* each axis's released end, learnt as it is seen */
};

struct Port {
    SDL_Gamepad *pad = nullptr;
    Wheel wheel;
    int steer_source = STEER_PAD;
};

std::array<Port, MAPLE_PORT_COUNT> g_ports{};
std::array<maple_device_t, MAPLE_PORT_COUNT> g_devices = {{
    {0, 0, true, {MAPLE_FUNC_CONTROLLER}},
    {1, 0, false, {MAPLE_FUNC_CONTROLLER}},
    {2, 0, false, {MAPLE_FUNC_CONTROLLER}},
    {3, 0, false, {MAPLE_FUNC_CONTROLLER}},
}};
std::array<cont_state_t, MAPLE_PORT_COUNT> g_controller_states{};
struct MapleCallback {
    uint32_t function;
    maple_user_callback_t callback;
    void *user_data;
};
std::vector<MapleCallback> g_maple_attach_callbacks;
std::vector<MapleCallback> g_maple_detach_callbacks;
bool g_controllers_initialized = false;
bool g_quit_requested = false;

bool port_used(const Port &p) { return p.pad != nullptr || p.wheel.joystick != nullptr; }

SDL_JoystickID port_id(const Port &p) {
    if (p.pad != nullptr) return SDL_GetGamepadID(p.pad);
    if (p.wheel.joystick != nullptr) return SDL_GetJoystickID(p.wheel.joystick);
    return 0;
}

int env_int(const char *name, int fallback) {
    const char *v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::atoi(v) : fallback;
}

bool is_wheel(SDL_JoystickID id) {
    return SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || !SDL_IsGamepad(id);
}

bool pad_has_rumble(SDL_Gamepad *pad) {
    return pad != nullptr &&
        SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

void close_port(Port &p) {
    if (p.pad != nullptr) {
        (void)SDL_RumbleGamepad(p.pad, 0u, 0u, 0u);
        SDL_CloseGamepad(p.pad);
    }
    if (p.wheel.joystick != nullptr) SDL_CloseJoystick(p.wheel.joystick);
    p = Port{};
}

void close_all() {
    for (size_t port = 0; port < g_ports.size(); port++) {
        close_port(g_ports[port]);
        g_devices[port].valid = port == 0;
        g_devices[port].info.functions = MAPLE_FUNC_CONTROLLER;
    }
}

void notify_maple_callbacks(const std::array<bool, MAPLE_PORT_COUNT> &previous_valid,
                            const std::array<uint32_t, MAPLE_PORT_COUNT> &previous_functions)
{
    for (size_t port = 0; port < g_devices.size(); port++) {
        const maple_device_t &device = g_devices[port];
        for (const MapleCallback &entry : g_maple_detach_callbacks) {
            const bool was = previous_valid[port] && (previous_functions[port] & entry.function) != 0u;
            const bool is = device.valid && (device.info.functions & entry.function) != 0u;
            if (was && !is && entry.callback != nullptr) entry.callback(&g_devices[port], entry.user_data);
        }
        for (const MapleCallback &entry : g_maple_attach_callbacks) {
            const bool was = previous_valid[port] && (previous_functions[port] & entry.function) != 0u;
            const bool is = device.valid && (device.info.functions & entry.function) != 0u;
            if (!was && is && entry.callback != nullptr) entry.callback(&g_devices[port], entry.user_data);
        }
    }
}

bool open_wheel(Port &p, SDL_JoystickID id) {
    SDL_Joystick *j = SDL_OpenJoystick(id);
    if (j == nullptr) return false;
    Wheel w;
    w.joystick = j;
    w.axes = SDL_GetNumJoystickAxes(j);
    w.buttons = SDL_GetNumJoystickButtons(j);
    if (w.axes < 1) {
        SDL_CloseJoystick(j);
        return false;
    }
    w.gas = w.axes >= 4 ? 2 : 1;
    w.brake = w.axes >= 4 ? 3 : 2;
    w.steer = env_int("DAYTONA_WHEEL_STEER", w.steer);
    w.gas = env_int("DAYTONA_WHEEL_GAS", w.gas);
    w.brake = env_int("DAYTONA_WHEEL_BRAKE", w.brake);
    w.up = env_int("DAYTONA_WHEEL_UP", w.up);
    w.down = env_int("DAYTONA_WHEEL_DOWN", w.down);
    w.start = env_int("DAYTONA_WHEEL_START", w.start);
    w.view = env_int("DAYTONA_WHEEL_VIEW", w.view);
    for (int a = 0; a < w.axes && a < static_cast<int>(w.rest.size()); a++) {
        const int v = SDL_GetJoystickAxis(j, a);
        /* A pedal rests at one end; an axis that reads near centre at open
         * (some wheels until first moved) is taken to rest at the top, the
         * common convention, and relearnt as it moves. */
        w.rest[a] = v > -16384 && v < 16384 ? 32767 : v;
    }
    p.wheel = w;
    std::fprintf(stderr,
                 "daytona: wheel on port %c: %s (%d axes, %d buttons; steer axis %d, gas %d, brake %d; "
                 "buttons up %d, down %d, start %d, view %d)\n",
                 static_cast<char>('A' + (&p - g_ports.data())), SDL_GetJoystickName(j), w.axes, w.buttons,
                 w.steer, w.gas, w.brake, w.up, w.down, w.start, w.view);
    return true;
}

void refresh_devices()
{
    std::array<bool, MAPLE_PORT_COUNT> previous_valid{};
    std::array<uint32_t, MAPLE_PORT_COUNT> previous_functions{};
    for (size_t port = 0; port < g_devices.size(); port++) {
        previous_valid[port] = g_devices[port].valid;
        previous_functions[port] = g_devices[port].info.functions;
    }
    for (Port &p : g_ports) {
        if ((p.pad != nullptr && !SDL_GamepadConnected(p.pad)) ||
            (p.wheel.joystick != nullptr && !SDL_JoystickConnected(p.wheel.joystick))) {
            close_port(p);
        }
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids != nullptr && i < count; i++) {
        bool open = false;
        for (const Port &p : g_ports) open = open || (port_used(p) && port_id(p) == ids[i]);
        if (open) continue;
        auto slot = std::find_if(g_ports.begin(), g_ports.end(), [](const Port &p) { return !port_used(p); });
        if (slot == g_ports.end()) break;
        if (is_wheel(ids[i])) {
            (void)open_wheel(*slot, ids[i]);
        } else if ((slot->pad = SDL_OpenGamepad(ids[i])) != nullptr) {
            std::fprintf(stderr, "daytona: controller on port %c: %s\n",
                         static_cast<char>('A' + (slot - g_ports.begin())), SDL_GetGamepadName(slot->pad));
        }
    }
    SDL_free(ids);
    for (size_t port = 0; port < g_devices.size(); port++) {
        const Port &p = g_ports[port];
        g_devices[port].valid = port == 0 || port_used(p);
        g_devices[port].info.functions = MAPLE_FUNC_CONTROLLER;
        if (pad_has_rumble(p.pad)) g_devices[port].info.functions |= MAPLE_FUNC_PURUPURU;
    }
    notify_maple_callbacks(previous_valid, previous_functions);
}

int8_t stick_to_i8(Sint16 axis, int deadzone)
{
    if (axis > -deadzone && axis < deadzone) return 0;
    return static_cast<int8_t>(std::clamp<int>(axis / 256, -127, 127));
}

uint8_t trigger_to_u8(Sint16 axis) { return axis > 0 ? static_cast<uint8_t>(std::min<int>(axis / 128, 255)) : 0u; }

Sint16 pad_axis(SDL_Gamepad *pad, SDL_GamepadAxis which) { return pad != nullptr ? SDL_GetGamepadAxis(pad, which) : 0; }

bool pad_button(SDL_Gamepad *pad, SDL_GamepadButton which) { return pad != nullptr && SDL_GetGamepadButton(pad, which); }

int wheel_axis(const Wheel &w, int a) {
    return a >= 0 && a < w.axes ? SDL_GetJoystickAxis(w.joystick, a) : 0;
}

bool wheel_button(const Wheel &w, int b) {
    return w.joystick != nullptr && b >= 0 && b < w.buttons && SDL_GetJoystickButton(w.joystick, b);
}

/* A pedal, 0 (released) .. 255 (floored), from where it rests to the other end. */
uint8_t wheel_pedal(Wheel &w, int a) {
    if (w.joystick == nullptr || a < 0 || a >= w.axes || a >= static_cast<int>(w.rest.size())) return 0u;
    const int v = SDL_GetJoystickAxis(w.joystick, a);
    int &rest = w.rest[a];
    if (rest >= 0 && v > rest) rest = v;
    if (rest < 0 && v < rest) rest = v;
    const float travel = rest >= 0 ? static_cast<float>(rest - v) / static_cast<float>(rest + 32768)
                                   : static_cast<float>(v - rest) / static_cast<float>(32767 - rest);
    const float pressed = std::clamp((travel - 0.02f) / 0.98f, 0.f, 1.f);
    return static_cast<uint8_t>(pressed * 255.f + 0.5f);
}

}  // namespace

extern "C" {

/* pc_steering.cpp: which model steers this port. */
int pc_input_steer_source(unsigned port)
{
    return port < g_ports.size() ? g_ports[port].steer_source : STEER_PAD;
}

void enj_host_input_request_quit(void) { g_quit_requested = true; }

void enj_host_input_shutdown(void)
{
    enj_host::audio_shutdown();
    close_all();
    g_maple_attach_callbacks.clear();
    g_maple_detach_callbacks.clear();
    g_controllers_initialized = false;
    g_quit_requested = false;
    SDL_Quit();
}

int enj_host_controllers_init(void)
{
    if (g_controllers_initialized) return 0;
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "daytona: SDL controller initialization failed: %s\n", SDL_GetError());
        return -1;
    }
    refresh_devices();
    g_controllers_initialized = true;
    return 0;
}

void enj_host_controllers_shutdown(void) { close_all(); }

int enj_host_quit_requested(void) { return g_quit_requested ? 1 : 0; }

size_t enj_host_controllers_poll(cont_state_t *states, uint8_t *connected, size_t capacity)
{
    if (states == nullptr || connected == nullptr || capacity == 0u) return 0u;
    std::memset(states, 0, capacity * sizeof(*states));
    std::memset(connected, 0, capacity * sizeof(*connected));

    SDL_PumpEvents();
    refresh_devices();
    const bool *keys = SDL_GetKeyboardState(nullptr);

    const size_t count = std::min(capacity, g_ports.size());
    for (size_t port = 0; port < count; port++) {
        Port &p = g_ports[port];
        SDL_Gamepad *pad = p.pad;
        Wheel &w = p.wheel;
        cont_state_t &s = states[port];
        const bool kb = port == 0 && keys != nullptr;
        auto key = [&](SDL_Scancode sc) { return kb && keys[sc]; };
        connected[port] = kb || port_used(p);

        const bool key_steer = key(SDL_SCANCODE_LEFT) || key(SDL_SCANCODE_RIGHT);
        if (w.joystick != nullptr) {
            s.joyx = static_cast<int8_t>(std::clamp<int>(wheel_axis(w, w.steer) / 256, -127, 127));
            s.rtrig = std::max(wheel_pedal(w, w.gas), key(SDL_SCANCODE_UP) ? uint8_t{255} : uint8_t{0});
            s.ltrig = std::max(wheel_pedal(w, w.brake), key(SDL_SCANCODE_DOWN) ? uint8_t{255} : uint8_t{0});
            p.steer_source = STEER_WHEEL;
        } else {
            s.joyx = stick_to_i8(pad_axis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0);
            s.rtrig = key(SDL_SCANCODE_UP) ? 255u : trigger_to_u8(pad_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            s.ltrig = key(SDL_SCANCODE_DOWN) ? 255u : trigger_to_u8(pad_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            /* The keys keep the steering until the stick is moved again. */
            if (key_steer) {
                p.steer_source = STEER_KEYS;
            } else if (std::abs(static_cast<int>(s.joyx)) > 12) {
                p.steer_source = STEER_PAD;
            }
        }
        if (key_steer) {
            s.joyx = key(SDL_SCANCODE_LEFT) ? -127 : 127;
            p.steer_source = STEER_KEYS;
        }
        s.a = key(SDL_SCANCODE_A) || pad_button(pad, SDL_GAMEPAD_BUTTON_SOUTH) || wheel_button(w, w.up);
        s.b = key(SDL_SCANCODE_BACKSPACE) || pad_button(pad, SDL_GAMEPAD_BUTTON_EAST);
        s.x = key(SDL_SCANCODE_Z) || pad_button(pad, SDL_GAMEPAD_BUTTON_WEST) || wheel_button(w, w.down);
        s.y = key(SDL_SCANCODE_V) || pad_button(pad, SDL_GAMEPAD_BUTTON_NORTH) || wheel_button(w, w.view);
        s.start = key(SDL_SCANCODE_RETURN) || pad_button(pad, SDL_GAMEPAD_BUTTON_START) || wheel_button(w, w.start);
        s.dpad_up = key(SDL_SCANCODE_1) || pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        s.dpad_right = key(SDL_SCANCODE_2) || pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        s.dpad_down = key(SDL_SCANCODE_3) || pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        s.dpad_left = key(SDL_SCANCODE_4) || pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        if (w.joystick != nullptr && SDL_GetNumJoystickHats(w.joystick) > 0) {
            const Uint8 hat = SDL_GetJoystickHat(w.joystick, 0);
            s.dpad_up = s.dpad_up || (hat & SDL_HAT_UP) != 0;
            s.dpad_right = s.dpad_right || (hat & SDL_HAT_RIGHT) != 0;
            s.dpad_down = s.dpad_down || (hat & SDL_HAT_DOWN) != 0;
            s.dpad_left = s.dpad_left || (hat & SDL_HAT_LEFT) != 0;
        }
        s.joyy = key(SDL_SCANCODE_I) ? -127 : key(SDL_SCANCODE_K) ? 127 : stick_to_i8(pad_axis(pad, SDL_GAMEPAD_AXIS_LEFTY), 6000);
    }
    /* DAYTONA_AUTOPLAY=1 (testing without hands): Start pulses every two
     * seconds for the first 16 (credit, start, course, transmission), then
     * the gas held down on port A. */
    static const bool autoplay = [] {
        const char *v = std::getenv("DAYTONA_AUTOPLAY");
        return v != nullptr && *v == '1';
    }();
    if (autoplay) {
        static const uint64_t t0 = SDL_GetTicks();
        const uint64_t t = SDL_GetTicks() - t0;
        /* DAYTONA_AUTOPLAY_KEYS="ms:BUTTON,..." (testing the menus without hands): those presses (300 ms each,
         * START A B X Y UP DOWN LEFT RIGHT) in place of the Start pulses; the gas after the last one. */
        static const std::vector<std::pair<uint32_t, std::string>> keys = [] {
            std::vector<std::pair<uint32_t, std::string>> out;
            const char *v = std::getenv("DAYTONA_AUTOPLAY_KEYS");
            for (const char *p = v; p && *p;) {
                char *end = nullptr;
                const unsigned long ms = std::strtoul(p, &end, 10);
                if (!end || *end != ':') break;
                const char *name = end + 1, *stop = std::strchr(name, ',');
                out.emplace_back(static_cast<uint32_t>(ms), std::string(name, stop ? stop : name + std::strlen(name)));
                p = stop ? stop + 1 : nullptr;
            }
            return out;
        }();
        if (!keys.empty()) {
            for (const auto &[ms, name] : keys) {
                if (t < ms || t >= ms + 300u) continue;
                if (name == "START") states[0].start = 1u;
                else if (name == "A") states[0].a = 1u;
                else if (name == "B") states[0].b = 1u;
                else if (name == "X") states[0].x = 1u;
                else if (name == "Y") states[0].y = 1u;
                else if (name == "UP") states[0].dpad_up = 1u;
                else if (name == "DOWN") states[0].dpad_down = 1u;
                else if (name == "LEFT") states[0].dpad_left = 1u;
                else if (name == "RIGHT") states[0].dpad_right = 1u;
            }
            if (t > keys.back().first + 1000u) states[0].rtrig = 255u;
        } else if (t < 16000u) {
            states[0].start = (t % 2000u) >= 1000u && (t % 2000u) < 1150u;
        } else {
            states[0].rtrig = 255u;
        }
    }
    if (g_quit_requested) {
        /* START+A+B+X+Y: the game's and enDjinn's exit chord. */
        states[0].a = states[0].b = states[0].x = states[0].y = states[0].start = 1u;
    }
    return count;
}

maple_device_t *maple_enum_type(int index, uint32_t function)
{
    (void)enj_host_controllers_init();
    if (index < 0) return nullptr;
    int match = 0;
    for (maple_device_t &device : g_devices) {
        if (!device.valid || (device.info.functions & function) == 0u) continue;
        if (match == index) return &device;
        match++;
    }
    return nullptr;
}

maple_device_t *maple_enum_dev(int port, int unit)
{
    return port >= 0 && port < MAPLE_PORT_COUNT && unit == 0 && g_devices[port].valid ? &g_devices[port] : nullptr;
}

void *maple_dev_status(maple_device_t *device)
{
    if (device == nullptr || device->port < 0 || device->port >= MAPLE_PORT_COUNT) return nullptr;
    uint8_t connected[MAPLE_PORT_COUNT]{};
    (void)enj_host_controllers_poll(g_controller_states.data(), connected, MAPLE_PORT_COUNT);
    return connected[device->port] != 0u ? &g_controller_states[device->port] : nullptr;
}

static void add_callback(std::vector<MapleCallback> &list, uint32_t function, maple_user_callback_t callback,
                         void *user_data)
{
    if (callback == nullptr) return;
    for (const MapleCallback &e : list) {
        if (e.function == function && e.callback == callback && e.user_data == user_data) return;
    }
    list.push_back({function, callback, user_data});
}

void maple_attach_callback(uint32_t function, maple_user_callback_t callback, void *user_data)
{
    add_callback(g_maple_attach_callbacks, function, callback, user_data);
}

void maple_detach_callback(uint32_t function, maple_user_callback_t callback, void *user_data)
{
    add_callback(g_maple_detach_callbacks, function, callback, user_data);
}

void purupuru_rumble_raw(maple_device_t *device, uint32_t raw)
{
    if (device == nullptr || device->port < 0 || device->port >= MAPLE_PORT_COUNT) return;
    SDL_Gamepad *pad = g_ports[static_cast<size_t>(device->port)].pad;
    if (pad == nullptr) return;
    const uint32_t motor = (raw >> 4u) & 0x0fu;
    const uint32_t power = std::max((raw >> 8u) & 0x07u, (raw >> 12u) & 0x07u);
    if (motor == 0u || power == 0u) {
        (void)SDL_RumbleGamepad(pad, 0u, 0u, 0u);
        return;
    }
    const uint32_t frequency = (raw >> 16u) & 0xffu;
    const uint32_t inclination = (raw >> 24u) & 0xffu;
    const bool continuous = (raw & 1u) != 0u;
    const double magnitude = static_cast<double>(power) / 7.0;
    const double high_mix = std::clamp((static_cast<double>(frequency) - 4.0) / 55.0, 0.0, 1.0);
    const uint32_t duration_ms = continuous ? 60000u : inclination != 0u ? std::clamp(inclination * 8u, 45u, 2000u) : 120u;
    const Uint16 low = static_cast<Uint16>(magnitude * (1.0 - high_mix) * 65535.0 + 0.5);
    const Uint16 high = static_cast<Uint16>(magnitude * high_mix * 65535.0 + 0.5);
    (void)SDL_RumbleGamepad(pad, low, high, duration_ms);
}

}  // extern "C"
