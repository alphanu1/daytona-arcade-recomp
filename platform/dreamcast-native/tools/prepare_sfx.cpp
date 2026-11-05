// The original's sound effects and voices as AICAflow inputs (host only; recipes/build_sfx.sh).
//
//   prepare_sfx render SOUND_DIR OUT_DIR
//       SOUND_DIR: sound_program.bin, pcm1.bin, pcm2.bin (tools/extract_sound_roms.py).
//       The native sequencer (src/runtime/native_sound_sequencer.*) plays each command the
//       game can send for an effect, from the state the original's boot bytes leave (f8 f8 ff,
//       the frame-179 controller burst): every sequence of banks 0x20..0x22 (ae20xx..ae22xx)
//       and every note of channel 14 (9exx7f). Each becomes OUT_DIR/vgm/KEY.vgm, a one-chip
//       MultiPCM log in the subset AICAforge's afx_vgm reads, with its own small sample ROM
//       (the samples it plays, renumbered) and its voices on the lowest free slots, so the
//       flow afx_vgm makes needs as many AICA channels as voices sound together. The ten
//       continuous engine layers (be16..be1f, be34..be37) become OUT_DIR/vgm/eN.vgm: the
//       layer's sample keyed on muted; `park` makes them SH4-controlled flows, and
//       OUT_DIR/engine.tbl holds their AICA pitch, level and pan words for every value of the
//       controllers. OUT_DIR/render.tsv lists every flow (pack reads it).
//   prepare_sfx park IN.afx OUT.afx
//       A finite afx_vgm flow whose last instruction is END, as a controlled flow ending in
//       PARK (flags CONTROLLED): it keeps its voice until the SH4 stops it.
//   prepare_sfx pack RENDER.tsv ENGINE.tbl|'' GAINS.bin FLOW_DIR OUT.dsx
//       The runtime's one file besides the bank: index, velocity gains, engine tables and
//       every flow of RENDER.tsv that FLOW_DIR holds (KEY.afx, all bound to one merged bank),
//       read by code/audio/sfx.c. OUT_DIR/gains.bin of render: the velocity gains.
//
// Time: the sequencer runs at 44100 Hz, the VGM sample rate. The voice's register values
// come from the sequencer's events (rate_code, level, pan_code). MultiPCM samples always loop;
// a sample whose loop holds silence is a one-shot and is keyed off when its playback reaches
// the loop (at the pitch it started with); a real loop is held until the sequence keys it off,
// or for at most HOLD_SECONDS. Every output here is ROM-derived: keep it under build/.
#include "runtime/native_sound_sequencer.h"

extern "C" {
#include <aicaflow/codec.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using Sequencer = snd::NativeSoundSequencer;
constexpr uint32_t kRate = 44100;
constexpr uint32_t kClock = 8000000;     // The Model 1 sound board's MultiPCM clock (vgmrips' logs: 0x7a1200).
constexpr double kGainDb = -6.4;         // AICAforge afx_vgm's default, as recipes/build_music.sh.
constexpr double kSequenceSeconds = 20, kNoteSeconds = 8, kTailSeconds = 0.4;
constexpr unsigned kSlots = 28;

std::vector<uint8_t> read_file(const std::string &path, size_t size) {
    std::vector<uint8_t> data(size);
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f || std::fread(data.data(), 1, size, f) != size) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(2); }
    std::fclose(f);
    return data;
}
std::vector<uint8_t> read_whole(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(2); }
    std::vector<uint8_t> data;
    uint8_t buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) data.insert(data.end(), buffer, buffer + n);
    std::fclose(f);
    return data;
}
void write_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(data.data(), 1, data.size(), f) != data.size() || std::fclose(f)) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str()); std::exit(2);
    }
}
void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void put16(std::vector<uint8_t> &v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void set32(std::vector<uint8_t> &v, size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i)); }

