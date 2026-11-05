#include "runtime/native_sound_sequencer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace snd {
namespace {
// Format locations established from the imported driver's reachable command
// handlers. These are addresses, not copies of any ROM table or instruction.
// They are daytona93's; Revision A's sound program has the tables from
// kProfiles on 0x48 bytes later, their pointers with them (shift_).
constexpr uint32_t kSequenceBanks = 0x2bc6, kProfiles = 0x505a;
constexpr uint32_t kFinePitch = 0x523a, kBendCurves = 0x56ea;
constexpr uint32_t kNoteCodes = 0x5c1a, kVelocity = 0x5cce;
constexpr uint32_t kInstruments = 0x5dce, kPercussion = 0x645a;
constexpr uint32_t kLayoutShifts[] = {0x00, 0x48}; // daytona93, daytona (Revision A)
// The music format's tick is 8 MHz / 9216, established from timer-B reload
// 0xfc and the reference driver. Here it is a rational sample-clock divider.
constexpr uint64_t kTickNumerator = 8000000, kTickDenominator = 9216;
constexpr double kSampleRate = 10000000.0 / 224.0;
constexpr unsigned kEventLimit = 4096;
unsigned data_bytes(uint8_t status) { return (status & 0xe0) == 0xc0 ? 1 : 2; }
uint8_t volume_scale(unsigned value, unsigned master) {
    const unsigned result = (value * master) >> 8;
    return uint8_t(result ? result + 1 : 0);
}
uint8_t voice_level(uint8_t velocity, uint8_t volume) {
    return uint8_t(~((unsigned(velocity) * volume) >> 8)) >> 1;
}
}

NativeSoundSequencer::NativeSoundSequencer(const std::vector<uint8_t> &program, uint32_t output_rate)
    : program_(program), output_rate_(output_rate) {
    if (program.size() < 0x40000 || output_rate < 8000 || output_rate > 192000)
        throw std::invalid_argument("native sound requires a complete imported program and valid output rate");
    // Which layout: Revision A's when its profile and instrument tables'
    // first entries point just past each table (0x40 and 0x200 bytes on), as
    // in both known drivers; daytona93's otherwise.
    for (uint32_t shift : kLayoutShifts)
        if (pointer(kProfiles + shift) == kProfiles + shift + 0x40 &&
            pointer(kInstruments + shift) == kInstruments + shift + 0x200) {
            shift_ = shift;
            break;
        }
    stats_ = {};
    reset();
}

bool NativeSoundSequencer::valid(uint32_t address, size_t bytes) {
    if (address > program_.size() || bytes > program_.size() - address) {
        ++stats_.invalid_data;
        return false;
    }
    return true;
}
uint8_t NativeSoundSequencer::byte(uint32_t address) {
    return valid(address, 1) ? program_[address] : 0;
}
uint16_t NativeSoundSequencer::word(uint32_t address) {
    return valid(address, 2) ? uint16_t((program_[address] << 8) | program_[address + 1]) : 0;
}
uint32_t NativeSoundSequencer::pointer(uint32_t address) {
    if (!valid(address, 4)) return 0;
    const uint32_t result = (uint32_t(program_[address]) << 24) | (uint32_t(program_[address + 1]) << 16) |
                            (uint32_t(program_[address + 2]) << 8) | program_[address + 3];
    // The sound program's upper half also has a mirrored address in its data.
    return result >= 0x80000 && result < 0xa0000 ? result - 0x60000 : result;
}

void NativeSoundSequencer::reset() {
    for (auto &voice : voices_) if (voice.active) stop_voice(voice, true);
    channels_ = {}; voices_ = {}; tracks_ = {}; bend_ = {}; hold_ = {};
    banks_ = {}; controls_ = {}; allocation_ = {}; uart_data_ = {};
    uart_status_ = uart_used_ = 0; master_volume_ = 255; voice_age_ = 0;
    // Preserve the monotonic output position when an incoming reset occurs.
    tick_phase_ = 0; percussion_countdown_ = 257;
    for (unsigned i = 0; i < voices_.size(); ++i) {
        voices_[i].event.voice_id = uint8_t(i);
        voices_[i].event.rom = uint8_t(i / 28);
        voices_[i].reserved = i >= 46;
    }
    load_channels(0, true);
}

