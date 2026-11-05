/* Host check of code/audio/sfx.c against a fake AICAflow (ABI 8, many instances): a missing
 * bank, loading a synthetic bank (sequences, a note, ten engine layers), the engine layers
 * keyed on and patched with only the words that changed, two commands in one update giving
 * one patch, a patch put off by a full queue, slip and skid layers, velocity gains, effect and
 * engine stops, commands without a flow, and shutdown freeing everything. Built by the
 * Makefile's check target with ASan/UBSan.
 *
 *   test_sfx [ASSET_ROOT [NATIVE.sound]]
 * With ASSET_ROOT: also loads ASSET_ROOT/sfx/race.afb/.dsx (recipes/build_sfx.sh). With a
 * command file of tests/trace_race_native.c --sound: replays it a frame at a time (one update
 * per frame, as the updater) and prints what reaches AICAflow per frame. */
#include "../code/audio/sfx.c"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAKE_INSTANCES 64u
#define FAKE_FLOWS 512u
static struct {
    uint32_t bank_low, bank_high; int banks_live;
    struct { int live; int controlled; } flow[FAKE_FLOWS + 1];
    unsigned flows_live, next_flow;
    struct { int live, stopping, controlled, running; unsigned age; afx_asset_t flow; uint32_t mask; uint16_t values[AFX_FIELD_COUNT]; } inst[FAKE_INSTANCES];
    uint16_t generation;
    unsigned activations, patches, gains, stops, engine_stops, full_once, refuse, park_late;
    uint8_t last_gain; int controlled_upload;
} fake;
static uint32_t controlled_keys_seen;
/* One-shots end after this many frames (replays: the announcer's phrases last 1 to 2 s). */
static unsigned fake_age = 30;

