/* Host check of code/audio/sound.c against a fake AICAflow (firmware ABI 8): a boot
 * timeout, loading, replay of a repeating recording without reloading when its end
 * is reported from another thread, a frame that only plays asking AICAflow nothing,
 * an error end, the original fade, stop commands, end reports racing the updater,
 * a full report queue, a missing file, the ADPCM copy when the AICA RAM lacks room, a load spread over frames within each frame's
 * time (none without time or just before a vblank, none during a DMA), a load dropped
 * by a change of mind, settling at a loading pause, shutdown
 * releasing everything, and the music map and selection tables. Built by the
 * Makefile's check target with ASan/UBSan. */
#define NM_SOUND_HOST_TEST 1
#include "../code/audio/sound.c"
#include "audio/music_select.h"

#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* audio/sfx.h has its own check (tests/test_sfx.c): here only that sound.c hands it every command. */
static unsigned sfx_inits, sfx_commands, sfx_updates, sfx_shutdowns;
int nm_sfx_init(const char *root) { (void)root; ++sfx_inits; return 1; }
void nm_sfx_command(uint32_t command) { (void)command; ++sfx_commands; }
void nm_sfx_update(void) { ++sfx_updates; }
void nm_sfx_quiesce(void) {}
void nm_sfx_shutdown(void (*wait)(unsigned)) { if (wait) wait(0); ++sfx_shutdowns; }

const unsigned char nm_sound_test_firmware[32] = {1};
const unsigned nm_sound_test_firmware_size = sizeof(nm_sound_test_firmware);
static uint64_t clock_us;
uint64_t nm_sound_test_now_us(void) { return clock_us; }

/* Fake AICAflow: one instance. As in ABI 8 there is no update call: an instance that
 * ends (by itself, or after a stop) is freed and reported to the end callback by
 * AICAflow's event thread, here a real second thread (end_from_thread). A bank
 * arrives by the stream API (begin, one DMA at a time, finish); a DMA completes
 * between updates. Files are read through nm_sound_test_read, which costs the fake
 * clock what a dcload read costs on hardware (1.2 ms a call, 3 us a byte; sfx.c's fit). */
static struct {
    int init_result, up, banks_loaded, banks_live, flows_live, activations, stops, shutdowns, calls;
    uint32_t generation, live; /* live: the current handle, 0 once freed */
    uint32_t state; int stop_pending;
    uint8_t gain;
    afx_end_callback_t end; void *end_user;
    /* the bank stream */
    int streaming, dma_pending, dma_ready, dma_overlap, bad_data;
    uint32_t stream_size, cursor, dma_bytes, dma_offset, reads, read_max;
    uint32_t read_extra_us;
    uint32_t aica_free; /* free AICA RAM afx_mem_stats reports (0: 2,000,000) */
    uint64_t read_us; /* the clock spent reading in the current frame */
} fake;

size_t nm_sound_test_read(void *to, size_t bytes, FILE *file) {
    if (fake.dma_pending) ++fake.dma_overlap; /* a file read while the G2 DMA runs */
    const size_t got = fread(to, 1, bytes, file);
    const uint64_t cost = 1200u + 3u * got + fake.read_extra_us;
    clock_us += cost; fake.read_us += cost; ++fake.reads;
    if (got > fake.read_max) fake.read_max = (uint32_t)got;
    return got;
}