void NativeSoundSequencer::load_channels(unsigned profile, bool initial) {
    const uint32_t base = pointer(kProfiles + shift_ + 4 * profile);
    if (!valid(base, 101)) return;
    auto load = [&](unsigned index, uint32_t address) {
        auto &c = channels_[index];
        c.flags = byte(address); c.channel = byte(address + 1);
        c.volume = byte(address + 2); c.program = byte(address + 3);
        c.transpose = int8_t(byte(address + 4)); c.modulation = byte(address + 6);
        c.pan_device = byte(address + 7); c.bend_range = byte(address + 9);
        c.effective_volume = volume_scale(c.volume, c.flags & 1 ? 255 : master_volume_);
    };
    for (unsigned i = 0; i < 10; ++i) load(i, base + 10 * i);
    banks_[0] = byte(base + 100) & 3;
    if (initial) {
        banks_[1] = banks_[0];
        const uint32_t effects = pointer(kProfiles + shift_ + 4);
        if (valid(effects, 60)) for (unsigned i = 0; i < 6; ++i) load(i + 10, effects + 10 * i);
    }
}

void NativeSoundSequencer::send(const uint8_t *data, size_t bytes) {
    if (!data && bytes) { ++stats_.invalid_data; return; }
    stats_.input_bytes += bytes;
    for (size_t i = 0; i < bytes; ++i) {
        const uint8_t value = data[i];
        if (value == 0xf8) continue;
        if (value == 0xff) { reset(); continue; }
        if (value & 0x80) { uart_status_ = value; uart_used_ = 0; continue; }
        if (!uart_status_) { ++stats_.invalid_data; continue; }
        uart_data_[uart_used_++] = value;
        if (uart_used_ == data_bytes(uart_status_)) {
            message(uart_status_, uart_data_[0], uart_used_ == 2 ? uart_data_[1] : 0, false);
            uart_used_ = 0;
        }
    }
}

void NativeSoundSequencer::message(uint8_t status, uint8_t a, uint8_t b, bool sequence) {
    ++stats_.messages;
    if (sequence) ++stats_.sequence_events;
    const uint8_t channel = status & 15;
    for (auto &c : channels_) {
        if (!(c.flags & 0x80) || c.channel != channel) continue;
        switch (status & 0xf0) {
        case 0x80: note_off(c, a); break;
        case 0x90: if (b) note_on(c, a, b); else note_off(c, a); break;
        case 0xa0: start_sequence(a, b); break;
        case 0xb0: controller(c, a, b); break;
        case 0xc0:
            c.program = a;
            for (auto &v : voices_) if (v.active && v.channel == channel && v.event.rom == (c.pan_device & 1)) stop_voice(v);
            break;
        case 0xe0: {
            const unsigned bend = (unsigned(a) | (unsigned(b) << 7)) >> 5;
            unsigned magnitude = bend & 255;
            if (!(bend & 256)) { magnitude = uint8_t(-int(magnitude)); if (!magnitude) magnitude = 255; }
            const uint32_t curve = pointer(kBendCurves + shift_ + 4 * (c.bend_range & 127));
            const int amount = byte(curve + magnitude);
            bend_[channel] = int16_t(bend & 256 ? amount : -amount);
            update_channel(c, true);
            break;
        }
        // The original driver consumes these messages without an audio action.
        case 0xd0: case 0xf0: break;
        default: ++stats_.unsupported; break;
        }
    }
}

uint16_t NativeSoundSequencer::note_pitch(uint8_t note_code, int tuning) {
    const uint32_t center = pointer(kFinePitch + shift_ + 4 * (note_code & 15));
    const int64_t address = int64_t(center) + 2 * tuning;
    if (address < 0 || address > std::numeric_limits<uint32_t>::max()) { ++stats_.invalid_data; return 0; }
    const uint16_t fine = word(uint32_t(address));
    return uint16_t((uint16_t(uint8_t((note_code & 0xf0) + (fine >> 8))) << 8) | (fine & 255));
}