int afx_mem_stats(afx_mem_stats_t *s) { memset(s, 0, sizeof(*s)); s->free_bytes = 1000000; s->largest_free_block = 900000; return 0; }
int afx_bank_load_file(afx_bank_t *bank, const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned char h[32];
    if (!f) return -AFX_BAD_BOUNDS;
    const size_t n = fread(h, 1, 32, f); fclose(f);
    if (n != 32 || le32(h) != AFX_BANK_MAGIC) return -AFX_BAD_FORMAT;
    bank->asset = 1; bank->id.low = le32(h + 8); bank->id.high = le32(h + 12); bank->bytes = le32(h + 20);
    ++fake.banks_live; return 0;
}
int afx_bank_release(afx_bank_t *bank) { if (bank->asset) --fake.banks_live; *bank = (afx_bank_t){0}; return 0; }
/* The chunked sample-bank upload (host.h): one DMA in flight, complete on its second poll. */
static struct { afx_asset_t asset; uint32_t size, cursor, pending; int polls, finished, in_flight; unsigned dmas, begins; } stream;
int afx_sample_bank_stream_begin(uint32_t bytes, afx_asset_t *out) {
    if (!bytes) return -AFX_BAD_BOUNDS;
    stream = (__typeof__(stream)){2000 + stream.begins + 1, bytes, 0, 0, 0, 0, 0, stream.dmas, stream.begins + 1};
    ++fake.banks_live; *out = stream.asset; return 0;
}
int afx_sample_bank_stream_dma_begin(afx_asset_t bank, uint32_t offset, const void *data, uint32_t bytes) {
    if (bank != stream.asset || stream.in_flight) return -AFX_BUSY;
    if (((uintptr_t)data & 31u) || offset != stream.cursor || bytes > stream.size - offset ||
        ((bytes & 31u) && offset + bytes != stream.size)) return -AFX_BAD_BOUNDS;
    stream.in_flight = 1; stream.pending = bytes; stream.polls = 0; ++stream.dmas; return 0;
}
int afx_sample_bank_stream_dma_poll(afx_asset_t bank, bool *complete) {
    *complete = false;
    if (bank != stream.asset || !stream.in_flight) return -AFX_BAD_BOUNDS;
    if (++stream.polls < 2) return 0;
    stream.in_flight = 0; stream.cursor += stream.pending; *complete = true; return 0;
}
int afx_sample_bank_stream_finish(afx_asset_t bank) {
    if (bank != stream.asset || stream.in_flight || stream.cursor != stream.size) return -AFX_BAD_BOUNDS;
    stream.finished = 1; return 0;
}
int afx_bank_flow_upload(const afx_bank_t *bank, const void *data, uint32_t size, afx_asset_t *out) {
    if (!bank->asset || !data || size < 16 || fake.next_flow >= FAKE_FLOWS) return -AFX_BAD_FORMAT;
    const afx_asset_t flow = ++fake.next_flow + 100;
    fake.flow[fake.next_flow].live = 1;
    /* Synthetic flows say "CTRL" when controlled; a real AFX has its flags at 12. */
    fake.flow[fake.next_flow].controlled = !memcmp(data, "CTRL", 4) || (!memcmp(data, "AFX2", 4) && (le32((const uint8_t *)data + 12) & 1));
    ++fake.flows_live; *out = flow; return 0;
}
static int flow_index(afx_asset_t flow) { return flow > 100 && flow - 100 <= fake.next_flow ? (int)(flow - 100) : -1; }
int afx_asset_free(afx_asset_t asset) {
    if (asset > 2000 && asset == stream.asset) { stream.asset = 0; --fake.banks_live; return 0; } /* unfinished stream */
    const int i = flow_index(asset);
    if (i < 0 || !fake.flow[i].live) return -AFX_INVALID_HANDLE;
    for (unsigned k = 0; k < FAKE_INSTANCES; ++k) if (fake.inst[k].live && fake.inst[k].flow == asset) return -AFX_BUSY;
    fake.flow[i].live = 0; --fake.flows_live; return 0;
}
static int resolve(afx_instance_t i) {
    const unsigned k = (i & 0xffffu) - 1u;
    return k < FAKE_INSTANCES && fake.inst[k].live && (i >> 16) == fake.generation ? (int)k : -1;
}
int afx_instance_activate(afx_asset_t flow, afx_instance_t *out) {
    const int f = flow_index(flow);
    if (f < 0 || !fake.flow[f].live) return -AFX_INVALID_HANDLE;
    if (fake.refuse) { --fake.refuse; return -AFX_NO_CHANNELS; }
    for (unsigned k = 0; k < FAKE_INSTANCES; ++k)
        if (!fake.inst[k].live) {
            fake.inst[k] = (__typeof__(fake.inst[k])){1, 0, fake.flow[f].controlled, (int)fake.park_late, 0, flow, 0, {0}};
            ++fake.activations; *out = ((uint32_t)fake.generation << 16) | (k + 1u); return 0;
        }
    return -AFX_NO_FLOW_SLOTS;
}
int afx_instance_gain(afx_instance_t i, uint8_t gain) { if (resolve(i) < 0) return -AFX_STALE_GENERATION; ++fake.gains; fake.last_gain = gain; return 0; }
int afx_instance_stop(afx_instance_t i) {
    const int k = resolve(i);
    if (k < 0) return -AFX_STALE_GENERATION;
    ++fake.stops; fake.engine_stops += fake.inst[k].controlled; fake.inst[k].stopping = 1; return 0;
}
int afx_instance_patch(afx_instance_t i, uint8_t channel, uint32_t mask, const uint16_t *values) {
    const int k = resolve(i);
    if (k < 0) return -AFX_STALE_GENERATION;
    if (channel || !mask || (mask & ~AFX_PATCH_FIELD_MASK) || !fake.inst[k].controlled) return -AFX_BAD_COMMAND;
    if (fake.full_once) { fake.full_once = 0; return -AFX_IPC_FULL; }
    unsigned n = 0;
    for (unsigned f = 0; f < AFX_FIELD_COUNT; ++f) if (mask & (1u << f)) fake.inst[k].values[f] = values[n++];
    fake.inst[k].mask |= mask; ++fake.patches; return 0;
}
int afx_instance_status(afx_instance_t i, afx_instance_status_t *out) {
    const int k = resolve(i);
    if (k < 0) return -AFX_STALE_GENERATION;
    memset(out, 0, sizeof(*out)); out->state = fake.inst[k].controlled && !fake.inst[k].running ? AFX_PARKED : AFX_RUNNING; return 0;
}
/* AICAflow's event thread between updates: stopped instances and one-shots older than 30 frames end. */
static void frame(void) {
    for (unsigned k = 0; k < FAKE_INSTANCES; ++k) fake.inst[k].running = 0; /* a new flow has run to its PARK */
    for (unsigned k = 0; k < FAKE_INSTANCES; ++k)
        if (fake.inst[k].live && (fake.inst[k].stopping || (!fake.inst[k].controlled && ++fake.inst[k].age > fake_age)))
            fake.inst[k].live = 0;
}
static void wait_ms(unsigned ms) { (void)ms; frame(); }
static unsigned live(int controlled) {
    unsigned n = 0;
    for (unsigned k = 0; k < FAKE_INSTANCES; ++k) n += fake.inst[k].live && fake.inst[k].controlled == controlled;
    return n;
}
/* Engine layers keyed and not patched silent (DIRECT 0, TL 255). */
static unsigned audible(void) {
    unsigned n = 0;
    for (unsigned k = 0; k < FAKE_INSTANCES; ++k)
        n += fake.inst[k].live && fake.inst[k].controlled && fake.inst[k].values[AFX_FIELD_DIRECT] &&
             (fake.inst[k].values[AFX_FIELD_MIX] >> 8) != 0xff;
    return n;
}
static const uint16_t *layer_values(unsigned l) {
    const int k = resolve(sfx.layer[l].instance);
    return k < 0 ? NULL : fake.inst[k].values;
}