// --- MultiPCM register words -> AICA words, as AICAforge afx_vgm.c lowers them (source_pitch,
// source_pan, source_mix at the pinned AICAforge e77de85), so that live engine patches agree
// with the setups afx_vgm authors for the same voice.
uint16_t aica_pitch(uint16_t rate_code) {
    const uint8_t r2 = uint8_t(rate_code), r3 = uint8_t(rate_code >> 8);
    int octave = ((r3 >> 4) - 1) & 15;
    if (octave & 8) octave -= 16;
    const int fns = (r3 & 15) * 64 + (r2 >> 2);
    const double value = std::ldexp((1024.0 + fns) / 1024.0, octave) * (double(kClock) / 180.0 / kRate);
    octave = int(std::floor(std::log2(value)));
    int fraction = int(std::nearbyint(1024.0 * (value / std::exp2(octave) - 1.0)));
    octave = std::clamp(octave, -8, 7);
    fraction = std::clamp(fraction, 0, 1023);
    return uint16_t((octave & 15) << 11 | fraction);
}
uint16_t aica_direct(uint8_t pan) {
    pan &= 15;
    if (pan == 8) return 0;
    if (pan < 7) return uint16_t(0x0f00 | pan);
    if (pan == 7) return 0x0f0f;
    if (pan < 15) return uint16_t(0x0f10 | (pan - 8));
    return 0x0f1f;
}
uint16_t aica_mix(uint8_t pan, uint8_t level) {
    const double attenuation = std::fmax(0.0, 1200.0 - kGainDb * 100.0 + (level & 127) * 37.5);
    int tl = (pan & 15) == 8 ? 255 : int(std::nearbyint(attenuation / 40.0));
    if (tl > 255) tl = 255;
    return uint16_t(tl << 8 | 0x24);
}

struct Roms {
    std::vector<uint8_t> program, pcm[2];
};

// One sample of the original's tables: header (12 bytes) and where its data is.
struct Source {
    unsigned rom, bank, index;
    uint32_t physical, frames, loop;
    std::array<uint8_t, 12> header;
    bool silent_loop;
};
bool source_for(const Roms &roms, unsigned rom, unsigned bank, unsigned index, Source &out) {
    const auto &pcm = roms.pcm[rom];
    if (index >= 512) return false;
    const uint8_t *h = pcm.data() + index * 12;
    const uint32_t address = uint32_t(h[0]) << 16 | uint32_t(h[1]) << 8 | h[2];
    if (address & 0x400000) return false; // 12-bit samples: none in this set, afx_vgm takes 8-bit only.
    const uint32_t logical = address & 0x3fffff;
    const uint32_t frames = 0x10000u - (uint32_t(h[5]) << 8 | h[6]), loop = uint32_t(h[3]) << 8 | h[4];
    if (!frames || loop >= frames || logical >= 0x200000) return false;
    const uint32_t physical = logical < 0x100000 ? logical : bank * 0x100000 + logical - 0x100000;
    if (physical + frames > pcm.size()) return false;
    out = {rom, bank, index, physical, frames, loop, {}, true};
    std::memcpy(out.header.data(), h, 12);
    for (uint32_t i = loop; i < frames; ++i) {
        const int value = int8_t(pcm[physical + i]);
        if (value > 4 || value < -4) { out.silent_loop = false; break; }
    }
    return true;
}
double source_hz(uint16_t rate_code) {
    const uint8_t r2 = uint8_t(rate_code), r3 = uint8_t(rate_code >> 8);
    int octave = ((r3 >> 4) - 1) & 15;
    if (octave & 8) octave -= 16;
    const int fns = (r3 & 15) * 64 + (r2 >> 2);
    return std::ldexp((1024.0 + fns) / 1024.0, octave) * (double(kClock) / 180.0);
}