NativeSoundSequencer::Voice &NativeSoundSequencer::allocate(unsigned rom) {
    const unsigned start = allocation_[rom];
    for (unsigned n = 0; n < 28; ++n) {
        const unsigned slot = (start + n) % 28;
        auto &v = voices_[rom * 28 + slot];
        if (!v.active && !v.reserved) { allocation_[rom] = (slot + 1) % 28; return v; }
    }
    Voice *oldest = nullptr;
    for (unsigned n = 0; n < 28; ++n) {
        auto &v = voices_[rom * 28 + n];
        if (!v.reserved && (!oldest || v.age < oldest->age)) oldest = &v;
    }
    stop_voice(*oldest, true);
    return *oldest;
}

void NativeSoundSequencer::note_on(Channel &c, uint8_t note, uint8_t velocity) {
    unsigned rom = c.pan_device & 1;
    uint8_t pan = c.pan_device, sample = 0, duration = 0;
    int8_t tuning = 0;
    const uint8_t transposed = uint8_t(note + c.transpose);
    uint8_t note_code = 0;
    uint16_t pitch = 0;
    const bool drum = c.program >= 0xf0;
    if (drum) {
        const uint32_t table = pointer(kPercussion + shift_ + 4 * (c.program - 0xf0));
        const uint32_t entry = table + 4 * (note & 127);
        if (!valid(entry, 4)) return;
        pan = byte(entry);
        if (pan == 255) return;
        if (pan == 128) pan = c.pan_device;
        rom = pan & 1;
        pitch = uint16_t(byte(entry + 1)) << 8;
        duration = byte(entry + 2); sample = byte(entry + 3);
        note_code = byte(kNoteCodes + shift_ + (transposed & 127));
    } else {
        uint32_t split = pointer(kInstruments + shift_ + 4 * (c.program & 127)) + 1;
        unsigned limit = 0;
        while (valid(split, 4) && transposed >= byte(split)) {
            split += 4;
            if (++limit == 128) { ++stats_.invalid_data; return; }
        }
        if (failed()) return;
        sample = byte(split + 1); tuning = int8_t(byte(split + 2));
        note_code = byte(kNoteCodes + shift_ + (uint8_t(transposed + byte(split + 3)) & 127));
        pitch = note_pitch(note_code, bend_[c.channel] + tuning);
    }
    auto &voice = allocate(rom);
    voice.active = true; voice.drum = drum; voice.held = false;
    voice.channel = c.channel; voice.note_code = note_code;
    voice.event.music = music_channel(c.channel);
    voice.velocity = byte(kVelocity + shift_ + (velocity & 127)); voice.tuning = tuning;
    voice.age = ++voice_age_; voice.lifetime = duration;
    voice.event.bank = banks_[rom];
    voice.event.sample_index = uint16_t(sample | ((pitch & 1) << 8));
    update_voice(voice, pitch, voice_level(voice.velocity, c.effective_volume), pan >> 4, EventKind::NoteOn);
}

void NativeSoundSequencer::note_off(Channel &c, uint8_t note) {
    if (c.program >= 0xf0) return;
    const uint8_t transposed = uint8_t(note + c.transpose);
    uint32_t split = pointer(kInstruments + shift_ + 4 * (c.program & 127)) + 1;
    for (unsigned n = 0; n < 128 && valid(split, 4) && transposed >= byte(split); ++n) split += 4;
    if (!valid(split, 4)) return;
    const uint8_t sample = byte(split + 1);
    const uint8_t note_code = byte(kNoteCodes + shift_ + (uint8_t(transposed + byte(split + 3)) & 127));
    for (auto &v : voices_) if (v.active && !v.drum && v.channel == c.channel &&
        v.event.rom == (c.pan_device & 1) && v.note_code == note_code && (v.event.sample_index & 255) == sample) {
        if (hold_[c.channel]) v.held = true; else stop_voice(v);
    }
}

