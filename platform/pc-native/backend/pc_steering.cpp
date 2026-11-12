/* Daytona USA PC: steering from a pad, the keyboard or a wheel to the arcade
 * wheel's ADC byte (0x20..0xe0, centre 0x80), which the game's own decoder
 * (nm_controls_decode) reads as on the cabinet.
 *
 * The arcade turns the car from the wheel position as it is, at every speed;
 * what makes it feel heavy at speed is the drive board's centring spring,
 * which stiffens in four steps (from about 53, 119, 187 and 255 km/h) and
 * limits how far and how fast a person turns the wheel.  A real wheel needs
 * nothing more: its position goes to the ADC directly.  A pad's stick can
 * reach full lock in one frame, so it gets the model the repository's
 * desktop build tuned for this game (docs/gamepad-steering.md,
 * src/app/controls.cpp): most of the stick's travel reaches only part of the
 * lock, the last 15% opens to full lock, and the virtual wheel moves toward
 * the stick at a rate that, like the lock, falls with speed.  Keys push
 * against the spring the game would command at that speed.
 *
 * Tuning (environment, all optional):
 *   DAYTONA_PAD_STEER=direct        the stick straight to the wheel (no model)
 *   DAYTONA_STICK_RANGE=0.6         share of the lock normal stick travel reaches (0.4..1)
 *   DAYTONA_STICK_EXPO=2.6          1 linear, higher: small pushes turn less (1..4)
 *   DAYTONA_STICK_DEADZONE=0.08     stick dead zone (0..0.5)
 *   DAYTONA_WHEEL_SENSITIVITY=1.0   wheel response curve, 1 linear (0.25..3)
 *   DAYTONA_STEER_INVERT=1          reverse steering
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

extern "C" int pc_input_steer_source(unsigned port); /* host_input.cpp: PC_STEER_* */
extern "C" float pc_saturn_handling_factor(void);     /* code/pc_saturn_options.c: Options > HANDLING */

namespace {

enum { PC_STEER_PAD = 0, PC_STEER_KEYS = 1, PC_STEER_WHEEL = 2 };
constexpr unsigned kPorts = 4;

float env_float(const char *name, float fallback, float lo, float hi) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') return fallback;
  const float f = std::strtof(v, nullptr);
  return std::isfinite(f) ? std::clamp(f, lo, hi) : fallback;
}

struct Settings {
  bool direct_pad = false;
  bool invert = false;
  float stick_range = 0.6f, stick_expo = 2.6f, stick_deadzone = 0.08f;
  float wheel_sensitivity = 1.0f;
  float wheel_weight = 1.0f, wheel_turnin = 0.06f, wheel_return = 0.08f; /* keys */
  Settings() {
    const char *mode = std::getenv("DAYTONA_PAD_STEER");
    direct_pad = mode != nullptr && (mode[0] == 'd' || mode[0] == 'D');
    const char *inv = std::getenv("DAYTONA_STEER_INVERT");
    invert = inv != nullptr && inv[0] == '1';
    stick_range = env_float("DAYTONA_STICK_RANGE", stick_range, 0.4f, 1.0f);
    stick_expo = env_float("DAYTONA_STICK_EXPO", stick_expo, 1.0f, 4.0f);
    stick_deadzone = env_float("DAYTONA_STICK_DEADZONE", stick_deadzone, 0.0f, 0.5f);
    wheel_sensitivity = env_float("DAYTONA_WHEEL_SENSITIVITY", wheel_sensitivity, 0.25f, 3.0f);
  }
};

struct PortState {
  float steer = 0.f;      /* the virtual wheel, -1..1 */
  int spring_step = -1;   /* -1: the resistance below ~53 km/h (the lightest spring), 0..3 */
  float key_hold = 0.f;
  int key_release_frames = 3;
  int last_source = PC_STEER_PAD;
};

const Settings &settings() {
  static const Settings s;
  return s;
}

PortState g_ports[kPorts];