// One effect being recorded as a VGM.
class Recorder {
public:
    explicit Recorder(const Roms &roms) : roms_(roms) { voice_slot_.fill(-1); }
    void start(uint64_t frame) { start_ = last_ = frame; }
    void event(const Sequencer::VoiceEvent &e) {
        if (e.voice_id >= 56) { ++rejected_; return; }
        if (e.kind == Sequencer::EventKind::NoteOn) note_on(e);
        else if (e.kind == Sequencer::EventKind::Update) update(e);
        else key_off_voice(e.voice_id, e.frame);
    }
    // Keys off every one-shot whose playback has reached its silent loop by `frame`.
    void expire(uint64_t frame) {
        for (unsigned s = 0; s < kSlots; ++s)
            if (slots_[s].active && slots_[s].ends && slots_[s].ends <= frame) key_off_slot(s, slots_[s].ends);
    }
    unsigned sounding() const {
        unsigned n = 0;
        for (const auto &s : slots_) n += s.active;
        return n;
    }
    // Ends the log: holds still sounding are keyed off at `frame`, then the release tail.
    void stop_at(uint64_t frame) { stop_ = frame; }
    // As finish, but the voices stay keyed on: an engine layer (park makes it a controlled flow).
    std::vector<uint8_t> finish_open() { open_ = true; return finish(); }
    std::vector<uint8_t> finish() {
        const uint64_t frame = std::max(stop_, last_);
        if (open_) wait_to(frame);
        else {
            for (unsigned s = 0; s < kSlots; ++s) if (slots_[s].active) { held_ = true; key_off_slot(s, frame); }
            wait_to(std::max(frame, last_) + uint64_t(kTailSeconds * kRate));
        }
        commands_.push_back(0x66);
        // Synthetic sample ROM: header table at 0 (renumbered samples), data from 0x2000.
        std::vector<uint8_t> rom(0x2000, 0);
        for (size_t k = 0; k < sources_.size(); ++k) {
            const Source &src = sources_[k];
            const uint32_t at = uint32_t(rom.size());
            std::array<uint8_t, 12> h = src.header;
            h[0] = uint8_t(at >> 16); h[1] = uint8_t(at >> 8); h[2] = uint8_t(at);
            std::memcpy(rom.data() + k * 12, h.data(), 12);
            rom.insert(rom.end(), roms_.pcm[src.rom].begin() + src.physical,
                       roms_.pcm[src.rom].begin() + src.physical + src.frames);
            sample_bytes_ += src.frames;
        }
        std::vector<uint8_t> vgm(0xc0, 0);
        std::memcpy(vgm.data(), "Vgm ", 4);
        set32(vgm, 0x08, 0x161);
        set32(vgm, 0x18, uint32_t(last_ - start_));
        set32(vgm, 0x34, 0xc0 - 0x34);
        set32(vgm, 0x88, kClock);
        // ROM blocks: the header table, then the sample data (one block).
        auto block = [&](uint32_t from, uint32_t to) {
            vgm.push_back(0x67); vgm.push_back(0x66); vgm.push_back(0x89);
            put32(vgm, to - from + 8); put32(vgm, 0x400000); put32(vgm, from);
            vgm.insert(vgm.end(), rom.begin() + from, rom.begin() + to);
        };
        block(0, 0x1800);
        if (rom.size() > 0x2000) block(0x2000, uint32_t(rom.size()));
        vgm.insert(vgm.end(), commands_.begin(), commands_.end());
        set32(vgm, 0x04, uint32_t(vgm.size() - 4));
        return vgm;
    }
    unsigned notes() const { return notes_; }
    unsigned peak() const { return peak_; }
    bool held() const { return held_; }
    uint32_t frames() const { return uint32_t(last_ - start_); }
    uint32_t sample_bytes() const { return sample_bytes_; }
    size_t samples() const { return sources_.size(); }
    unsigned rejected() const { return rejected_; }
    int first_level() const { return first_level_; }
    std::string ids() const {
        std::string out;
        for (const auto &src : sources_) {
            char id[24]; std::snprintf(id, sizeof id, "%s%u:%u:%u", out.empty() ? "" : ",", src.rom, src.bank, src.index);
            out += id;
        }
        return out;
    }

private:
    struct Slot { bool active = false; int voice = -1; uint64_t freed = 0, ends = 0; uint16_t rate = 0; uint8_t level = 0, pan = 0; };
    const Roms &roms_;
    std::vector<uint8_t> commands_;
    std::vector<Source> sources_;
    std::array<Slot, kSlots> slots_{};
    std::array<int, 56> voice_slot_{};
    uint64_t start_ = 0, last_ = 0, stop_ = 0;
    unsigned notes_ = 0, peak_ = 0, rejected_ = 0;
    uint32_t sample_bytes_ = 0;
    int first_level_ = -1;
    bool held_ = false, open_ = false;