#ifdef M2_DC_NATIVE_HOST
bool NativeSoundSequencer::tracks_idle() const {
    for (const auto &track : tracks_) if (track.active) return false;
    return true;
}
#endif

void NativeSoundSequencer::emit(Voice &v, EventKind kind) {
    v.event.frame = frame_; v.event.kind = kind;
    if (kind == EventKind::NoteOn) ++stats_.note_ons;
    else if (kind == EventKind::NoteOff || kind == EventKind::Stop) ++stats_.note_offs;
    else ++stats_.updates;
    if (sink_) sink_(context_, v.event);
}
void NativeSoundSequencer::stop_voice(Voice &v, bool immediate) {
    if (!v.active) return;
    v.active = false; v.held = false; v.lifetime = 0;
    emit(v, immediate ? EventKind::Stop : EventKind::NoteOff);
}

void NativeSoundSequencer::update_voice(Voice &v, uint16_t pitch, uint8_t level, uint8_t pan, EventKind kind) {
    v.rate_code = pitch; v.pan = pan;
#ifdef M2_DC_NATIVE_HOST
    v.event.rate_code = pitch; v.event.level = level; v.event.pan_code = pan;
#endif
    const unsigned fraction = ((pitch >> 8) & 15) * 64 + ((pitch & 255) >> 2);
    int octave = int(((pitch >> 12) - 1) & 15); if (octave >= 8) octave -= 16;
    v.event.source_rate_hz = std::ldexp(kSampleRate * (1024.0 + fraction) / 1024.0, octave);
    double left = 1, right = 1;
    if (pan == 8) left = right = 0;
    else if (pan & 8) right = (16 - pan) == 7 ? 0 : std::pow(10.0, -0.15 * (16 - pan));
    else if (pan) left = pan == 7 ? 0 : std::pow(10.0, -0.15 * pan);
    // Preserve the table's channel gains using the mixer's equal-power pan;
    // the mixer's native output gain is applied separately from these events.
    v.event.gain = float(0.25 * std::pow(10.0, -0.01875 * level) * std::hypot(left, right));
    v.event.pan = left + right ? float(std::atan2(right, left) * (4.0 / 3.14159265358979323846) - 1.0) : 0.f;
    emit(v, kind);
}

void NativeSoundSequencer::update_channel(Channel &c, bool retune, bool repan) {
    c.effective_volume = volume_scale(c.volume, c.flags & 1 ? 255 : master_volume_);
    for (auto &v : voices_) if (v.active && !v.reserved && v.channel == c.channel && v.event.rom == (c.pan_device & 1)) {
        const uint16_t rate = retune ? note_pitch(v.note_code, bend_[c.channel] + v.tuning) : v.rate_code;
        update_voice(v, rate, voice_level(v.velocity, c.effective_volume),
                     repan ? c.pan_device >> 4 : v.pan, EventKind::Update);
    }
}

void NativeSoundSequencer::controller(Channel &c, uint8_t control, uint8_t value) {
    controls_[control & 127] = value;
    switch (control) {
    case 0x07: c.volume = uint8_t(value ? value * 2 + 1 : 0); update_channel(c); break;
    case 0x10:
        master_volume_ = uint8_t(value ? value * 2 + 1 : 0);
        for (unsigned i = 0; i < 10; ++i) update_channel(channels_[i]);
        break;
    case 0x0a: {
        unsigned pan = value >> 3;
        pan = pan >= 8 ? pan & 7 : pan ? pan | 8 : 9;
        c.pan_device = uint8_t((pan << 4) | (c.pan_device & 1)); update_channel(c, false, true); break;
    }
    case 0x40:
        hold_[c.channel] = value < 64;
        if (value >= 64) for (auto &v : voices_) if (v.active && v.channel == c.channel && v.held) stop_voice(v);
        break;
    case 0x7b:
        for (auto &v : voices_) if (v.active && v.channel == c.channel && v.event.rom == (c.pan_device & 1)) stop_voice(v);
        break;
    case 0x50:
        for (auto &v : voices_) {
            const bool music = music_channel(v.channel);
            if (v.active && (value == 0x7d ? !music && !v.reserved : value == 0x7e ? music : true)) stop_voice(v);
        }
        bend_ = {}; hold_ = {};
        break;
    case 0x20: load_channels(value); break;
    case 0x21: banks_[c.pan_device & 1] = value & 3; break;
    case 0x1a: tracks_[0].fade_interval = tracks_[0].fade_count = value; break;
    case 0x01: if (value) ++stats_.unsupported; break; // Native mixer LFO not yet implemented.
    case 0x14: break; // Deliberately ignored by the original command handler.
    case 0x16: case 0x17: case 0x18: case 0x19: case 0x1b: case 0x1c:
    case 0x1d: case 0x1e: case 0x1f: case 0x34: case 0x35: case 0x36: case 0x37:
        update_engines(); break;
    default: ++stats_.unsupported; break;
    }
}