#ifdef NM_SFX_VOICE_STREAM
static uint32_t dcload_cost(uint32_t bytes) { return 1200u + 3u * bytes; }
#endif
static int failures;
static void expect(int ok, const char *what, ...) {
    if (ok) return;
    va_list args; va_start(args, what);
    fputs("FAIL: ", stdout); vprintf(what, args); fputc('\n', stdout);
    va_end(args); ++failures;
}

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
/* A bank and a .dsx in prepare_sfx's layout: flows of 16 bytes, "CTRL" for the engine layers.
 * Table: PITCH 0x1000+layer*0x100+row, MIX 0x2000+row, DIRECT 0x3000+layer (by row for layer 8). */
static void write_assets(const char *root) {
    char path[256];
    snprintf(path, sizeof(path), "%s/sfx", root); mkdir(path, 0700);
    unsigned char afb[64] = {0};
    put32(afb, AFX_BANK_MAGIC); put32(afb + 8, 0x1111); put32(afb + 12, 0x2222); put32(afb + 20, 32);
    snprintf(path, sizeof(path), "%s/sfx/race.afb", root);
    FILE *f = fopen(path, "wb"); fwrite(afb, 1, sizeof(afb), f); fclose(f);
    static unsigned char d[16384];
    memset(d, 0, sizeof(d));
    uint32_t at = 36;
    memcpy(d, "DSX1", 4); put32(d + 4, 1); put32(d + 28, 0x1111); put32(d + 32, 0x2222);
    put32(d + 20, at); for (unsigned v = 0; v < 128; ++v) d[at + v] = (unsigned char)(v == 127 ? 255 : v * 2); at += 128;
    put32(d + 16, at); memcpy(d + at, "ENG1", 4); put32(d + at + 4, 10); at += 8;
    for (unsigned l = 0; l < 10; ++l) {
        const unsigned rows = l < 8 ? 128 : 16;
        put16(d + at, (uint16_t)rows); at += 4;
        for (unsigned r = 0; r < rows; ++r) put16(d + at + 2 * r, (uint16_t)(0x1000 + l * 0x100 + r));
        for (unsigned r = 0; r < rows; ++r) put16(d + at + 2 * (rows + r), (uint16_t)(0x2000 + r));
        for (unsigned r = 0; r < rows; ++r) put16(d + at + 2 * (2 * rows + r), (uint16_t)(0x3000 + l + (l == 8 ? r : 0)));
        at += rows * 6;
    }
    const uint32_t keys[] = {0x9e0700, 0xae2109, 0xae210c, 0xbe0000, 0xbe0001, 0xbe0002, 0xbe0003, 0xbe0004,
                             0xbe0005, 0xbe0006, 0xbe0007, 0xbe0008, 0xbe0009};
    const unsigned count = sizeof(keys) / sizeof(keys[0]);
    put32(d + 8, count); put32(d + 12, at);
    uint32_t flows = at + 16 * count;
    for (unsigned i = 0; i < count; ++i) {
        unsigned char *e = d + at + 16 * i;
        const unsigned kind = keys[i] >= 0xbe0000 ? 2 : keys[i] >= 0xae0000 ? 0 : 1;
        put32(e, keys[i]); e[4] = (unsigned char)kind; e[5] = (unsigned char)(kind == 2 ? keys[i] & 15 : kind == 0 ? 4 : 0);
        put32(e + 8, flows); put32(e + 12, 16);
        memcpy(d + flows, kind == 2 ? "CTRL" : "FLOW", 4); flows += 16;
    }
    put32(d + 24, flows);
    snprintf(path, sizeof(path), "%s/sfx/race.dsx", root);
    f = fopen(path, "wb"); fwrite(d, 1, flows, f); fclose(f);
    /* sfx/voices.pak (build_sfx.sh --voices stream): three phrases, a bank of 9000 sample bytes (two DMA chunks)
     * and a 16-byte flow each. */
    static unsigned char v[32768];
    memset(v, 0, sizeof(v));
    memcpy(v, "VOX1", 4); put32(v + 4, 1); put32(v + 8, 3);
    const uint32_t phrase[3] = {0xae2105, 0xae2111, 0xae2114};
    uint32_t vat = 96;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned char *e = v + 12 + 20 * i;
        const uint32_t bank = 32 + 9000, flow_at = vat + ((bank + 31u) & ~31u);
        put32(e, phrase[i]); put32(e + 4, vat); put32(e + 8, bank); put32(e + 12, flow_at); put32(e + 16, 16);
        put32(v + vat, AFX_BANK_MAGIC); put32(v + vat + 4, 1); put32(v + vat + 8, phrase[i]);
        put32(v + vat + 16, 32); put32(v + vat + 20, 9000); put32(v + vat + 24, bank);
        memcpy(v + flow_at, "FLOW", 4);
        vat = flow_at + 32;
    }
    snprintf(path, sizeof(path), "%s/sfx/voices.pak", root);
    f = fopen(path, "wb"); fwrite(v, 1, vat, f); fclose(f);
}