int afx_init(const void *firmware, uint32_t size) {
    (void)firmware;
    if (fake.init_result) return fake.init_result;
    fake.up = size == 32; return fake.up ? 0 : -1;
}
void afx_shutdown(void) { ++fake.calls; ++fake.shutdowns; fake.up = 0; fake.end = NULL; }
void afx_set_end_callback(afx_end_callback_t callback, void *user) { ++fake.calls; fake.end = callback; fake.end_user = user; }
int afx_mem_stats(afx_mem_stats_t *s) {
    memset(s, 0, sizeof(*s)); s->total_bytes = 2000000;
    s->free_bytes = s->largest_free_block = fake.aica_free ? fake.aica_free : 2000000; return 0;
}
int afx_sample_bank_stream_begin(uint32_t bytes, afx_asset_t *out) {
    ++fake.calls;
    if (fake.streaming || fake.banks_live) return -AFX_NO_AICA_RAM; /* one recording fits */
    fake.streaming = 1; fake.stream_size = bytes; fake.cursor = 0; *out = 7; return 0;
}
int afx_sample_bank_stream_dma_begin(afx_asset_t asset, uint32_t offset, const void *data, uint32_t bytes) {
    ++fake.calls;
    if (asset != 7 || !fake.streaming || ((uintptr_t)data & 31u) || offset != fake.cursor || !bytes ||
        bytes > fake.stream_size - offset || ((bytes & 31u) && offset + bytes != fake.stream_size)) return -AFX_BAD_BOUNDS;
    if (fake.dma_pending) return -AFX_BUSY;
    const unsigned char *p = data;
    for (uint32_t i = 0; i < bytes; ++i) if (p[i] != (unsigned char)((offset + i) * 7u)) { ++fake.bad_data; break; }
    for (uint32_t i = bytes; i & 31u; ++i) if (p[i]) { ++fake.bad_data; break; } /* the padding is zero */
    fake.dma_pending = 1; fake.dma_ready = 0; fake.dma_bytes = bytes; fake.dma_offset = offset; return 0;
}
int afx_sample_bank_stream_dma_poll(afx_asset_t asset, bool *complete) {
    ++fake.calls; *complete = false;
    if (asset != 7 || !fake.dma_pending) return -AFX_BAD_BOUNDS;
    if (!fake.dma_ready) return 0;
    fake.dma_pending = 0; fake.cursor += fake.dma_bytes; *complete = true; return 0;
}
int afx_sample_bank_stream_finish(afx_asset_t asset) {
    ++fake.calls;
    if (asset != 7 || !fake.streaming || fake.dma_pending || fake.cursor != fake.stream_size) return -AFX_BAD_BOUNDS;
    fake.streaming = 0; ++fake.banks_loaded; ++fake.banks_live; return 0;
}
int afx_bank_release(afx_bank_t *bank) { ++fake.calls; if (bank->asset) --fake.banks_live; *bank = (afx_bank_t){0}; return 0; }
int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size, afx_asset_t *out) {
    ++fake.calls;
    if (!bank->asset || bank->asset != 7 || bank->bytes != fake.cursor || bank->id.low != 0x1234 || !data ||
        size != 84 || memcmp(data, "AFX!", 4)) return -AFX_BAD_FORMAT;
    ++fake.flows_live; *out = 9; return 0;
}
int afx_asset_free(afx_asset_t asset) {
    ++fake.calls;
    if (asset == 9) --fake.flows_live;
    if (asset == 7 && fake.streaming) { fake.streaming = 0; fake.dma_pending = 0; } /* a bank dropped half loaded */
    return 0;
}
int afx_instance_activate(afx_asset_t flow, afx_instance_t *out) {
    ++fake.calls;
    if (flow != 9 || fake.live) return -AFX_BUSY;
    ++fake.activations; fake.live = ++fake.generation; fake.state = AFX_RUNNING;
    *out = fake.live; return 0;
}
int afx_instance_stop(afx_instance_t i) {
    ++fake.calls;
    if (!i || i != fake.live) return -AFX_STALE_GENERATION;
    ++fake.stops; fake.stop_pending = 1; return 0;
}
int afx_instance_gain(afx_instance_t i, uint8_t gain) { ++fake.calls; if (i != fake.live) return -AFX_STALE_GENERATION; fake.gain = gain; return 0; }
int afx_instance_status(afx_instance_t i, afx_instance_status_t *out) {
    ++fake.calls;
    if (!i || i != fake.live) return -AFX_STALE_GENERATION;
    memset(out, 0, sizeof(*out)); out->reference = i; out->state = fake.state; return 0;
}