void NativeSoundSequencer::start_sequence(uint8_t bank, uint8_t index) {
    const uint32_t table = pointer(kSequenceBanks + 4 * (bank & 127));
    if (!valid(table, 2) || index > word(table)) return;
    uint32_t descriptor = pointer(table + 2 + 4 * index);
    if (!valid(descriptor, 2)) return;
    const uint8_t flags = byte(descriptor++);
    unsigned track_id = 0;
    if (flags & 128) {
        const unsigned channel = byte(descriptor) & 15;
        if (channel < 10 || channel > 14) { ++stats_.invalid_data; return; }
        track_id = channel - 9;
    }
    auto &track = tracks_[track_id];
    track = {}; track.active = true; track.delay = 1;
    if (flags & 128) track.cursor = descriptor;
    else {
        track.playlist = true;
        track.cursor = pointer(descriptor + 3);
        track.next_section = descriptor + 7;
    }
}

void NativeSoundSequencer::next_section(Track &track) {
    if (!track.playlist) { track.active = false; return; }
    uint32_t cursor = pointer(track.next_section);
    if (cursor == 0xffffffff) { track.active = false; return; }
    if (cursor == 0xfffffff1) {
        track.next_section = pointer(track.next_section + 4);
        cursor = pointer(track.next_section);
    }
    track.next_section += 4;
    if (!valid(cursor, 1)) { track.active = false; return; }
    track.cursor = cursor;
}

void NativeSoundSequencer::tick_track(Track &track) {
    if (!track.active || (track.delay && --track.delay)) return;
    for (unsigned work = 0; work < kEventLimit && track.active && !failed(); ++work) {
        uint8_t status = byte(track.cursor++);
        if (status & 128) track.status = status;
        else { status = track.status; --track.cursor; }
        bool immediate = false;
        if ((status & 0xf0) == 0xf0) {
            if (status == 0xff) {
                const uint8_t type = byte(track.cursor++);
                if (type == 0x2f) { next_section(track); continue; }
                const uint8_t length = byte(track.cursor++); track.cursor += length;
            } else if (status == 0xf7) { const uint8_t length = byte(track.cursor++); track.cursor += length; }
            else if (status == 0xf0) {
                unsigned length = 0;
                do {
                    ++track.cursor; // This format stores paired bytes, terminated by the second byte.
                    if (byte(track.cursor++) == 0xf7) break;
                } while (++length < kEventLimit && !failed());
                if (length == kEventLimit) { ++stats_.event_limit_hits; track.active = false; return; }
            } else { next_section(track); continue; }
        } else {
            uint8_t a = byte(track.cursor++), b = 0;
            if ((status & 0xf0) == 0x80) { immediate = a & 128; a &= 127; b = 127; }
            else if (data_bytes(status) == 1) { immediate = a & 128; a &= 127; }
            else { b = byte(track.cursor++); immediate = b & 128; b &= 127; }
            if ((status & 0xf0) == 0xb0 && a == 0x10) {
                if (track.fade_interval) b = track.volume; else track.volume = b;
            }
            message(status, a, b, true);
        }
        if (!track.active || immediate) continue;
        unsigned delay = byte(track.cursor++);
        if (delay & 128) delay = ((delay & 127) << 7) | byte(track.cursor++);
        if (delay) { track.delay = delay; return; }
    }
    if (track.active && !failed()) { ++stats_.event_limit_hits; track.active = false; }
}