    void wait_to(uint64_t frame) {
        if (frame < last_) frame = last_;
        for (uint64_t n = frame - last_; n;) {
            const uint64_t step = std::min<uint64_t>(n, 65535);
            commands_.push_back(0x61); put16(commands_, uint16_t(step)); n -= step;
        }
        last_ = frame;
    }
    void write(uint8_t port, uint8_t value) { commands_.push_back(0xb5); commands_.push_back(port); commands_.push_back(value); }
    void select(unsigned slot, uint8_t reg) { write(1, uint8_t(slot + slot / 7)); write(2, reg); }
    void reg(unsigned slot, uint8_t r, uint8_t value) { select(slot, r); write(0, value); }
    int sample_for(const Sequencer::VoiceEvent &e, Source &src) {
        const unsigned index = e.sample_index & 511;
        for (size_t k = 0; k < sources_.size(); ++k)
            if (sources_[k].rom == e.rom && sources_[k].index == index && sources_[k].bank == e.bank) {
                src = sources_[k]; return int(k);
            }
        if (!source_for(roms_, e.rom, e.bank, index, src) || sources_.size() == 512) return -1;
        sources_.push_back(src);
        return int(sources_.size() - 1);
    }
    void note_on(const Sequencer::VoiceEvent &e) {
        Source src{};
        const int k = sample_for(e, src);
        if (k < 0) { ++rejected_; return; }
        if (voice_slot_[e.voice_id] >= 0) key_off_voice(e.voice_id, e.frame);
        int slot = -1;
        for (unsigned s = 0; s < kSlots; ++s)
            if (!slots_[s].active && (slot < 0 || slots_[s].freed < slots_[slot].freed)) slot = int(s);
        if (slot < 0) { ++rejected_; return; }
        wait_to(e.frame);
        Slot &s = slots_[slot];
        s = {true, e.voice_id, s.freed, 0, e.rate_code, e.level, e.pan_code};
        if (src.silent_loop) {
            const double seconds = double(src.loop) / source_hz(e.rate_code);
            s.ends = e.frame + std::max<uint64_t>(1, uint64_t(seconds * kRate + 0.5));
        }
        voice_slot_[e.voice_id] = slot;
        reg(slot, 1, uint8_t(k));
        reg(slot, 2, uint8_t((e.rate_code & 0xfe) | (k >> 8)));
        reg(slot, 3, uint8_t(e.rate_code >> 8));
        reg(slot, 0, uint8_t((e.pan_code & 15) << 4));
        reg(slot, 5, uint8_t(((e.level & 127) << 1) | 1));
        reg(slot, 4, 0x80);
        if (first_level_ < 0) first_level_ = e.level & 127;
        ++notes_;
        peak_ = std::max(peak_, sounding());
    }
    void update(const Sequencer::VoiceEvent &e) {
        const int slot = voice_slot_[e.voice_id];
        if (slot < 0 || !slots_[slot].active) return;
        Slot &s = slots_[slot];
        wait_to(e.frame);
        if (e.rate_code != s.rate) {
            reg(slot, 2, uint8_t((e.rate_code & 0xfe) | (s.rate & 1)));
            reg(slot, 3, uint8_t(e.rate_code >> 8));
            s.rate = e.rate_code;
        }
        if (e.pan_code != s.pan) { reg(slot, 0, uint8_t((e.pan_code & 15) << 4)); s.pan = e.pan_code; }
        if (e.level != s.level) { reg(slot, 5, uint8_t(((e.level & 127) << 1) | 1)); s.level = e.level; }
    }
    void key_off_slot(unsigned slot, uint64_t frame) {
        wait_to(frame);
        reg(slot, 4, 0);
        if (slots_[slot].voice >= 0) voice_slot_[slots_[slot].voice] = -1;
        slots_[slot].active = false; slots_[slot].voice = -1; slots_[slot].freed = frame;
    }
    void key_off_voice(unsigned voice, uint64_t frame) {
        const int slot = voice_slot_[voice];
        if (slot >= 0 && slots_[slot].active) key_off_slot(unsigned(slot), frame);
    }
};

Recorder *recording;
std::vector<Sequencer::VoiceEvent> engine_events;
void sink(void *, const Sequencer::VoiceEvent &e) {
    if (e.voice_id >= 46) { engine_events.push_back(e); return; } // The engine layers' reserved voices.
    if (recording) recording->event(e);
}

