/* Daytona USA PC: the game's sound from the arcade's own sound board program.
 *
 * On the Dreamcast, code/audio/sound.c plays the music as AICAflow recordings
 * and code/audio/sfx.c renders the effects into AICA banks.  On PC both are
 * replaced by this file: every packed command the game sends (status byte in
 * bits 23..16, the bytes the i960 queues for the sound board's UART at 19bf4,
 * audio/sound.h) goes unchanged to the recomp's NativeSoundEngine
 * (src/runtime/native_sound_engine.h), which runs the sound program's own
 * sequences (epr-16489/16490 or epr-16720/16721) over the original MultiPCM
 * samples (mpr-16491..16494) at 48 kHz.  Music, effects, the engine and the
 * announcer all come from the ROM set; nothing is pre-rendered.
 *
 * The sound images are read from ASSET_ROOT/sound/{sound_program,pcm1,pcm2}.bin,
 * written by platform/pc-native/recipes/build_pc_assets.sh with
 * platform/dreamcast-native/tools/extract_sound_roms.py.
 */
#include "app/native_audio.h"
#include "runtime/native_sound_engine.h"

extern "C" {
#include "audio/sfx.h"
#include "audio/sound.h"
}

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

using Audio = app::NativeAudio<snd::NativeSoundEngine>;

Audio *g_audio = nullptr;
float g_music_base = 1.f, g_sfx_base = 1.f, g_music_level = 1.f;   /* the options' BGM VOLUME scales the music */
bool g_mono = false;
bool g_on = false;
bool g_setup_sent = false;
uint64_t g_boot_ns = 0;
int g_hold = 0;
std::vector<uint32_t> g_held;   /* music starts deferred by nm_sound_hold(1) */
nm_sfx_stats_t g_sfx_stats{};
std::FILE *g_log = nullptr;      /* DAYTONA_SOUND_LOG: "<ms since boot> <bytes>" per send */
unsigned long g_commands = 0;

bool read_file(const std::string &path, size_t size, std::vector<uint8_t> &out)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.assign(size, 0);
    const size_t got = std::fread(out.data(), 1, size, f);
    std::fclose(f);
    return got == size;
}

void send_bytes(const uint8_t *bytes, size_t n)
{
    if (g_log != nullptr) {
        std::fprintf(g_log, "%llu", static_cast<unsigned long long>((SDL_GetTicksNS() - g_boot_ns) / 1000000u));
        for (size_t i = 0; i < n; i++) std::fprintf(g_log, " %02x", bytes[i]);
        std::fputc('\n', g_log);
    }
    if (g_audio != nullptr && !g_audio->send(bytes, n)) {
        static unsigned reported = 0;
        if (++reported <= 4) {
            std::fprintf(stderr, "Sound: command queue full, %zu bytes dropped\n", n);
        }
    }
}

void send_command(uint32_t command)
{
    const uint8_t bytes[3] = {static_cast<uint8_t>(command >> 16), static_cast<uint8_t>(command >> 8),
                              static_cast<uint8_t>(command)};
    send_bytes(bytes, sizeof bytes);
}

/* A sequence start on bank 0x10: the music (audio/music_map.h). */
bool is_music_start(uint32_t command)
{
    return ((command >> 16) & 0xf0u) == 0xa0u && ((command >> 8) & 0xffu) == 0x10u;
}

/* The controller burst the original sends after its sound board boots
 * (f8 f8 ff, then these; tools/music_notes.cpp, code/audio/sfx.c:205). The
 * Dreamcast game never sends it: its sfx.c hardcodes the result. */
const uint8_t kSetup[] = {0xbe, 0x14, 0x1f, 0xbe, 0x16, 0x02, 0xbe, 0x1b, 0x06, 0xbe, 0x1c, 0x03,
                          0xbe, 0x1d, 0x01, 0xbe, 0x1e, 0x02, 0xbe, 0x1f, 0x05, 0xbe, 0x36, 0x09,
                          0xbe, 0x17, 0x00, 0xbe, 0x18, 0x04, 0xbe, 0x35, 0x00, 0xbe, 0x34, 0x00};

float env_volume(const char *name, float fallback)
{
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return static_cast<float>(std::atof(v));
}