/* The drive board's centring spring for the speed (docs/gamepad-steering.md:
 * steps up at about 53, 119, 187, 255 km/h, down a few km/h lower). */
int spring_step(int current, float kmh) {
  static constexpr float up[4] = {53.f, 119.f, 187.f, 255.f};
  constexpr float hysteresis = 6.f;
  int step = current;
  while (step < 3 && kmh >= up[step + 1]) ++step;
  while (step >= 0 && kmh < up[step] - hysteresis) --step;
  return step;
}

}  // namespace

extern "C" uint8_t pc_steer_adc(unsigned port, int8_t joyx, int32_t speed_kmh) {
  const Settings &cfg = settings();
  /* HANDLING (SLOW / NORMAL / QUICK): how fast the virtual wheel follows the stick or the keys; a wheel is direct. */
  const float handling = pc_saturn_handling_factor();
  PortState &st = g_ports[port < kPorts ? port : 0];
  const int source = pc_input_steer_source(port);
  float target = std::clamp(static_cast<float>(joyx) / 127.f, -1.f, 1.f);
  if (cfg.invert) target = -target;
  const float kmh = static_cast<float>(std::max<int32_t>(speed_kmh, 0));
  st.spring_step = spring_step(st.spring_step, kmh);
  if (source != st.last_source) {
    st.key_release_frames = 3;
    st.last_source = source;
  }

  if (source == PC_STEER_WHEEL) {
    /* A wheel: its position, direct, as the cabinet's potentiometer. */
    const float a = std::fabs(target);
    st.steer = std::copysign(cfg.wheel_sensitivity == 1.f ? a : std::pow(a, 1.f / cfg.wheel_sensitivity), target);
  } else if (source == PC_STEER_PAD && cfg.direct_pad) {
    st.steer = target;
  } else if (source == PC_STEER_PAD) {
    const float raw = std::fabs(target);
    const float a = raw <= cfg.stick_deadzone ? 0.f : (raw - cfg.stick_deadzone) / (1.f - cfg.stick_deadzone);
    constexpr float kSpeedCut = 0.45f, kSmooth = 0.18f;
    const float v = std::clamp(kmh / 215.f, 0.f, 1.2f);
    const float speed_fac = 1.f / (1.f + kSpeedCut * v * v);
    const float u = std::clamp((a - 0.85f) / 0.15f, 0.f, 1.f);
    const float curved = std::copysign(cfg.stick_range * std::pow(a, cfg.stick_expo) +
                                           (1.f - cfg.stick_range) * u * u * (3.f - 2.f * u),
                                       target);
    st.steer += (curved * std::sqrt(speed_fac) - st.steer) * std::min(1.f, kSmooth * speed_fac * handling);
  } else {
    /* Keys: a steady push against the spring for this speed. */
    constexpr float k0 = 7.f / 31.f;
    constexpr int kReleaseHold = 3;
    const float k = st.spring_step < 0 ? k0 : (7.f + 2.f * static_cast<float>(st.spring_step)) / 31.f;
    const float push = target;
    if (push != 0.f) {
      st.key_hold = push;
      st.key_release_frames = 0;
    } else if (st.key_release_frames < kReleaseHold) {
      st.key_release_frames++;
    }
    const float in = (push != 0.f || st.key_release_frames < kReleaseHold) ? st.key_hold : 0.f;
    const float t = std::clamp(in, -1.f, 1.f) * std::clamp(cfg.wheel_weight * std::sqrt(k0 / k), 0.f, 1.f);
    const bool out = t != 0.f && t * st.steer >= 0.f && std::fabs(t) > std::fabs(st.steer);
    const float rate = (out ? cfg.wheel_turnin * std::sqrt(k0 / k) : cfg.wheel_return * std::sqrt(k / k0)) * handling;
    st.steer += std::clamp(t - st.steer, -rate, rate);
  }
  const float s = std::clamp(st.steer, -1.f, 1.f);
  return static_cast<uint8_t>(std::lround(128.f + s * 96.f));
}