void boot(Sequencer &seq) {
    const uint8_t start[] = {0xf8, 0xf8, 0xff};
    const uint8_t setup[] = {0xbe, 0x14, 0x1f, 0xbe, 0x16, 0x02, 0xbe, 0x1b, 0x06, 0xbe, 0x1c, 0x03, 0xbe, 0x1d, 0x01,
                             0xbe, 0x1e, 0x02, 0xbe, 0x1f, 0x05, 0xbe, 0x36, 0x09, 0xbe, 0x17, 0x00, 0xbe, 0x18, 0x04,
                             0xbe, 0x35, 0x00, 0xbe, 0x34, 0x00, 0xbe, 0x17, 0x00, 0xbe, 0x19, 0x00};
    seq.send(start, sizeof start); seq.advance(kRate / 10);
    seq.send(setup, sizeof setup); seq.advance(kRate / 10);
    engine_events.clear();
}

struct Rendered { std::string key; uint32_t command; unsigned kind, track; Recorder recorder; };

// Plays one command from the boot state; false when it sounds nothing.
bool render(const Roms &roms, const uint8_t command[3], double seconds, Recorder &out) {
    Sequencer seq(roms.program, kRate);
    seq.set_sink(sink, nullptr);
    boot(seq);
    out.start(seq.frames());
    recording = &out;
    seq.send(command, 3);
    const uint64_t cap = seq.frames() + uint64_t(seconds * kRate);
    while (seq.frames() < cap) {
        seq.advance(kRate / 1000);
        out.expire(seq.frames());
        if (seq.tracks_idle() && !out.sounding()) break;
        if (seq.failed()) break;
    }
    out.expire(seq.frames());
    out.stop_at(seq.frames());
    recording = nullptr;
    return out.notes() && !seq.failed();
}

uint16_t program_word(const Roms &r, uint32_t at) { return uint16_t(r.program[at] << 8 | r.program[at + 1]); }
// A pointer of the sound program (NativeSoundSequencer::pointer: the upper half's mirror).
uint32_t program_pointer(const Roms &r, uint32_t at) {
    const uint32_t p = uint32_t(program_word(r, at)) << 16 | program_word(r, at + 2);
    return p >= 0x80000 && p < 0xa0000 ? p - 0x60000 : p;
}
// start_sequence: a single-track descriptor (flags bit 7) names channel 10..14, track 1..5; a playlist is track 0.
unsigned sequence_track(const Roms &r, uint32_t table, unsigned index) {
    const uint32_t descriptor = program_pointer(r, table + 2 + 4 * index);
    if (descriptor + 2 > r.program.size() || !(r.program[descriptor] & 128)) return 0;
    const unsigned channel = r.program[descriptor + 1] & 15;
    return channel >= 10 && channel <= 14 ? channel - 9 : 0;
}

// update_engines (native_sound_sequencer.cpp): ten layers, in the runtime's order. slot: the
// reserved voice (rom 1); index: the controller that selects the row of the tables.
struct Layer { unsigned slot; uint8_t index_control; uint16_t pitch_table, level_table; uint8_t pan; unsigned rows; };
constexpr Layer kLayers[10] = {
    {25, 0x17, 0x18d4, 0x1854, 0, 128}, {24, 0x17, 0x18d4, 0x1a86, 0, 128}, {23, 0x17, 0x1d38, 0x1cb8, 0, 128},
    {27, 0x17, 0x1f6c, 0x1eec, 0, 128}, {26, 0x17, 0x2120, 0x1eec, 0, 128}, {22, 0x17, 0x2344, 0x22c4, 0, 128},
    {20, 0x17, 0x18d4, 0, 7, 128}, // level: controller 0x34 (0: 255)
    {19, 0x17, 0x18d4, 0, 9, 128}, // level: controller 0x35
    {21, 0x19, 0x2528, 0x2518, 0, 16}, // pan: table 0x254a (high nibble)
    {18, 0x37, 0x27ce, 0x27be, 0, 16},
};