/* The event thread: frees the instance, then reports it (state DONE or ERROR). */
struct end_report { afx_instance_t instance; uint32_t state; int report; };
static void *event_thread(void *arg) {
    const struct end_report *r = arg;
    if (r->instance == fake.live) fake.live = 0;
    afx_instance_status_t status = {.reference = r->instance, .state = r->state, .result = r->state == AFX_ERROR ? 3u : 0u};
    if (r->report && fake.end) fake.end(r->instance, &status, fake.end_user);
    return NULL;
}
static void end_from_thread(afx_instance_t instance, uint32_t state, int report) {
    struct end_report r = {instance, state, report};
    pthread_t thread;
    if (pthread_create(&thread, NULL, event_thread, &r) || pthread_join(thread, NULL)) { perror("pthread"); exit(1); }
}
/* A queued stop takes effect between updates. */
static void deliver_stop(void) {
    if (!fake.stop_pending) return;
    fake.stop_pending = 0;
    end_from_thread(fake.live, AFX_DONE, 1);
}
void nm_sound_test_wait_ms(unsigned ms) { clock_us += ms * 1000u; if (fake.dma_pending) fake.dma_ready = 1; deliver_stop(); }

/* Concurrent reports for other (stale) instances while the updater runs. */
static volatile int racing;
static void *foreign_reports(void *arg) {
    (void)arg;
    afx_instance_status_t status = {.state = AFX_DONE};
    for (uint32_t i = 0; i < 200000 && racing; ++i) {
        status.reference = 0x10000u + i;
        fake.end(status.reference, &status, fake.end_user);
    }
    racing = 0;
    return NULL;
}

static int failures;
static void expect(int ok, const char *what, ...) {
    if (ok) return;
    va_list args; va_start(args, what);
    fputs("FAIL: ", stderr); vfprintf(stderr, what, args); fputc('\n', stderr);
    va_end(args); ++failures;
}
/* A frame a vblank period after the previous one (the DMA started then has completed): the update, then the
 * idle wait for the PVR with budget_us for the music (code/audio/idle_wait.c). */
static void frame(uint32_t budget_us) {
    clock_us += 16683u; fake.read_us = 0;
    if (fake.dma_pending) fake.dma_ready = 1;
    nm_sound_update(); deliver_stop();
    (void)nm_sound_idle(budget_us);
}
static void frames(unsigned n) { while (n--) frame(8000u); }
/* Updates until the recording plays (at most n). */
static unsigned until_playing(unsigned n) {
    unsigned k = 0;
    while (k < n && sound.state != SOUND_PLAYING) { frames(1); ++k; }
    return k;
}
/* A flow: the 80-byte AFX header ("AFX!", the file's length at +8), then 4 bytes. */
static void write_file(const char *path) {
    unsigned char flow[84] = {'A', 'F', 'X', '!', 0, 0, 0, 0, 84};
    FILE *f = fopen(path, "wb"); fwrite(flow, 1, sizeof(flow), f); fclose(f);
}
/* An AFB: the 32-byte header (magic, version, identity 0x1234/0, payload offset and size, file size), then the payload. */
static void write_bank(const char *path, uint32_t payload) {
    unsigned char header[32] = {0};
    const uint32_t words[7] = {AFX_BANK_MAGIC, AFX_BANK_VERSION, 0x1234, 0, 32, payload, 32 + payload};
    for (unsigned i = 0; i < 7; ++i) for (unsigned b = 0; b < 4; ++b) header[4 * i + b] = (unsigned char)(words[i] >> 8 * b);
    FILE *f = fopen(path, "wb"); fwrite(header, 1, 32, f);
    for (uint32_t i = 0; i < payload; ++i) fputc((int)(unsigned char)(i * 7u), f);
    fclose(f);
}