/* The Saturn horse's hoof sounds (code/pc_horse.h): sound/horse-hoof{0,1,2}.wav from the Saturn disc
 * (tools/extract_saturn_horse.py), each at 48 kHz, on streams of their own (one sound may still ring when the
 * next starts) mixed by SDL with the arcade engine's. */
struct Hoofs {
    SDL_AudioDeviceID device = 0;
    SDL_AudioStream *streams[4] = {};
    std::vector<uint8_t> sound[3];
    unsigned next = 0;
} g_hoofs;

void hoofs_open(const std::string &root, float gain)
{
    const SDL_AudioSpec out{SDL_AUDIO_S16, 1, 48000};
    unsigned loaded = 0;
    for (unsigned i = 0; i < 3; ++i) {
        SDL_AudioSpec spec;
        Uint8 *data = nullptr;
        Uint32 len = 0;
        const std::string path = root + "sound/horse-hoof" + std::to_string(i) + ".wav";
        if (!SDL_LoadWAV(path.c_str(), &spec, &data, &len)) continue;
        Uint8 *converted = nullptr;
        int converted_len = 0;
        if (SDL_ConvertAudioSamples(&spec, data, static_cast<int>(len), &out, &converted, &converted_len)) {
            g_hoofs.sound[i].assign(converted, converted + converted_len);
            SDL_free(converted);
            ++loaded;
        }
        SDL_free(data);
    }
    if (!loaded) return;
    g_hoofs.device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &out);
    if (!g_hoofs.device) return;
    for (SDL_AudioStream *&stream : g_hoofs.streams) {
        stream = SDL_CreateAudioStream(&out, &out);
        if (stream == nullptr || !SDL_BindAudioStream(g_hoofs.device, stream)) continue;
        SDL_SetAudioStreamGain(stream, gain);
    }
    std::printf("Sound: %u Saturn horse hoof sounds\n", loaded);
}

void hoofs_close()
{
    for (SDL_AudioStream *&stream : g_hoofs.streams) {
        if (stream != nullptr) SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
    if (g_hoofs.device) SDL_CloseAudioDevice(g_hoofs.device);
    g_hoofs.device = 0;
}

}  // namespace

extern "C" {

void pc_sound_horse_hoof(unsigned sound)
{
    if (!g_on || !g_hoofs.device || sound >= 3 || g_hoofs.sound[sound].empty()) return;
    SDL_AudioStream *stream = g_hoofs.streams[g_hoofs.next++ % 4];
    if (stream == nullptr) return;
    SDL_ClearAudioStream(stream);
    SDL_PutAudioStreamData(stream, g_hoofs.sound[sound].data(), static_cast<int>(g_hoofs.sound[sound].size()));
}

int nm_sound_init(const char *asset_root)
{
    if (g_audio != nullptr) {
        return g_on ? 1 : 0;
    }
    const std::string root = asset_root != nullptr ? asset_root : "";
    std::vector<uint8_t> program, pcm1, pcm2;
    if (!read_file(root + "sound/sound_program.bin", 0x40000, program) ||
        !read_file(root + "sound/pcm1.bin", 0x400000, pcm1) ||
        !read_file(root + "sound/pcm2.bin", 0x400000, pcm2)) {
        std::fprintf(stderr, "Sound: no sound images in %ssound/ (build_pc_assets.sh writes them); silent\n",
                     root.c_str());
        return 0;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "Sound: SDL audio: %s; silent\n", SDL_GetError());
        return 0;
    }
    std::unique_ptr<snd::NativeSoundEngine> engine;
    try {
        engine = std::make_unique<snd::NativeSoundEngine>(std::move(program), std::move(pcm1), std::move(pcm2));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Sound: the sound program was rejected (%s); silent\n", e.what());
        return 0;
    }
    g_audio = new Audio();
    if (!g_audio->open(std::move(engine)) || !g_audio->resume()) {
        std::fprintf(stderr, "Sound: no audio device (%s); silent\n", SDL_GetError());
        delete g_audio;
        g_audio = nullptr;
        return 0;
    }
    g_audio->volume(std::clamp(env_volume("DAYTONA_VOLUME", 0.8f), 0.f, 1.f));
    g_music_base = std::clamp(env_volume("DAYTONA_MUSIC_VOLUME", 1.f), 0.f, 1.f);
    g_sfx_base = std::clamp(env_volume("DAYTONA_SFX_VOLUME", 1.f), 0.f, 1.f);
    g_audio->volumes(g_music_base * g_music_level, g_sfx_base);
    g_audio->mono(g_mono);
    const uint8_t boot[] = {0xf8, 0xf8, 0xff};
    g_boot_ns = SDL_GetTicksNS();
    if (const char *log = std::getenv("DAYTONA_SOUND_LOG"); log != nullptr && *log != '\0') {
        g_log = std::fopen(log, "w");
    }
    send_bytes(boot, sizeof boot);
    hoofs_open(root, std::clamp(env_volume("DAYTONA_VOLUME", 0.8f), 0.f, 1.f) *
                     std::clamp(env_volume("DAYTONA_SFX_VOLUME", 1.f), 0.f, 1.f));
    g_setup_sent = false;
    g_on = true;
    return 1;
}