void NativeSoundSequencer::tick() {
    auto &music = tracks_[0];
    if (music.fade_interval && --music.fade_count == 0) {
        if (music.volume < 4) {
            music.active = false; music.fade_interval = 0;
            message(0xbe, 0x50, 0x7e, true);
        } else {
            music.volume -= 4; music.fade_count = music.fade_interval;
            message(0xbe, 0x10, music.volume, true);
        }
    }
    for (auto &track : tracks_) tick_track(track);
    // Percussion durations share a coarse articulation pulse in the music
    // format: the first sweep follows 257 ticks, then one every 36 ticks.
    if (--percussion_countdown_ == 0) {
        percussion_countdown_ = 36;
        for (auto &voice : voices_) if (voice.active && voice.drum) {
            voice.lifetime = uint8_t(voice.lifetime - 1);
            if (!voice.lifetime) stop_voice(voice);
        }
    }
}

void NativeSoundSequencer::advance(size_t output_frames) {
    const uint64_t period = uint64_t(output_rate_) * kTickDenominator;
    while (output_frames) {
        const uint64_t to_tick = (period - tick_phase_ + kTickNumerator - 1) / kTickNumerator;
        const uint64_t step = std::min<uint64_t>(output_frames, to_tick);
        frame_ += step; output_frames -= size_t(step); tick_phase_ += step * kTickNumerator;
        if (tick_phase_ >= period) { tick_phase_ -= period; if (!failed()) tick(); }
    }
}

void NativeSoundSequencer::update_engines() {
    // Native continuous effect layers: ROM tables give rate/gain for each RPM
    // or effect-intensity value. Sample selections are separate commands.
    struct Layer { unsigned slot, sample_control, pitch_table, level_table; };
    constexpr Layer layers[] = {
        {25,0x16,0x18d4,0x1854}, {24,0x1b,0x18d4,0x1a86},
        {23,0x1c,0x1d38,0x1cb8}, {27,0x1d,0x1f6c,0x1eec},
        {26,0x1d,0x2120,0x1eec}, {22,0x1e,0x2344,0x22c4}
    };
    auto layer = [&](unsigned slot, unsigned sample, uint16_t pitch, uint8_t level, uint8_t pan, bool on) {
        auto &v = voices_[28 + slot];
        if (!on) { stop_voice(v); return; }
        const bool starting = !v.active;
        v.active = true; v.channel = 10; v.event.music = false; v.event.bank = banks_[1];
        if (starting) v.event.sample_index = uint16_t(sample | ((pitch & 1) << 8));
        update_voice(v, pitch, level >> 1, pan, starting ? EventKind::NoteOn : EventKind::Update);
    };
    const unsigned rpm = controls_[0x17] & 127;
    for (const auto &l : layers)
        layer(l.slot, controls_[l.sample_control], word(l.pitch_table + 2 * rpm), byte(l.level_table + rpm), 0, rpm != 0);
    layer(20, controls_[0x1f], word(0x18d4 + 2 * rpm), controls_[0x34] ? controls_[0x34] : 255, 7, rpm != 0);
    layer(19, controls_[0x1f], word(0x18d4 + 2 * rpm), controls_[0x35] ? controls_[0x35] : 255, 9, rpm != 0);
    const unsigned effect = controls_[0x19] & 15;
    layer(21, controls_[0x18], word(0x2528 + 2 * effect), byte(0x2518 + effect), byte(0x254a + effect) >> 4, effect != 0);
    const unsigned second = controls_[0x37] & 15;
    layer(18, controls_[0x36], word(0x27ce + 2 * second), byte(0x27be + second), 0, second != 0);
}

} // namespace snd