int main(void) {
    char root[64] = "/tmp/nm-sound-XXXXXX", path[512];
    if (!mkdtemp(root)) return perror("mkdtemp"), 1;
    snprintf(path, sizeof(path), "%s/music", root); mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/music/t05.afb", root); write_bank(path, 3000);
    snprintf(path, sizeof(path), "%s/music/t05.afx", root); write_file(path);
    snprintf(path, sizeof(path), "%s/music/t07.afb", root); write_bank(path, 409166); /* BREAK DOWN's size */
    snprintf(path, sizeof(path), "%s/music/t07.afx", root); write_file(path);
    strcat(root, "/");

    /* No boot report from the ARM7 (-AFX_TIMEOUT): silent, and nothing asks AICAflow anything. */
    fake.init_result = -AFX_TIMEOUT;
    expect(nm_sound_init(root) == 0 && sound.state == SOUND_OFF && !fake.end, "a boot timeout leaves sound off");
    nm_sound_command(0xae1009); frames(10); nm_sound_settle(); nm_sound_shutdown();
    expect(fake.calls == 0 && fake.activations == 0 && fake.shutdowns == 0, "silent after a timeout (%d calls)", fake.calls);
    fake.init_result = 0;

    /* ae1009 (SKY HIGH): recording 05, repeating. */
    expect(nm_sound_init(root) == 1 && fake.end == on_instance_end, "init registers the end callback");
    nm_sound_command(0xae1009);
    expect(sound.wanted == 5 && sound.wanted_loops == 1, "ae1009 wants recording 05, repeating");
    const unsigned took = until_playing(20);
    expect(sound.state == SOUND_PLAYING && sound.track == 5 && fake.activations == 1 && fake.banks_loaded == 1 &&
           fake.cursor == 3000 && !fake.bad_data, "test track playing (state %d track %u, %u updates)", sound.state, sound.track, took);
    expect(took >= 5 && !fake.dma_overlap, "a load takes several updates (%u) and never reads during a DMA", took);
    /* Just playing: AICAflow is asked nothing at all. */
    int calls = fake.calls;
    frames(64);
    expect(fake.calls == calls, "playing asks AICAflow %d times in 64 updates", fake.calls - calls);
    /* It ends by itself, reported from the event thread: started again at the next update, without a reload. */
    end_from_thread(sound.instance, AFX_DONE, 1);
    expect(sound.state == SOUND_PLAYING, "the report waits for the updater");
    frames(1);
    expect(fake.activations == 2 && fake.banks_loaded == 1 && sound.state == SOUND_PLAYING, "replay without reload (activations %d loads %d)", fake.activations, fake.banks_loaded);
    /* Reports for other instances racing the updater are ignored; the next own one still lands. */
    racing = 1;
    pthread_t racer;
    if (pthread_create(&racer, NULL, foreign_reports, NULL)) return perror("pthread"), 1;
    for (unsigned i = 0; i < 100000 && racing; ++i) nm_sound_update();
    racing = 0; pthread_join(racer, NULL);
    frames(1);
    expect(sound.state == SOUND_PLAYING && fake.activations == 2 && ended_head == ended_tail, "foreign reports change nothing");
    end_from_thread(sound.instance, AFX_DONE, 1); frames(1);
    expect(fake.activations == 3 && sound.state == SOUND_PLAYING, "replay after the race (activations %d)", fake.activations);
    /* An error end is reported and not repeated. */
    end_from_thread(sound.instance, AFX_ERROR, 1); frames(3);
    expect(sound.state == SOUND_IDLE && fake.activations == 3 && sound.track == 5, "an error end stays stopped (state %d)", sound.state);
    want(5, 1); frames(1);
    expect(sound.state == SOUND_PLAYING && fake.activations == 4 && fake.banks_loaded == 1, "restarted after the error, no reload");

    /* be1a10: fade over 32 * 16 / 868 s, then stop and stay stopped. */
    nm_sound_command(0xbe1a10);
    const uint64_t fade = (uint64_t)FADE_STEPS * 16 * 1000000u / SEQUENCER_HZ;
    expect(sound.fade_us == fade, "fade length %llu", (unsigned long long)sound.fade_us);
    frames(10);
    expect(fake.gain > 0 && fake.gain < 255, "fading gain %u", fake.gain);
    frames((unsigned)(fade / 16667) + 8);
    expect(sound.state == SOUND_IDLE && fake.stops == 1 && fake.activations == 4, "faded out and idle (state %d stops %d)", sound.state, fake.stops);
    frames(20);
    expect(fake.activations == 4, "a faded repeating recording does not restart");

    /* A sequence start without a recording is ignored; be507e stops music; be507d (effects) does not. */
    const unsigned ignored = sound.ignored;
    calls = fake.calls;
    nm_sound_command(0xae2109);
    frames(30);
    expect(sound.ignored == ignored + 1 && sound.state == SOUND_IDLE && fake.calls == calls,
           "a voice start is ignored and an idle module asks AICAflow nothing");
    want(5, 0); frames(3);
    expect(sound.state == SOUND_PLAYING && fake.banks_loaded == 1, "resident recording restarted");
    nm_sound_command(0xbe507d); frames(3);
    expect(sound.state == SOUND_PLAYING, "effects stop leaves music");
    nm_sound_command(0xbe507e); frames(4);
    expect(sound.state == SOUND_IDLE && fake.stops == 2, "music stop (state %d)", sound.state);
    /* Results without the podium: b00700 (first of b0..b8 07 00) stops the recording; b70700 alone does not. */
    want(5, 0); frames(3);
    nm_sound_command(0xb70700); frames(3);
    expect(sound.state == SOUND_PLAYING, "one channel's volume leaves the recording");
    nm_sound_command(0xb00700); frames(4);
    expect(sound.state == SOUND_IDLE && fake.stops == 3, "b00700 stops the recording (state %d)", sound.state);
    /* At a loading pause a change completes inside nm_sound_settle: playing -> stopped -> started again. */
    want(5, 0); frames(3);
    const int activations = fake.activations;
    want(5, 0); nm_sound_settle();
    expect(sound.state == SOUND_PLAYING && sound.wanted < 0 && fake.activations == activations + 1, "settled restart");

    /* A stop whose end report is lost to a full report queue still completes: the handle goes stale. */
    want(5, 0); frames(3);
    want(0, 0); nm_sound_update();
    expect(sound.state == SOUND_ENDING && fake.stop_pending, "stopping (state %d)", sound.state);
    for (unsigned i = 0; i < ENDED_CAPACITY + 2; ++i) end_from_thread(0x20000u + i, AFX_DONE, 1);
    fake.stop_pending = 0; end_from_thread(fake.live, AFX_DONE, 1); /* freed; its report does not fit */
    expect(ended_head - ended_tail == ENDED_CAPACITY && !fake.live, "report queue full (%u)", ended_head - ended_tail);
    frames(1);
    expect(sound.state == SOUND_IDLE && !sound.instance, "stale handle ends the stop (state %d)", sound.state);
    want(5, 0); frames(1);
    expect(sound.state == SOUND_PLAYING, "plays again after the full queue");
    want(0, 0); frames(3);

    /* A recording whose files are missing: reported, nothing left half loaded. */
    want(6, 0); frames(3);
    expect(sound.state == SOUND_IDLE && fake.banks_live == 0 && fake.flows_live == 0 && sound.track == 0,
           "missing files leave nothing resident (banks %d flows %d)", fake.banks_live, fake.flows_live);

    /* A load in the idle waits: BREAK DOWN (409,166 bytes) with 3.5 ms a wait: no wait reads longer than that, the
     * bank arrives in order, the music starts at the end; nothing is read in an update. */
    want(7, 0);
    unsigned updates = 0, over = 0;
    uint64_t longest = 0;
    while (updates < 3000 && sound.state != SOUND_PLAYING) {
        frame(3500); ++updates;
        if (fake.read_us > longest) longest = fake.read_us;
        over += fake.read_us > 3500;
    }
    expect(sound.state == SOUND_PLAYING && sound.track == 7 && fake.cursor == 409166 && !fake.bad_data && !fake.dma_overlap,
           "BREAK DOWN plays after a load in the idle waits (state %d, %u updates)", sound.state, updates);
    expect(!over && longest > 2000 && updates > 300 && updates < 2000,
           "each wait reads within its time (longest %llu us, %u over, %u updates)", (unsigned long long)longest, over, updates);
    /* A wait without time for a read: nothing is read; and nothing ever in an update. */
    want(5, 0); frames(4);
    expect(sound.state == SOUND_LOADING, "loading track 05 (state %d)", sound.state);
    unsigned reads = fake.reads;
    for (unsigned i = 0; i < 50; ++i) frame(1500);
    for (unsigned i = 0; i < 50; ++i) { clock_us += 16683u; nm_sound_update(); deliver_stop(); }
    expect(fake.reads == reads && sound.state == SOUND_LOADING, "no read without time, none in an update (%u reads)", fake.reads - reads);
    until_playing(40);
    expect(sound.state == SOUND_PLAYING && sound.track == 5, "the load completes afterwards (state %d)", sound.state);
    /* A change of mind in the middle of a load: the half-loaded bank goes, the new one loads. */
    want(7, 0);
    for (unsigned i = 0; i < 40; ++i) frame(8000);
    expect(sound.state == SOUND_LOADING && fake.streaming && fake.cursor > 0 && fake.cursor < 409166, "BREAK DOWN half loaded (%u)", fake.cursor);
    want(0, 0); frames(1);
    expect(sound.state == SOUND_IDLE && !fake.streaming && !fake.banks_live && !fake.flows_live && !sound.track && !load.buffer,
           "a stop drops the load (state %d)", sound.state);
    want(7, 0);
    for (unsigned i = 0; i < 40; ++i) frame(8000);
    want(5, 0);
    until_playing(40);
    expect(sound.state == SOUND_PLAYING && sound.track == 5 && fake.banks_live == 1 && fake.flows_live == 1, "another track instead (state %d)", sound.state);
    /* A slow host (each read 10 ms more, as /pc on a busy machine): the fit learns it from the reads that overran
     * and sizes later reads by it (or none); once the host is fast again the load completes. */
    want(5, 0); frames(1); until_playing(40);
    fake.read_extra_us = 10000;
    want(7, 0);
    unsigned long overrun_reads = 0;
    for (unsigned i = 0; i < 1500; ++i) { frame(8000); overrun_reads += fake.read_us > 8000; }
    expect(sound.state == SOUND_LOADING && overrun_reads >= 1 && overrun_reads <= 4 && load.overruns == overrun_reads,
           "a slow host: %lu overrunning reads in 1500 waits (loader counted %lu)", overrun_reads, (unsigned long)load.overruns);
    fake.read_extra_us = 0;
    for (unsigned i = 0; i < 3000 && sound.state != SOUND_PLAYING; ++i) frame(20000);
    expect(sound.state == SOUND_PLAYING && sound.track == 7, "the load completes once there is time for it (state %d)", sound.state);
    /* At a loading pause the whole load runs inside nm_sound_settle. */
    want(5, 0); frames(1); until_playing(40);
    want(7, 0); nm_sound_settle();
    expect(sound.state == SOUND_PLAYING && sound.track == 7 && fake.cursor == 409166 && fake.read_max == 8192,
           "settled load (state %d, chunks up to %u)", sound.state, fake.read_max);

    /* Too little AICA RAM for a recording's PCM8 bank and flow (Advanced's t04 beside the effects' bank): its ADPCM
     * copy in music/adpcm/ is loaded instead; without one the PCM8 load is tried as before. */
    snprintf(path, sizeof(path), "%smusic/t04.afb", root); write_bank(path, 3000);
    snprintf(path, sizeof(path), "%smusic/t04.afx", root); write_file(path);
    snprintf(path, sizeof(path), "%smusic/adpcm", root); mkdir(path, 0700);
    snprintf(path, sizeof(path), "%smusic/adpcm/t04.afb", root); write_bank(path, 1500);
    snprintf(path, sizeof(path), "%smusic/adpcm/t04.afx", root); write_file(path);
    want(5, 0); frames(1); until_playing(40);
    fake.aica_free = 2500;
    want(4, 1); frames(1); until_playing(60);
    expect(sound.state == SOUND_PLAYING && sound.track == 4 && fake.stream_size == 1500 && fake.cursor == 1500 && !fake.bad_data,
           "the ADPCM copy when the PCM8 bank and flow have no room (state %d, bank %u)", sound.state, fake.stream_size);
    fake.aica_free = 0;
    want(4, 1); frames(1);
    want(5, 0); frames(1); until_playing(40);
    want(4, 1); frames(1); until_playing(60);
    expect(sound.state == SOUND_PLAYING && sound.track == 4 && fake.stream_size == 3000, "the PCM8 recording where it fits (bank %u)", fake.stream_size);

    /* Shutdown from playing: stopped, freed by AICAflow, released, AICAflow down. */
    want(5, 1); frames(1); until_playing(40);
    expect(sound.state == SOUND_PLAYING && sound.track == 5, "playing again after a reload");
    nm_sound_shutdown();
    expect(sound.state == SOUND_OFF && fake.shutdowns == 1 && fake.banks_live == 0 && fake.flows_live == 0 && !fake.live,
           "shutdown releases everything");
    expect(sfx_inits == 1 && sfx_commands >= 5 && sfx_updates > 10 && sfx_shutdowns == 1,
           "effects: started once (not after the boot timeout), every command and update handed on, shut down (%u %u %u %u)", sfx_inits, sfx_commands, sfx_updates, sfx_shutdowns);

    /* The map: bank 0x10 sequence starts, each command and each recording once, the four race pieces repeat. */
    unsigned char seen[46] = {0};
    const unsigned entries = sizeof(nm_music_map) / sizeof(nm_music_map[0]);
    expect(entries == 45, "45 recordings mapped (%u)", entries);
    for (unsigned i = 0; i < entries; ++i) {
        const nm_music_entry_t *e = &nm_music_map[i];
        expect((e->command >> 8) == 0xae10, "entry %u: %06lx is not a bank 0x10 start", i, (unsigned long)e->command);
        expect(e->track >= 1 && e->track <= 45 && !seen[e->track], "entry %u: track %u", i, e->track);
        if (e->track <= 45) seen[e->track] = 1;
        expect(e->loops == (e->track >= 3 && e->track <= 6), "entry %u: track %u loops %u", i, e->track, e->loops);
        expect(nm_music_find(e->command) == e, "entry %u: command %06lx listed twice", i, (unsigned long)e->command);
    }
    for (unsigned i = 0; i < 4; ++i)
        expect(nm_music_find(nm_music_race[i]) && nm_music_find(nm_music_race[i])->loops && nm_music_find(nm_music_finish[i]) &&
               nm_music_find(nm_music_podium[i]), "selection %u has its three recordings", i);
    expect(nm_music_find(NM_MUSIC_ADVERTISE) && nm_music_find(NM_MUSIC_GAME_START), "advertisement and game start mapped");

    snprintf(path, sizeof(path), "rm -r '%s'", root); if (system(path)) return 1;
    if (failures) return 1;
    puts("sound check: boot timeout, start, repeat on a reported end, nothing asked while playing, error end, fade, stops, end reports from another thread, full report queue, missing files, loads in the idle waits within their time, no read without time or in an update, a slow host, a load dropped, settle, shutdown, map and selection tables: pass");
    return 0;
}