/* tests/trace_race_native.c --sound: commands by reference frame, one update per frame. */
/* With a hints file (tests/trace_race_native.c --hints) and a read budget a frame (NM_SFX_VOICE_STREAM): each
 * frame's hints, then one idle-wait read in the budget, on the dcload clock. */
static FILE *replay_hints;
static uint32_t replay_budget_us;
static void replay_frame(long g) {
#ifdef NM_SFX_VOICE_STREAM
    static char line[256];
    static long at = -1;
    static uint32_t keys[8];
    static unsigned n;
    while (replay_hints && at < g) {
        if (!fgets(line, sizeof(line), replay_hints)) { replay_hints = NULL; break; }
        char *p = line;
        if (sscanf(p, "frame=%ld", &at) != 1) continue;
        n = 0; p = strchr(p, ' ');
        while (p && n < 8) { keys[n++] = (uint32_t)strtoul(p, &p, 16); if (*p != ' ') break; }
    }
    if (at >= g && at <= g + 1) nm_sfx_hint(keys, n);
    if (replay_budget_us) voice_idle_read(replay_budget_us);
    voice_fake_us += 16683;
#else
    (void)g;
#endif
}
static void replay(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("cannot read %s\n", path); ++failures; return; }
    long frame_now = -1, first = -1, last = 0, previous = -1;
    unsigned commands = 0, frames_with_patches = 0, worst = 0, worst_frame = 0, worst_starts = 0;
    unsigned long total = 0;
    unsigned histogram[12] = {0};
    char line[128];
    (void)replay_hints; (void)replay_budget_us;
    nm_sfx_stats_t before = sfx.stats;
    unsigned fake_before = fake.patches;
    for (;;) {
        long at = -1; unsigned long cmd = 0;
        const int got = fgets(line, sizeof(line), f) && sscanf(line, "frame=%ld tick=%*ld cmd=%lx", &at, &cmd) == 2;
        if (!got || at != frame_now) {
            if (frame_now >= 0) {
                /* Every frame from the previous command's to this one: the updater runs each frame. */
                for (long g = previous < 0 ? frame_now : previous + 1; g <= frame_now; ++g) {
                    const unsigned p0 = fake.patches, a0 = fake.activations, s0 = fake.stops;
                    replay_frame(g);
                    nm_sfx_update(); frame();
                    const unsigned work = fake.patches - p0 + fake.activations - a0 + fake.stops - s0;
                    total += work; ++histogram[work < 11 ? work : 11];
                    frames_with_patches += work != 0;
                    if (work > worst) { worst = work; worst_frame = (unsigned)g; worst_starts = fake.activations - a0; }
                }
                previous = frame_now; last = frame_now;
            }
            if (!got) break;
            frame_now = at; if (first < 0) first = at;
        }
        nm_sfx_command((uint32_t)cmd); ++commands;
    }
    fclose(f);
    printf("replay %s: %u commands over frames %ld..%ld; AICAflow commands from effects and engine: %lu "
           "(patches %u, one-shots started %u, without a flow %u, refused %u), %u frames with any, at most %u in a frame "
           "(frame %u, %u starts); frames by command count 0..10, 11+:",
           path, commands, first, last, total, fake.patches - fake_before, sfx.stats.started - before.started,
           sfx.stats.missing - before.missing, sfx.stats.dropped - before.dropped, frames_with_patches, worst, worst_frame, worst_starts);
    for (unsigned i = 0; i < 12; ++i) printf(" %u", histogram[i]);
    putchar('\n');
}