// engine.tbl: for each layer, rows x {AICA PITCH, MIX, DIRECT} words (little-endian). Layers 6 and 7:
// PITCH by rpm, MIX by the level controller's value. The layer VGMs eN.vgm key their sample on muted.
std::vector<uint8_t> engine_layers(const Roms &roms, const std::string &out, std::FILE *tsv) {
    Sequencer seq(roms.program, kRate);
    seq.set_sink(sink, nullptr);
    boot(seq);
    const uint8_t on[] = {0xbe, 0x17, 0x01, 0xbe, 0x19, 0x01, 0xbe, 0x37, 0x01};
    seq.send(on, sizeof on);
    seq.advance(kRate / 100);
    std::vector<uint8_t> table;
    put32(table, 0x31474e45); // "ENG1"
    put32(table, 10);
    for (unsigned l = 0; l < 10; ++l) {
        const Layer &layer = kLayers[l];
        const Sequencer::VoiceEvent *start = nullptr;
        for (const auto &e : engine_events)
            if (e.kind == Sequencer::EventKind::NoteOn && e.voice_id == 28 + layer.slot) { start = &e; break; }
        if (!start) { std::fprintf(stderr, "engine layer %u (slot %u) did not start\n", l, layer.slot); std::exit(1); }
        Sequencer::VoiceEvent muted = *start;
        muted.pan_code = 8; muted.level = 127; muted.frame = 0; muted.voice_id = 0;
        Recorder rec(roms);
        rec.start(0);
        rec.event(muted);
        rec.stop_at(kRate / 1000);
        std::vector<uint8_t> vgm = rec.finish_open();
        char key[16]; std::snprintf(key, sizeof key, "e%u", l);
        write_file(out + "/vgm/" + key + ".vgm", vgm);
        std::fprintf(tsv, "%s\t%06x\t2\t%u\t1\t1\t1\t1\t%zu\t%u\t%s\n", key, 0xbe0000u | l, l, rec.samples(),
                     rec.sample_bytes(), rec.ids().c_str());
        put16(table, uint16_t(layer.rows)); put16(table, 0);
        std::vector<uint16_t> pitch, mix, direct;
        for (unsigned row = 0; row < layer.rows; ++row) {
            pitch.push_back(aica_pitch(program_word(roms, layer.pitch_table + 2 * row)));
            uint8_t pan = layer.pan, level;
            if (l == 6 || l == 7) level = uint8_t((row ? row : 255) >> 1);
            else level = uint8_t(roms.program[layer.level_table + row] >> 1);
            if (l == 8) pan = uint8_t(roms.program[0x254a + row] >> 4);
            mix.push_back(aica_mix(pan, level));
            direct.push_back(aica_direct(pan));
        }
        for (auto v : pitch) put16(table, v);
        for (auto v : mix) put16(table, v);
        for (auto v : direct) put16(table, v);
    }
    return table;
}

