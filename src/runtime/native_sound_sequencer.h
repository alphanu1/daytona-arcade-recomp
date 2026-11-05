// Direct command and music-event sequencing for the imported Daytona sound
// data. No processor context, instruction execution, chip registers or timers.
// Tables and sequences remain in the user's immutable imported ROM image.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace snd {

class NativeSoundSequencer {
public:
    // The driver's music channels: the ones its "stop music" command (0x50
    // 0x7e) stops. The rest, and the engine layers, are effects.
    static bool music_channel(unsigned channel) { return channel <= 9 || channel == 15; }
    enum class EventKind : uint8_t { NoteOn, NoteOff, Update, Stop };
    struct VoiceEvent {
        uint64_t frame = 0;
        EventKind kind = EventKind::Stop;
        uint8_t voice_id = 0, rom = 0, bank = 0;
        uint16_t sample_index = 0;
        double source_rate_hz = 0;
        float gain = 0, pan = 0;
        bool music = false; // a music channel's (0-9, 15), else an effect's: the launcher's two volumes
#ifdef M2_DC_NATIVE_HOST
        // The same voice as the driver's MultiPCM register values: pitch word
        // (octave/FNS, bit 0 the sample's bit 8), total level 0..127 and pan
        // nibble. For offline tools that author chip-level streams.
        uint16_t rate_code = 0;
        uint8_t level = 0, pan_code = 0;
#endif
    };
    struct Stats {
        uint64_t input_bytes = 0, messages = 0, sequence_events = 0;
        uint64_t note_ons = 0, note_offs = 0, updates = 0;
        uint64_t unsupported = 0, invalid_data = 0, event_limit_hits = 0;
    };
    using Sink = void (*)(void *, const VoiceEvent &);

    // The immutable ROM must outlive this sequencer. All time is measured in
    // output samples, so music continues independently of graphics cadence.
    explicit NativeSoundSequencer(const std::vector<uint8_t> &program, uint32_t output_rate = 48000);
    void set_sink(Sink sink, void *context) { sink_ = sink; context_ = context; }
    void reset();
    void send(const uint8_t *data, size_t bytes);
    void advance(size_t output_frames);
    uint64_t frames() const { return frame_; }
    uint32_t output_rate() const { return output_rate_; }
    const Stats &stats() const { return stats_; }
    bool failed() const { return stats_.invalid_data != 0 || stats_.event_limit_hits != 0; }
#ifdef M2_DC_NATIVE_HOST
    // No music or effect sequence running (voices may still sound).
    bool tracks_idle() const;
#endif

private:
    struct Channel {
        uint8_t flags = 0, channel = 0, volume = 0, program = 0;
        int8_t transpose = 0;
        uint8_t modulation = 0, pan_device = 0, effective_volume = 0, bend_range = 0;
    };
    struct Voice {
        VoiceEvent event;
        bool active = false, drum = false, held = false, reserved = false;
        uint8_t channel = 0, note_code = 0, velocity = 0, pan = 0;
        uint16_t rate_code = 0;
        int8_t tuning = 0;
        uint32_t age = 0, lifetime = 0;
    };
    struct Track {
        bool active = false, playlist = false;
        uint32_t cursor = 0, next_section = 0, delay = 0;
        uint8_t status = 0, volume = 0, fade_interval = 0, fade_count = 0;
    };
    const std::vector<uint8_t> &program_;
    uint32_t output_rate_;
    uint32_t shift_ = 0; // where the tables are: daytona93's layout, or Revision A's (0x48 on)
    Sink sink_ = nullptr;
    void *context_ = nullptr;
    Stats stats_;
    uint64_t frame_ = 0, tick_phase_ = 0;
    uint32_t voice_age_ = 0;
    uint16_t percussion_countdown_ = 257;
    std::array<Channel, 16> channels_{};
    std::array<Voice, 56> voices_{};
    std::array<Track, 6> tracks_{};
    std::array<int16_t, 16> bend_{};
    std::array<bool, 16> hold_{};
    std::array<uint8_t, 2> banks_{};
    std::array<uint8_t, 128> controls_{};
    std::array<unsigned, 2> allocation_{};
    uint8_t master_volume_ = 255, uart_status_ = 0, uart_used_ = 0;
    std::array<uint8_t, 2> uart_data_{};

    uint8_t byte(uint32_t address);
    uint16_t word(uint32_t address);
    uint32_t pointer(uint32_t address);
    bool valid(uint32_t address, size_t bytes);
    void load_channels(unsigned profile, bool initial = false);
    void message(uint8_t status, uint8_t a, uint8_t b, bool sequence);
    void controller(Channel &channel, uint8_t control, uint8_t value);
    void note_on(Channel &channel, uint8_t note, uint8_t velocity);
    void note_off(Channel &channel, uint8_t note);
    void start_sequence(uint8_t bank, uint8_t index);
    void tick();
    void tick_track(Track &track);
    void next_section(Track &track);
    void update_engines();
    void update_voice(Voice &voice, uint16_t pitch, uint8_t level, uint8_t pan, EventKind kind);
    void emit(Voice &voice, EventKind kind);
    void stop_voice(Voice &voice, bool immediate = false);
    void update_channel(Channel &channel, bool retune = false, bool repan = false);
    uint16_t note_pitch(uint8_t note_code, int tuning);
    Voice &allocate(unsigned rom);
};

} // namespace snd
