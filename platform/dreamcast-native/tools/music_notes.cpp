// The note-ons the native sequencer (src/runtime/native_sound_sequencer.*) plays for one command
// sent after the game's boot bytes (f8 f8 ff, then the frame-179 controller burst of the original
// program). Host only; used by tools/check_music_map.py.
//   music_notes SOUND_PROGRAM.bin STATUS BANK INDEX SECONDS   (command bytes in hex)
// Prints a summary line, then "seconds rom bank sample rate_hz" per note-on.
#include "runtime/native_sound_sequencer.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
std::vector<snd::NativeSoundSequencer::VoiceEvent> notes;
void sink(void *, const snd::NativeSoundSequencer::VoiceEvent &e) {
    if (e.kind == snd::NativeSoundSequencer::EventKind::NoteOn) notes.push_back(e);
}
}

int main(int argc, char **argv) {
    if (argc != 6) { std::fprintf(stderr, "usage: music_notes SOUND_PROGRAM.bin STATUS BANK INDEX SECONDS\n"); return 2; }
    std::vector<uint8_t> program(0x40000);
    std::FILE *file = std::fopen(argv[1], "rb");
    if (!file || std::fread(program.data(), 1, program.size(), file) != program.size()) return 2;
    std::fclose(file);
    snd::NativeSoundSequencer sequencer(program, 48000);
    sequencer.set_sink(sink, nullptr);
    const uint8_t boot[] = {0xf8, 0xf8, 0xff};
    const uint8_t setup[] = {0xbe, 0x14, 0x1f, 0xbe, 0x16, 0x02, 0xbe, 0x1b, 0x06, 0xbe, 0x1c, 0x03, 0xbe, 0x1d, 0x01,
                             0xbe, 0x1e, 0x02, 0xbe, 0x1f, 0x05, 0xbe, 0x36, 0x09, 0xbe, 0x17, 0x00, 0xbe, 0x18, 0x04,
                             0xbe, 0x35, 0x00, 0xbe, 0x34, 0x00};
    sequencer.send(boot, sizeof boot); sequencer.advance(4800);
    sequencer.send(setup, sizeof setup); sequencer.advance(480);
    const uint8_t command[] = {uint8_t(std::strtoul(argv[2], nullptr, 16)), uint8_t(std::strtoul(argv[3], nullptr, 16)),
                               uint8_t(std::strtoul(argv[4], nullptr, 16))};
    const uint64_t start = sequencer.frames();
    sequencer.send(command, sizeof command);
    for (unsigned long i = 0, n = std::strtoul(argv[5], nullptr, 10) * 10; i < n; ++i) sequencer.advance(4800);
    const auto &s = sequencer.stats();
    std::printf("# notes=%zu unsupported=%llu invalid=%llu limit=%llu\n", notes.size(), (unsigned long long)s.unsupported,
                (unsigned long long)s.invalid_data, (unsigned long long)s.event_limit_hits);
    for (const auto &n : notes)
        std::printf("%.4f %u %u %u %.3f\n", double(n.frame - start) / 48000., n.rom, n.bank, n.sample_index, n.source_rate_hz);
    return s.invalid_data || s.event_limit_hits ? 1 : 0;
}