int cmd_render(const char *sound_dir, const char *out_dir) {
    Roms roms;
    const std::string dir = sound_dir;
    roms.program = read_file(dir + "/sound_program.bin", 0x40000);
    roms.pcm[0] = read_file(dir + "/pcm1.bin", 0x400000);
    roms.pcm[1] = read_file(dir + "/pcm2.bin", 0x400000);
    const std::string out = out_dir;
    std::system(("mkdir -p '" + out + "/vgm'").c_str());
    std::FILE *tsv = std::fopen((out + "/render.tsv").c_str(), "w");
    if (!tsv) return 2;
    std::fprintf(tsv, "key\tcommand\tkind\ttrack\tnotes\tvoices\tms\theld\tsamples\tsample_bytes\tids\n");
    unsigned flows = 0, held = 0;
    uint64_t sample_total = 0;
    auto emit = [&](const std::string &key, uint32_t command, unsigned kind, unsigned track, Recorder &rec) {
        std::vector<uint8_t> vgm = rec.finish();
        write_file(out + "/vgm/" + key + ".vgm", vgm);
        std::fprintf(tsv, "%s\t%06x\t%u\t%u\t%u\t%u\t%u\t%u\t%zu\t%u\t%s\n", key.c_str(), command, kind, track, rec.notes(),
                     rec.peak(), unsigned(uint64_t(rec.frames()) * 1000 / kRate), unsigned(rec.held()), rec.samples(),
                     rec.sample_bytes(), rec.ids().c_str());
        ++flows; held += rec.held(); sample_total += rec.sample_bytes();
    };
    // Sequences: bank table 0x2bc6 (native_sound_sequencer.cpp kSequenceBanks), word 0 = last index.
    for (unsigned bank = 0x20; bank <= 0x22; ++bank) {
        const uint32_t table = program_pointer(roms, 0x2bc6 + 4 * bank);
        if (table + 2 > roms.program.size()) continue;
        const unsigned last = program_word(roms, table);
        for (unsigned index = 0; index <= last && index < 128; ++index) {
            const uint8_t command[3] = {0xae, uint8_t(bank), uint8_t(index)};
            Recorder rec(roms);
            if (!render(roms, command, kSequenceSeconds, rec)) continue;
            char key[16]; std::snprintf(key, sizeof key, "ae%02x%02x", bank, index);
            emit(key, 0xae0000u | bank << 8 | index, 0, sequence_track(roms, table, index), rec);
        }
    }
    // Channel 14 notes at velocity 127 (velocity: the gain table below).
    for (unsigned note = 0; note < 128; ++note) {
        const uint8_t command[3] = {0x9e, uint8_t(note), 0x7f};
        Recorder rec(roms);
        if (!render(roms, command, kNoteSeconds, rec)) continue;
        char key[16]; std::snprintf(key, sizeof key, "9e%02x", note);
        emit(key, 0x9e0000u | note << 8, 1, 0, rec);
    }
    // Velocity: the level the driver gives a channel-14 note at each velocity, as a linear gain
    // relative to velocity 127 (0.375 dB a level step; code/audio/sfx.c: afx_instance_gain).
    std::vector<uint8_t> gains(128, 0);
    {
        std::array<int, 128> level{};
        for (unsigned v = 1; v < 128; ++v) {
            const uint8_t command[3] = {0x9e, 0x07, uint8_t(v)};
            Recorder rec(roms);
            level[v] = render(roms, command, 0.01, rec) ? rec.first_level() : -1;
        }
        for (unsigned v = 1; v < 128; ++v)
            gains[v] = level[v] < 0 || level[127] < 0 ? 255 :
                uint8_t(std::clamp(std::nearbyint(255.0 * std::pow(10.0, -(level[v] - level[127]) * 0.375 / 20.0)), 0.0, 255.0));
    }
    write_file(out + "/gains.bin", gains);
    // The engine layers: the voices update_engines keys on for controller values 1, from the boot state.
    std::vector<uint8_t> table = engine_layers(roms, out, tsv);
    write_file(out + "/engine.tbl", table);
    std::fclose(tsv);
    std::printf("rendered %u flows (%u held to the cap), %llu sample bytes before merging\n", flows, held,
                (unsigned long long)sample_total);
    return 0;
}

int cmd_park(const char *in, const char *out) {
    std::vector<uint8_t> data = read_whole(in);
    afx_file_header_t h;
    if (afx_file_validate(data.data(), uint32_t(data.size()), &h)) { std::fprintf(stderr, "%s: not a valid AFX\n", in); return 1; }
    const uint32_t last = h.image_offset + h.stream_offset + h.stream_size - 1;
    if (h.flags & AFX_FLAG_CONTROLLED || data[last] != AFX_OP_END) { std::fprintf(stderr, "%s: no final END\n", in); return 1; }
    data[last] = AFX_OP_PARK;
    set32(data, 12, AFX_FLAG_CONTROLLED);
    set32(data, 32, afx_control_id(data.data() + h.image_offset, h.image_size));
    afx_validation_profile_t profile;
    if (afx_file_validate_profile(data.data(), uint32_t(data.size()), &h, &profile) ||
        h.work_profile != AFX_WORK_PROFILE(profile.peak_commands, profile.peak_register_writes)) {
        std::fprintf(stderr, "%s: the controlled flow does not validate\n", in); return 1;
    }
    write_file(out, data);
    return 0;
}