int main(int argc, char **argv) {
    char root[64] = "/tmp/nm-sfx-XXXXXX", slash[80];
    if (!mkdtemp(root)) return perror("mkdtemp"), 1;
    snprintf(slash, sizeof(slash), "%s/", root);
    fake.generation = 1;

    expect(!nm_sfx_init(slash) && !sfx.up && !fake.banks_live, "no bank: no effects");
    nm_sfx_command(0xae2109); nm_sfx_update(); expect(!fake.activations, "without effects nothing starts");

    write_assets(root);
    expect(nm_sfx_init(slash) && sfx.count == 13 && fake.flows_live == 13 && sfx.words, "the synthetic bank loads (%u)", sfx.count);

    /* Engine on: rpm 1 keys layers 0..7 on; once their flows have parked (a patch before would land before the
     * flow's NOTE, which overwrites it), every word sent once. */
    fake.park_late = 1;
    nm_sfx_command(0xbe1701); nm_sfx_update();
    expect(live(1) == 8 && fake.patches == 0, "rpm 1: eight layers keyed on, no patch before they park (%u)", fake.patches);
    nm_sfx_update(); expect(fake.patches == 0, "still running: no patch");
    fake.park_late = 0; frame(); nm_sfx_update();
    expect(live(1) == 8 && fake.patches == 8, "rpm 1: eight layers keyed on and patched (%u, %u)", live(1), fake.patches);
    const uint16_t *v = layer_values(0);
    expect(v && v[AFX_FIELD_PITCH] == 0x1001 && v[AFX_FIELD_MIX] == 0x2001 && v[AFX_FIELD_DIRECT] == 0x3000, "layer 0 row 1");
    v = layer_values(6);
    expect(v && v[AFX_FIELD_PITCH] == 0x1601 && v[AFX_FIELD_MIX] == 0x2000, "layer 6: pitch by rpm, level by be34 (0)");
    nm_sfx_update(); expect(fake.patches == 8, "nothing changed: no patch");
    /* Two rpm values in one update: one patch per layer, PITCH and MIX only. */
    const unsigned p = fake.patches;
    nm_sfx_command(0xbe1702); nm_sfx_command(0xbe1703); nm_sfx_update();
    v = layer_values(1);
    expect(fake.patches == p + 8 && v && v[AFX_FIELD_PITCH] == 0x1103, "coalesced: one patch per layer (%u)", fake.patches - p);
    expect(fake.inst[resolve(sfx.layer[1].instance)].mask == ((1u << AFX_FIELD_PITCH) | (1u << AFX_FIELD_MIX) | (1u << AFX_FIELD_DIRECT)),
           "the first patch had all three words");
    /* Level of the opponent layers changes only their MIX. */
    const unsigned p2 = fake.patches;
    nm_sfx_command(0xbe342d); nm_sfx_update();
    expect(fake.patches == p2 + 1 && layer_values(6)[AFX_FIELD_MIX] == 0x202d, "be34: layer 6's MIX only");
    /* A full queue: put off, sent the next update. */
    fake.full_once = 1; nm_sfx_command(0xbe1704); nm_sfx_update();
    expect(sfx.stats.retries == 1 && layer_values(0)[AFX_FIELD_PITCH] == 0x1003, "queue full: layer 0 waits");
    nm_sfx_update(); expect(layer_values(0)[AFX_FIELD_PITCH] == 0x1004, "and is sent on the next update");
    /* Slip: layer 8 on at row 9 (DIRECT by row there), off again at 0. */
    nm_sfx_command(0xbe1909); nm_sfx_update();
    expect(layer_values(8) && layer_values(8)[AFX_FIELD_DIRECT] == 0x3011 && live(1) == 9, "slip 9: layer 8 keyed on");
    nm_sfx_command(0xbe1900); nm_sfx_update(); frame(); nm_sfx_update();
    expect(live(1) == 9 && audible() == 8 && layer_values(8)[AFX_FIELD_DIRECT] == 0 && layer_values(8)[AFX_FIELD_MIX] == 0xff00 &&
           !fake.engine_stops, "slip 0: layer 8 silent, still keyed");
    const unsigned slip_activations = fake.activations;
    nm_sfx_command(0xbe1909); nm_sfx_update();
    expect(audible() == 9 && layer_values(8)[AFX_FIELD_DIRECT] == 0x3011 && fake.activations == slip_activations,
           "slip 9 again: its words, no new key-on");
    nm_sfx_command(0xbe1900); nm_sfx_update();
    /* One-shots: a sequence, a note at a velocity, one without a flow, one refused. */
    const unsigned a = fake.activations;
    nm_sfx_command(0xae2109); nm_sfx_command(0x9e073f);
    expect(fake.activations == a + 2 && fake.last_gain == 0x7e && fake.gains == 1, "sequence and note started, velocity 3f gain 7e");
    nm_sfx_command(0x9e077f); expect(fake.gains == 1, "velocity 7f: no gain command");
    nm_sfx_command(0xae2177); nm_sfx_command(0x9e2000); nm_sfx_command(0x9e3f7f);
    expect(sfx.stats.missing == 2, "no flow: counted (%u); velocity 0 is no note", sfx.stats.missing);
    fake.refuse = 1; nm_sfx_command(0xae210c); expect(sfx.stats.dropped == 1, "refused: counted");
    nm_sfx_command(0xae1007); nm_sfx_command(0xbe1a10); expect(sfx.stats.missing == 2, "music commands are not effects");
    /* be507d: the effects stop, not the engine. */
#ifdef NM_SFX_VOICE_STREAM
    {   /* Streamed phrases: two wait, a third is dropped; one at a time, each freed when it has ended. */
        const nm_sfx_stats_t before = sfx.stats;
        const int banks = fake.banks_live;
        expect(sfx.stats.voices == 3, "voices.pak: three phrases (%u)", sfx.stats.voices);
        nm_sfx_command(0xae2111); nm_sfx_command(0xae2114); nm_sfx_command(0xae2105); nm_sfx_command(0xae2199);
        expect(sfx.stats.voice_drops == before.voice_drops + 1 && sfx.stats.missing == before.missing + 1,
               "a third phrase dropped, one not in the pack missing");
        nm_sfx_update(); /* read */
        nm_sfx_update(); /* the samples' allocation */
        expect(!sfx.stats.voice_starts && stream.begins == 1 && !stream.dmas, "allocated, no DMA yet");
        unsigned updates = 0;
        while (!sfx.stats.voice_starts && updates < 20) { nm_sfx_update(); ++updates; }
        expect(sfx.stats.voice_starts == 1 && stream.dmas == 1 && stream.finished && updates >= 2 && fake.banks_live == banks + 1,
               "the first phrase plays after its DMA, polled over %u updates (never waiting)", updates);
        nm_sfx_command(0xae2105); nm_sfx_update(); nm_sfx_quiesce();
        expect(!stream.in_flight, "quiesce: no chunk left on the bus");
        for (unsigned i = 0; i < 60; ++i) { frame(); nm_sfx_update(); }
        expect(sfx.stats.voice_starts >= 2 && stream.begins >= 2, "the second after the first ended (%u)", sfx.stats.voice_starts);
        for (unsigned i = 0; i < 80; ++i) { frame(); nm_sfx_update(); }
        for (unsigned i = 0; i < 40; ++i) { frame(); nm_sfx_update(); }
        expect(fake.banks_live == banks && voice.stage == VOICE_IDLE && !voice.queued && sfx.stats.voice_starts == 3,
               "all three played and freed (%d banks, %u started)", fake.banks_live, sfx.stats.voice_starts);
    }

#endif
    nm_sfx_command(0xbe507d); frame();
    expect(live(0) == 0 && audible() == 8, "be507d: effects stop (%u), engine stays (%u)", live(0), audible());
    nm_sfx_command(0xbe507e); nm_sfx_update(); expect(audible() == 8, "be507e: music only");
    nm_sfx_command(0xbe5000); nm_sfx_update(); frame();
    expect(audible() == 0 && live(1) == 9 && !fake.engine_stops, "be5000: the engine silent too, its layers keyed");
    const unsigned engine_activations = fake.activations;
    nm_sfx_command(0xbe1705); nm_sfx_update();
    expect(audible() == 8 && fake.activations == engine_activations && layer_values(0)[AFX_FIELD_PITCH] == 0x1005,
           "the next engine command: the layers' words again, no new key-on");
    nm_sfx_command(0xbe1700); nm_sfx_update(); frame();
    expect(audible() == 0 && live(1) == 9 && !fake.engine_stops, "rpm 0: layers silent, keyed (%u audible, %u keyed, %u stops)",
           audible(), live(1), fake.engine_stops);
    nm_sfx_command(0xbe1701); nm_sfx_update(); nm_sfx_command(0xae2109);
    nm_sfx_shutdown(wait_ms);
    expect(!sfx.up && !fake.flows_live && !fake.banks_live && !live(0) && !live(1), "shutdown frees everything (%u flows, %d banks)",
           fake.flows_live, fake.banks_live);
#ifdef NM_SFX_VOICE_STREAM
    {   /* The main-loop reading of /pc (dcload): reads only in the idle wait, sized by the fitted cost; repeats and
         * prefetched phrases start at once. The clock: dcload as measured (a call 1.2 ms plus 3 us a byte). */
        voice_force_mainloop = 1;
        voice_fake_read_cost = dcload_cost;
        memset(&fake, 0, sizeof(fake)); fake.generation = 1; stream = (__typeof__(stream)){0};
        expect(nm_sfx_init(slash) && voice.mainloop, "re-init: main-loop reads");
        nm_sfx_command(0xae2111);
        nm_sfx_update(); nm_sfx_update();
        expect(voice.reading && !voice.read_pos && !sfx.stats.voice_starts, "nothing read outside the idle wait");
        unsigned frames = 0;
        while (!sfx.stats.voice_starts && frames < 200) { voice_idle_read(3500); nm_sfx_update(); ++frames; }
        expect(sfx.stats.voice_starts == 1 && !sfx.stats.voice_overruns, "on demand: plays after %u frames, no read over its budget",
               frames);
        printf("dcload model (1.2 ms a call + 3 us a byte, 3.5 ms a frame): a 9 KB phrase on demand starts after %u frames\n", frames);
        for (unsigned i = 0; i < 60; ++i) { frame(); nm_sfx_update(); }
        const unsigned before = sfx.stats.voice_hits;
        nm_sfx_command(0xae2111); nm_sfx_update(); nm_sfx_update();
        expect(sfx.stats.voice_hits == before + 1 && voice.stage == VOICE_UPLOADING, "a repeat: found read, uploading at once");
        for (unsigned i = 0; i < 60; ++i) { frame(); nm_sfx_update(); }
        const uint32_t hinted[2] = {0xae2114, 0xae2105};
        nm_sfx_hint(hinted, 2);
        for (unsigned i = 0; i < 200 && sfx.stats.voice_prefetches < 2; ++i) { voice_idle_read(3500); nm_sfx_update(); }
        for (unsigned i = 0; i < 200 && voice.reading; ++i) { voice_idle_read(3500); nm_sfx_update(); }
        const uint32_t served = sfx.stats.voice_starts;
        nm_sfx_command(0xae2114);
        unsigned waited = 0;
        while (sfx.stats.voice_starts == served && waited < 50) { nm_sfx_update(); ++waited; }
        expect(sfx.stats.voice_prefetches == 2 && waited <= 4, "prefetched: plays %u updates after its request", waited);
        printf("prefetched or repeated phrases start %u updates after the request (the chunked upload)\n", waited);
        for (unsigned i = 0; i < 60; ++i) { frame(); nm_sfx_update(); }
        voice_force_mainloop = 0; voice_fake_read_cost = NULL;
        nm_sfx_shutdown(wait_ms);
        expect(!fake.banks_live && !fake.flows_live, "main-loop reads: shutdown frees everything");
    }
#endif

    if (argc > 1) {
        /* The real bank of recipes/build_sfx.sh: parsed, every flow uploaded, the engine table read. */
        memset(&fake, 0, sizeof(fake)); fake.generation = 1;
#ifdef NM_SFX_VOICE_STREAM
        /* test_sfx ROOT SOUND HINTS BUDGET_US: the /pc reading on the dcload clock, phrases of 1.5 s. */
        if (argc > 4) {
            voice_force_mainloop = 1; voice_fake_read_cost = dcload_cost; fake_age = 90;
            replay_hints = fopen(argv[3], "r"); replay_budget_us = (uint32_t)strtoul(argv[4], NULL, 10);
            expect(replay_hints != NULL, "hints %s", argv[3]);
        }
#endif
        expect(nm_sfx_init(argv[1]) && sfx.words, "%ssfx/race.* loads", argv[1]);
        unsigned controlled = 0;
        for (unsigned i = 0; i < sfx.count; ++i) controlled += sfx.entries[i].kind == SFX_LAYER;
        expect(controlled == 10, "ten engine layers (%u)", controlled);
        int samples = 1;
        for (unsigned l = 0; l < SFX_LAYERS; ++l) samples &= sfx.table[l].sample && sfx.table[l].sample < 0x200000u;
        expect(samples && sfx.table[0].sample == 0x58100u, "every layer's sample address read (layer 0 %05lx)",
               (unsigned long)sfx.table[0].sample);
        expect(sfx.table[0].word[WORD_PITCH][1] != sfx.table[0].word[WORD_PITCH][127], "layer 0: pitch rises with rpm");
        printf("assets %s: %u flows (%u engine layers), gains v1 %u v3f %u v7f %u\n", argv[1], sfx.count, controlled,
               sfx.gains[1], sfx.gains[0x3f], sfx.gains[0x7f]);
        if (argc > 2) replay(argv[2]);
        nm_sfx_shutdown(wait_ms);
        expect(!fake.flows_live && !fake.banks_live, "real bank: shutdown frees everything");
    }
    (void)controlled_keys_seen;
    char cmd[128]; snprintf(cmd, sizeof(cmd), "rm -r '%s'", root); if (system(cmd)) return 1;
    if (failures) return 1;
#ifdef NM_SFX_VOICE_STREAM
    puts("sfx check (streamed phrases): queue of two, drop, read, chunked DMA over updates without waiting, quiesce, start, free after the end, then the next: pass");
#endif
    puts("sfx check: missing bank, load, engine layers keyed on and patched with changed words only, coalescing, full queue, slip layer, gains, missing flows, refusals, effect stops, the engine silent and on again without a new key-on, shutdown: pass");
    return 0;
}