void nm_sound_command(uint32_t command)
{
    if (!g_on || command == 0u) {
        return;
    }
    if (g_hold && is_music_start(command)) {
        g_held.push_back(command);
        return;
    }
    ++g_commands;
    send_command(command);
}

void nm_sound_update(void)
{
    if (!g_on) {
        return;
    }
    /* The board's reset settles before the controller burst (music_notes.cpp waits 100 ms). */
    if (!g_setup_sent && SDL_GetTicksNS() - g_boot_ns >= 100000000ull) {
        send_bytes(kSetup, sizeof kSetup);
        g_setup_sent = true;
    }
}

void nm_sound_hold(int on)
{
    g_hold = on;
    if (!on) {
        for (uint32_t command : g_held) {
            send_command(command);
        }
        g_held.clear();
    }
}

uint32_t nm_sound_idle(uint32_t) { return 0; }
int nm_sound_loading(void) { return 0; }

void nm_sound_settle(void)
{
    /* Nothing loads: the sequences are in the sound program. Make sure the
     * controller burst precedes a scene's first command. */
    if (g_on && !g_setup_sent) {
        send_bytes(kSetup, sizeof kSetup);
        g_setup_sent = true;
    }
}

void nm_sound_shutdown(void)
{
    g_on = false;
    if (g_log != nullptr) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    if (g_audio != nullptr && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        const auto st = g_audio->stats();
        std::printf("Sound: %lu commands; %u callbacks, %u frames rendered, %u unsupported, %u invalid, %u overflows\n",
                    g_commands, st.callbacks, st.frames, st.unsupported, st.invalid, st.overflows);
    }
    if (g_audio != nullptr) {
        /* enDjinn's shutdown may already have called SDL_Quit (host_input.cpp),
         * which closed the device: then the stream is gone and must not be
         * touched again; the process is exiting. */
        if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            hoofs_close();
            g_audio->close();
            delete g_audio;
        }
        g_audio = nullptr;
    }
    g_held.clear();
}

/* audio/sfx.h: the effects are part of the same sound program on PC. */
int nm_sfx_init(const char *) { return 1; }
void nm_sfx_command(uint32_t) {}
void nm_sfx_update(void) {}
void nm_sfx_shutdown(void (*)(unsigned)) {}
const nm_sfx_stats_t *nm_sfx_stats(void) { return &g_sfx_stats; }
void nm_sfx_quiesce(void) {}
void nm_sfx_hint(const uint32_t *, unsigned) {}
uint32_t nm_sfx_idle(uint32_t) { return 0; }

}  // extern "C"

/* The Saturn-style options (pc_saturn_options.h): BGM VOLUME as a music level 0..1, SOUND MODE mono. */
extern "C" void pc_sound_options(float music_level, int mono)
{
    g_music_level = std::clamp(music_level, 0.f, 1.f);
    g_mono = mono != 0;
    if (g_audio) {
        g_audio->volumes(g_music_base * g_music_level, g_sfx_base);
        g_audio->mono(g_mono);
    }
}

/* The race's pause (pc_saturn_menu.h): everything silent while it is up. */
extern "C" void pc_sound_pause(int on)
{
    if (g_audio) g_audio->mute(on != 0);
}