// sfx.dsx (code/audio/sfx.c reads it; little-endian):
//   0 "DSX1", 4 version 1, 8 flow count, 12 directory offset, 16 engine table offset (0: none),
//   20 velocity gains offset (128 bytes), 24 file bytes, 28/32 the bank identity every flow binds.
//   Directory, sorted by key, 16 bytes each: key (command word, a note's velocity zero; engine
//   layer N: 0xbe0000+N), kind (0 sequence, 1 note, 2 engine layer), track (sequence track 1..5,
//   0 none; engine: the layer), flags (bit 0: held to the render cap), 0, offset, bytes of the AFX.
int cmd_pack(const char *tsv_path, const char *engine_path, const char *gains_path, const char *flow_dir,
             const char *out_path) {
    struct Entry { uint32_t key; uint8_t kind, track, flags; std::vector<uint8_t> afx; };
    std::vector<Entry> entries;
    std::FILE *tsv = std::fopen(tsv_path, "r");
    if (!tsv) return 2;
    char line[8192];
    if (!std::fgets(line, sizeof line, tsv)) return 2;
    uint32_t bank_low = 0, bank_high = 0;
    while (std::fgets(line, sizeof line, tsv)) {
        char key[32]; unsigned command, kind, track, notes, voices, ms, held;
        if (std::sscanf(line, "%31s %x %u %u %u %u %u %u", key, &command, &kind, &track, &notes, &voices, &ms, &held) != 8) return 2;
        const std::string path = std::string(flow_dir) + "/" + key + ".afx";
        std::FILE *probe = std::fopen(path.c_str(), "rb");
        if (!probe) continue; // Not in this bank.
        std::fclose(probe);
        Entry e{command, uint8_t(kind), uint8_t(track), uint8_t(held ? 1 : 0), read_whole(path)};
        afx_file_header_t h;
        if (afx_file_validate(e.afx.data(), uint32_t(e.afx.size()), &h)) { std::fprintf(stderr, "%s: invalid\n", path.c_str()); return 1; }
        if (entries.empty()) { bank_low = h.bank_id_low; bank_high = h.bank_id_high; }
        else if (h.bank_id_low != bank_low || h.bank_id_high != bank_high) {
            std::fprintf(stderr, "%s: bound to another bank\n", path.c_str()); return 1;
        }
        if ((kind == 2) != bool(h.flags & AFX_FLAG_CONTROLLED)) { std::fprintf(stderr, "%s: wrong kind\n", path.c_str()); return 1; }
        entries.push_back(std::move(e));
    }
    std::fclose(tsv);
    if (entries.empty()) { std::fprintf(stderr, "no flows in %s\n", flow_dir); return 1; }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.key < b.key; });
    for (size_t i = 1; i < entries.size(); ++i)
        if (entries[i].key == entries[i - 1].key) { std::fprintf(stderr, "key %06x twice\n", entries[i].key); return 1; }
    const std::vector<uint8_t> engine = engine_path[0] ? read_whole(engine_path) : std::vector<uint8_t>{};
    const std::vector<uint8_t> gains = read_whole(gains_path);
    if (gains.size() != 128) return 1;
    std::vector<uint8_t> out(36, 0);
    std::memcpy(out.data(), "DSX1", 4);
    set32(out, 4, 1); set32(out, 8, uint32_t(entries.size())); set32(out, 28, bank_low); set32(out, 32, bank_high);
    while (out.size() % 4) out.push_back(0);
    set32(out, 20, uint32_t(out.size()));
    out.insert(out.end(), gains.begin(), gains.end());
    if (!engine.empty()) { set32(out, 16, uint32_t(out.size())); out.insert(out.end(), engine.begin(), engine.end()); }
    while (out.size() % 4) out.push_back(0);
    const size_t directory = out.size();
    set32(out, 12, uint32_t(directory));
    out.resize(directory + 16 * entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        while (out.size() % 32) out.push_back(0);
        const size_t at = directory + 16 * i;
        set32(out, at, entries[i].key);
        out[at + 4] = entries[i].kind; out[at + 5] = entries[i].track; out[at + 6] = entries[i].flags;
        set32(out, at + 8, uint32_t(out.size())); set32(out, at + 12, uint32_t(entries[i].afx.size()));
        out.insert(out.end(), entries[i].afx.begin(), entries[i].afx.end());
    }
    set32(out, 24, uint32_t(out.size()));
    write_file(out_path, out);
    std::printf("%s: %zu flows, %zu bytes\n", out_path, entries.size(), out.size());
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && !std::strcmp(argv[1], "render")) return cmd_render(argv[2], argv[3]);
    if (argc == 4 && !std::strcmp(argv[1], "park")) return cmd_park(argv[2], argv[3]);
    if (argc == 7 && !std::strcmp(argv[1], "pack")) return cmd_pack(argv[2], argv[3], argv[4], argv[5], argv[6]);
    std::fprintf(stderr, "usage: prepare_sfx render SOUND_DIR OUT_DIR | park IN.afx OUT.afx |\n"
                         "       prepare_sfx pack RENDER.tsv ENGINE.tbl|'' GAINS.bin FLOW_DIR OUT.dsx\n");
    return 2;
}
