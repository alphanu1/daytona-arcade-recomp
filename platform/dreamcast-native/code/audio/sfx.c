/* Sound effects and voices on the AICA through AICAflow: see include/audio/sfx.h. */
#include "audio/sfx.h"
#include <aicaflow/bank.h>
#include <aicaflow/format.h>
#include <aicaflow/host.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nm_mem.h"

#define SFX_LAYERS 10u
#define SFX_RING 32u /* effect instances remembered for a stop */
enum { SFX_SEQUENCE, SFX_NOTE, SFX_LAYER };
enum { WORD_PITCH, WORD_DIRECT, WORD_MIX, WORDS };

typedef struct { uint32_t key; uint8_t kind, track; afx_asset_t flow; } sfx_entry_t;
typedef struct {
    afx_instance_t instance;
    uint8_t want, sent_valid;
    uint8_t parked;                        /* its flow has run its NOTE and parked: patches stick from now on */
    uint8_t reported;                      /* diagnostics: an end or a refusal already on the console */
    uint16_t word[WORDS], sent[WORDS];
} sfx_layer_t;
/* Layers 0..7 have a row per rpm (or opponent level), 8 and 9 one per slip or skid value. */
#define SFX_TABLE_WORDS (8u * 128u * 3u + 2u * 16u * 3u)

static struct {
    int up;
    afx_bank_t bank;
    sfx_entry_t *entries;
    unsigned count;
    uint8_t gains[128];
    uint16_t *words;                       /* engine table, owned */
    struct { unsigned rows; const uint16_t *word[WORDS]; afx_asset_t flow; uint32_t sample; } table[SFX_LAYERS];
    sfx_layer_t layer[SFX_LAYERS];
    uint8_t controls[128];
    afx_instance_t ring[SFX_RING];
    unsigned ring_next, sample_warnings;
    unsigned engine_updates;               /* diagnostics: updates since the layers last came on (0: off) */
    nm_sfx_stats_t stats;
} sfx;

/* The announcer's streamed phrases (NM_SFX_VOICE_STREAM, below); without the flag they do nothing. */
static void voice_init(const char *root);
static int voice_request(uint32_t key);
static void voice_update(void);
static void voice_shutdown(void (*wait_ms)(unsigned));
static void voice_quiesce(void);
static void voice_hint(const uint32_t *keys, unsigned n);

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void free_flows(void) {
    for (unsigned i = 0; i < sfx.count; ++i)
        if (sfx.entries[i].flow) {
            const int result = afx_asset_free(sfx.entries[i].flow);
            if (result) printf("Sound effects: freeing flow %06lx failed (%d)\n", (unsigned long)sfx.entries[i].key, result);
            sfx.entries[i].flow = AFX_ASSET_INVALID;
        }
    free(sfx.entries); sfx.entries = NULL; sfx.count = 0;
    free(sfx.words); sfx.words = NULL;
    if (sfx.bank.asset) afx_bank_release(&sfx.bank);
    sfx.bank = (afx_bank_t){0};
}

static unsigned char *read_all(const char *path, uint32_t *size) {
    FILE *file = fopen(path, "rb");
    long bytes = -1;
    unsigned char *data = NULL;
    if (file && !fseek(file, 0, SEEK_END)) bytes = ftell(file);
    if (bytes > 36 && !fseek(file, 0, SEEK_SET) && (data = nm_alloc((size_t)bytes)) &&
        fread(data, 1, (size_t)bytes, file) != (size_t)bytes) { free(data); data = NULL; }
    if (file) fclose(file);
    *size = data ? (uint32_t)bytes : 0;
    return data;
}

/* The engine table of the .dsx: "ENG1", 10, then per layer rows, 0 and rows PITCH, MIX, DIRECT words. */
static int load_engine(const unsigned char *dsx, uint32_t size, uint32_t at) {
    if (!at) return 1; /* A bank without engine layers. */
    if (at > size || size - at < 8 || le32(dsx + at) != 0x31474e45u || le32(dsx + at + 4) != SFX_LAYERS) return 0;
    if (!(sfx.words = nm_alloc(SFX_TABLE_WORDS * sizeof(*sfx.words)))) return 0;
    const unsigned char *p = dsx + at + 8;
    uint32_t left = size - at - 8, used = 0;
    for (unsigned l = 0; l < SFX_LAYERS; ++l) {
        const unsigned rows = left >= 4 ? le16(p) : 0;
        if (rows != (l < 8 ? 128u : 16u) || left - 4 < rows * 6u) return 0;
        uint16_t *out = sfx.words + used;
        for (unsigned i = 0; i < rows * 3u; ++i) out[i] = le16(p + 4 + 2 * i);
        sfx.table[l].rows = rows;
        sfx.table[l].word[WORD_PITCH] = out;
        sfx.table[l].word[WORD_MIX] = out + rows;
        sfx.table[l].word[WORD_DIRECT] = out + 2 * rows;
        used += rows * 3u; p += 4 + rows * 6u; left -= 4 + rows * 6u;
    }
    return 1;
}

static const sfx_entry_t *find(uint32_t key) {
    unsigned lo = 0, hi = sfx.count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (sfx.entries[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo < sfx.count && sfx.entries[lo].key == key ? &sfx.entries[lo] : NULL;
}

int nm_sfx_init(const char *root) {
    char path[256];
    uint32_t size = 0;
    unsigned char *dsx = NULL;
    if (sfx.up || !root) return sfx.up;
    nm_memset(&sfx, 0, sizeof(sfx));
    snprintf(path, sizeof(path), "%ssfx/race.afb", root);
    int result = afx_bank_load_file(&sfx.bank, path);
    if (result) { printf("Sound effects: cannot load %s (%d): none\n", path, result); sfx.bank = (afx_bank_t){0}; return 0; }
    snprintf(path, sizeof(path), "%ssfx/race.dsx", root);
    if (!(dsx = read_all(path, &size)) || memcmp(dsx, "DSX1", 4) || le32(dsx + 4) != 1 || le32(dsx + 24) != size ||
        le32(dsx + 28) != sfx.bank.id.low || le32(dsx + 32) != sfx.bank.id.high) {
        printf("Sound effects: %s is missing or not for this bank: none\n", path);
        goto fail;
    }
    const uint32_t count = le32(dsx + 8), directory = le32(dsx + 12), gains = le32(dsx + 20);
    if (!count || count > 4096 || directory > size || (size - directory) / 16 < count || gains > size || size - gains < 128 ||
        !(sfx.entries = nm_calloc(count, sizeof(*sfx.entries))) || !load_engine(dsx, size, le32(dsx + 16))) {
        printf("Sound effects: %s is malformed: none\n", path);
        goto fail;
    }
    nm_memcpy(sfx.gains, dsx + gains, 128);
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *e = dsx + directory + 16 * i;
        const uint32_t key = le32(e), offset = le32(e + 8), bytes = le32(e + 12);
        sfx_entry_t *entry = &sfx.entries[sfx.count];
        if ((i && key <= sfx.entries[sfx.count - 1].key) || e[4] > SFX_LAYER || offset > size || bytes > size - offset ||
            (e[4] == SFX_LAYER && e[5] >= SFX_LAYERS)) {
            printf("Sound effects: %s entry %lu is malformed: none\n", path, (unsigned long)i);
            goto fail;
        }
        *entry = (sfx_entry_t){key, e[4], e[5], AFX_ASSET_INVALID};
        ++sfx.count;
        if ((result = afx_bank_flow_upload(&sfx.bank, dsx + offset, bytes, &entry->flow))) {
            printf("Sound effects: flow %06lx not accepted (%d): none\n", (unsigned long)key, result);
            entry->flow = AFX_ASSET_INVALID;
            goto fail;
        }
        if (entry->kind == SFX_LAYER) {
            sfx.table[entry->track].flow = entry->flow;
            /* Its one setup's bank-relative sample address (CONTROL bits 6..0, SAMPLE_LOW): diagnostics. */
            const uint32_t image = bytes >= 20 ? le32(dsx + offset + 16) : bytes;
            if (image <= bytes - 4)
                sfx.table[entry->track].sample = (uint32_t)(le16(dsx + offset + image) & 0x7f) << 16 | le16(dsx + offset + image + 2);
        }
    }
    free(dsx);
    for (unsigned l = 0; l < SFX_LAYERS; ++l)
        if (sfx.words && !sfx.table[l].flow) { puts("Sound effects: an engine layer has no flow: no engine"); free(sfx.words); sfx.words = NULL; break; }
    sfx.stats.flows = sfx.count;
    sfx.up = 1;
    voice_init(root);
    afx_mem_stats_t mem;
    if (!afx_mem_stats(&mem))
        printf("Sound effects: %u flows, bank %lu bytes; %lu bytes of AICA RAM free (largest %lu)\n", sfx.count,
               (unsigned long)sfx.bank.bytes, (unsigned long)mem.free_bytes, (unsigned long)mem.largest_free_block);
    return 1;
fail:
    free(dsx);
    free_flows();
    return 0;
}

static void remember(afx_instance_t instance) {
    sfx.ring[sfx.ring_next++ % SFX_RING] = instance;
}

static void start(const sfx_entry_t *entry, uint8_t gain) {
    afx_instance_t instance;
    if (afx_instance_activate(entry->flow, &instance)) { ++sfx.stats.dropped; return; }
    if (gain != 255) (void)afx_instance_gain(instance, gain);
    ++sfx.stats.started;
    remember(instance);
}

/* update_engines (native_sound_sequencer.cpp): which layers sound, at which table row. */
static void engine_words(void) {
    if (!sfx.words) return;
    const unsigned rpm = sfx.controls[0x17] & 127, slip = sfx.controls[0x19] & 15, skid = sfx.controls[0x37] & 15;
    for (unsigned l = 0; l < SFX_LAYERS; ++l) {
        unsigned row = rpm, level_row = rpm;
        if (l == 6) level_row = sfx.controls[0x34] & 127;
        else if (l == 7) level_row = sfx.controls[0x35] & 127;
        else if (l == 8) row = level_row = slip;
        else if (l == 9) row = level_row = skid;
        sfx.layer[l].want = row != 0;
        sfx.layer[l].word[WORD_PITCH] = sfx.table[l].word[WORD_PITCH][row];
        sfx.layer[l].word[WORD_MIX] = sfx.table[l].word[WORD_MIX][level_row];
        sfx.layer[l].word[WORD_DIRECT] = sfx.table[l].word[WORD_DIRECT][level_row];
    }
}

static void stop_effects(void) {
    for (unsigned i = 0; i < SFX_RING; ++i)
        if (sfx.ring[i]) { (void)afx_instance_stop(sfx.ring[i]); sfx.ring[i] = 0; }
}

/* The board's samples per layer, sent once at boot (be16..be36): the flows hold these. */
static const uint8_t layer_samples[][2] = {
    {0x16, 2}, {0x18, 4}, {0x1b, 6}, {0x1c, 3}, {0x1d, 1}, {0x1e, 2}, {0x1f, 5}, {0x36, 9},
};

#ifdef NM_SFX_VOICE_STREAM
/* The announcer's phrases streamed one at a time (audio/sfx.h, NATIVE_SFX_VOICE_STREAM): sfx/voices.pak of
 * recipes/build_sfx.sh (--voices stream). Each phrase is one contiguous range of the pack (its AFB, padding to 32
 * bytes, its AFX), read into one of VOICE_SLOTS 32-byte aligned slots of SH4 RAM, which also cache it:
 *  - from a disc (/cd) by a worker thread below the game's priority (a GD read blocks only that thread);
 *  - from dcload's /pc, whose file calls hold the whole CPU with interrupts off, by the main thread in its idle wait
 *    for the PVR (the pvr_wait_ready wrapper below): one call a frame, as many bytes as the cost model (a fixed cost
 *    per call plus one per byte, fitted to the reads so far) allows in the shortest idle wait of the last frames.
 * A request whose phrase is cached starts at once; one that is not is read first. Without a request to serve, the
 * reader prefetches the hinted phrases (nm_sfx_hint: race_sound.c's next announcements and time calls) into slots
 * that hold nothing queued or hinted, least recently used first. The updater uploads a phrase's samples with
 * AICAflow's chunked G2 DMA without waiting (each update polls the chunk in flight and starts the next), then its
 * flow, starts it and frees both in AICA RAM once its instance has ended; the slot keeps the phrase for a repeat.
 * Two requests wait at most (a third is dropped and counted). AICAflow takes one DMA at a time: sound.c calls
 * nm_sfx_quiesce before its own loads. */
#include <errno.h>
#ifdef _arch_dreamcast
#include <arch/timer.h>
#include <kos/sem.h>
#include <kos/thread.h>
static uint64_t voice_us(void) { return timer_us_gettime64(); }
#else
static uint64_t voice_fake_us; /* host checks: their clock */
static uint32_t (*voice_fake_read_cost)(uint32_t bytes); /* host checks: a read's cost on that clock */
static uint64_t voice_us(void) { return voice_fake_us; }
#endif
static int voice_force_mainloop; /* host checks: the main-loop reading without dcload */
#define VOICE_QUEUE 2u
#define VOICE_SLOTS 8u          /* phrases the arena holds at most */
#define VOICE_ARENA_LARGEST 4u  /* the arena: this many of the largest phrase, about 96 KB */
#define VOICE_HINTS 8u
#define VOICE_STACK 8192u
#define VOICE_CHUNK 65536u     /* bytes of one G2 DMA at most (a multiple of 32): a phrase's samples in one */
#define VOICE_READ_MAX 16384u  /* bytes of one main-loop read at most */
#define VOICE_READ_STEP 128u   /* main-loop reads in steps of this */
enum { VOICE_IDLE, VOICE_UPLOADING, VOICE_PLAYING, VOICE_ENDED };
enum { SLOT_EMPTY, SLOT_READING, SLOT_READY, SLOT_UPLOAD };
typedef struct { uint32_t key, bank_offset, bank_bytes, flow_offset, flow_bytes; } voice_entry_t;
/* A phrase in the arena: phrase is its directory index + 1 (0: the slot is free, no bytes). */
typedef struct { unsigned phrase; int state; uint32_t used, offset, size; } voice_slot_t;
static struct {
    int up;
    FILE *file;
    voice_entry_t *dir;
    unsigned count;
    unsigned char *arena;                  /* the phrases read, each at its slot's offset */
    uint32_t slot_bytes, arena_bytes;      /* the largest phrase (32-byte multiple); the arena */
    voice_slot_t slot[VOICE_SLOTS];
    unsigned queue[VOICE_QUEUE], queued;   /* requested directory indices (updater thread only) */
    uint32_t queued_at[VOICE_QUEUE];
    unsigned hint[VOICE_HINTS], hints;     /* directory indices worth having read */
    /* The read in progress: slot + 1 (0: none); the worker publishes its end in read_done (slot + 1, 0 failed). */
    unsigned reading;
    volatile unsigned worker_slot, worker_done, worker_ok;
    volatile int quit;
    uint32_t read_pos;                     /* main loop: bytes of the range read */
    int mainloop;                          /* /pc: chunked reads in the idle wait, no worker */
    float call_us, byte_us;                /* main loop: the fitted cost of one read call */
    double fit_n, fit_x, fit_y, fit_xx, fit_xy;
    uint32_t updates, served;
    uint64_t latency_sum;
    int stage;
    unsigned upload_slot;                  /* slot + 1 being uploaded or playing */
    afx_asset_t upload;
    uint32_t data_offset, data_size, uploaded;
    afx_bank_id_t id;
    int in_flight;
    afx_bank_t bank;
    afx_asset_t flow;
    afx_instance_t instance;
#ifdef _arch_dreamcast
    semaphore_t wake;
    kthread_t *thread;
#endif
} voice;

static uint32_t voice_align(uint32_t n) { return (n + 31u) & ~31u; }
static uint32_t voice_range(const voice_entry_t *e) { return e->flow_offset - e->bank_offset + e->flow_bytes; }
static unsigned char *voice_slot_data(unsigned slot) { return voice.arena + voice.slot[slot].offset; }
static void voice_slot_free(unsigned slot) { voice.slot[slot] = (voice_slot_t){0, SLOT_EMPTY, 0, 0, 0}; }

#ifdef _arch_dreamcast
static void *voice_worker(void *unused) {
    (void)unused;
    for (;;) {
        sem_wait(&voice.wake);
        if (voice.quit) return NULL;
        const unsigned slot = voice.worker_slot;
        if (!slot) continue;
        const voice_entry_t *e = &voice.dir[voice.slot[slot - 1u].phrase - 1u];
        const uint32_t bytes = voice_range(e);
        const uint64_t begin = voice_us();
        const int ok = !fseek(voice.file, (long)e->bank_offset, SEEK_SET) &&
                       fread(voice_slot_data(slot - 1u), 1, bytes, voice.file) == bytes;
        const uint32_t took = (uint32_t)(voice_us() - begin);
        if (took > sfx.stats.voice_read_us) sfx.stats.voice_read_us = took;
        voice.worker_ok = ok;
        voice.worker_slot = 0;
        voice.worker_done = slot;
    }
}
#endif

static void voice_close(void) {
    if (voice.file) fclose(voice.file);
    free(voice.dir);
    free(voice.arena);
    nm_memset(&voice, 0, sizeof(voice));
}

static void voice_init(const char *root) {
    char path[256];
    unsigned char head[12];
    nm_memset(&voice, 0, sizeof(voice));
    snprintf(path, sizeof(path), "%ssfx/voices.pak", root);
    if (!(voice.file = fopen(path, "rb"))) { printf("Sound effects: no %s: the announcer's phrases are silent\n", path); return; }
    const uint32_t count = fread(head, 1, 12, voice.file) == 12 && !memcmp(head, "VOX1", 4) && le32(head + 4) == 1 ? le32(head + 8) : 0;
    unsigned char *raw = count && count <= 256 ? nm_alloc(20u * count) : NULL;
    voice.dir = count && count <= 256 ? nm_calloc(count, sizeof(*voice.dir)) : NULL;
    if (!raw || !voice.dir || fread(raw, 20, count, voice.file) != count) {
        printf("Sound effects: %s is malformed: the announcer's phrases are silent\n", path);
        free(raw); voice_close(); return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *p = raw + 20u * i;
        voice.dir[i] = (voice_entry_t){le32(p), le32(p + 4), le32(p + 8), le32(p + 12), le32(p + 16)};
        const voice_entry_t *e = &voice.dir[i];
        /* The flow follows the bank at its 32-byte end, as the slot holds them (build_sfx.sh's pack). */
        if ((i && e->key <= voice.dir[i - 1].key) || e->bank_bytes < 32 || !e->flow_bytes ||
            e->flow_offset != e->bank_offset + voice_align(e->bank_bytes) || voice_range(e) > (1u << 20)) {
            printf("Sound effects: %s entry %lu is malformed: the announcer's phrases are silent\n", path, (unsigned long)i);
            free(raw); voice_close(); return;
        }
        if (voice_align(voice_range(e)) > voice.slot_bytes) voice.slot_bytes = voice_align(voice_range(e));
    }
    free(raw);
    voice.count = count;
#ifdef _arch_dreamcast
    voice.mainloop = !strncmp(root, "/pc", 3);
#else
    voice.mainloop = voice_force_mainloop;
#endif
    if (voice.mainloop) setvbuf(voice.file, NULL, _IONBF, 0); /* one read call a chunk */
    /* The fit starts from dcload's measured cost (17 October 2026: 512 bytes 2.5 ms, about 24 KB 62 ms). */
    voice.call_us = 1500.0f; voice.byte_us = 3.0f;
    voice.arena_bytes = VOICE_ARENA_LARGEST * voice.slot_bytes;
    if (!(voice.arena = nm_alloc(voice.arena_bytes))) {
        puts("Sound effects: no slots for the phrases: silent"); voice_close(); return;
    }
#ifdef _arch_dreamcast
    if (!voice.mainloop) {
        sem_init(&voice.wake, 0);
        /* Below the game's threads: the disc read runs while they wait (vertical blank, PVR). */
        const kthread_attr_t attr = {.stack_size = VOICE_STACK, .prio = PRIO_DEFAULT + 5, .label = "voices"};
        if (!(voice.thread = thd_create_ex(&attr, voice_worker, NULL))) {
            puts("Sound effects: no thread for the phrases: silent"); sem_destroy(&voice.wake); voice_close(); return;
        }
    }
#endif
    voice.up = 1;
    sfx.stats.voices = count;
    printf("Sound effects: %lu announcer phrases streamed from %s (%lu bytes of cache; read %s)\n", (unsigned long)count,
           path, (unsigned long)voice.arena_bytes, voice.mainloop ? "in the idle wait, chunked" : "by a worker thread");
}

static int voice_find(uint32_t key) {
    unsigned lo = 0, hi = voice.count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (voice.dir[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo < voice.count && voice.dir[lo].key == key ? (int)lo : -1;
}

/* A sequence start without a resident flow: a phrase of the pack is queued. 0: not one of them. */
static int voice_request(uint32_t key) {
    if (!voice.up) return 0;
    const int index = voice_find(key);
    if (index < 0) return 0;
    if (voice.queued < VOICE_QUEUE) {
        for (unsigned s = 0; s < VOICE_SLOTS; ++s)
            if (voice.slot[s].phrase == (unsigned)index + 1u && voice.slot[s].state == SLOT_READY) { ++sfx.stats.voice_hits; break; }
        voice.queued_at[voice.queued] = voice.updates; voice.queue[voice.queued++] = (unsigned)index;
    } else ++sfx.stats.voice_drops;
    return 1;
}

static int voice_cached(unsigned index, int state) {
    for (unsigned s = 0; s < VOICE_SLOTS; ++s)
        if (voice.slot[s].phrase == index + 1u && (state < 0 || voice.slot[s].state == state)) return (int)s;
    return -1;
}

static int voice_wanted(unsigned phrase) { /* queued or hinted (phrase: directory index + 1) */
    for (unsigned i = 0; i < voice.queued; ++i) if (voice.queue[i] + 1u == phrase) return 1;
    for (unsigned i = 0; i < voice.hints; ++i) if (voice.hint[i] + 1u == phrase) return 2;
    return 0;
}

/* Room for phrase index in the arena (first fit), freeing the least recently used phrases that are only read
 * (not queued and, for a prefetch, not hinted either) until it fits. The slot index, its bytes reserved; -1: none. */
static int voice_place(unsigned index, int for_request) {
    const uint32_t need = voice_align(voice_range(&voice.dir[index]));
    for (;;) {
        int free_slot = -1;
        for (unsigned s = 0; s < VOICE_SLOTS && free_slot < 0; ++s) if (!voice.slot[s].phrase) free_slot = (int)s;
        if (free_slot >= 0) {
            /* The lowest offset with need bytes before the next phrase (or the arena's end). */
            uint32_t at = 0;
            for (;;) {
                uint32_t next = voice.arena_bytes;
                int clash = 0;
                for (unsigned s = 0; s < VOICE_SLOTS; ++s) {
                    const voice_slot_t *o = &voice.slot[s];
                    if (!o->phrase) continue;
                    if (o->offset < at + need && at < o->offset + o->size) { at = o->offset + o->size; clash = 1; break; }
                    if (o->offset >= at && o->offset < next) next = o->offset;
                }
                if (clash) continue;
                if (at + need <= next && at + need <= voice.arena_bytes) {
                    voice.slot[free_slot] = (voice_slot_t){index + 1u, SLOT_EMPTY, voice.updates, at, need};
                    return free_slot;
                }
                if (next >= voice.arena_bytes) break;
                at = next; /* past the phrase at next */
            }
        }
        int best = -1;
        for (unsigned s = 0; s < VOICE_SLOTS; ++s) {
            const voice_slot_t *o = &voice.slot[s];
            if (!o->phrase || o->state != SLOT_READY) continue;
            const int wanted = voice_wanted(o->phrase);
            if (wanted == 1 || (wanted == 2 && !for_request)) continue;
            if (best < 0 || o->used < voice.slot[best].used) best = (int)s;
        }
        if (best < 0) return -1;
        voice_slot_free((unsigned)best);
    }
}

static void voice_read_start(unsigned index, int slot) {
    voice.slot[slot].phrase = index + 1u; voice.slot[slot].state = SLOT_READING; voice.slot[slot].used = voice.updates;
    voice.reading = (unsigned)slot + 1u;
    voice.read_pos = 0;
    if (voice.mainloop) return; /* The idle wait reads it. */
#ifdef _arch_dreamcast
    voice.worker_slot = (unsigned)slot + 1u;
    sem_signal(&voice.wake);
#else
    const voice_entry_t *e = &voice.dir[index];
    const int ok = !fseek(voice.file, (long)e->bank_offset, SEEK_SET) &&
                   fread(voice_slot_data((unsigned)slot), 1, voice_range(e), voice.file) == voice_range(e);
    voice.worker_ok = ok; voice.worker_done = (unsigned)slot + 1u;
#endif
}

static void voice_read_end(int ok) {
    voice_slot_t *slot = &voice.slot[voice.reading - 1u];
    if (ok) { slot->state = SLOT_READY; ++sfx.stats.voice_reads; }
    else { voice_slot_free(voice.reading - 1u); ++sfx.stats.voice_read_failures; }
    voice.reading = 0;
}

/* The main-loop read (/pc): one call of at most what budget_us allows by the fitted cost, in whole steps (all
 * of the rest of the range when it fits). Called in the idle wait only (the pvr_wait_ready wrapper; host checks
 * call it directly). */
static void voice_idle_read(uint32_t budget_us) {
    if (!voice.up || !voice.mainloop || !voice.reading) return;
    const unsigned slot = voice.reading - 1u;
    const voice_entry_t *e = &voice.dir[voice.slot[slot].phrase - 1u];
    const uint32_t left = voice_range(e) - voice.read_pos;
    const float fit = (0.9f * (float)budget_us - voice.call_us) / voice.byte_us; /* a tenth kept for the fit's error */
    if (fit < (float)VOICE_READ_STEP && fit < (float)left) return; /* Not even one step in this wait. */
    uint32_t bytes = fit >= (float)left ? left : fit >= (float)VOICE_READ_MAX ? VOICE_READ_MAX
                   : (uint32_t)fit / VOICE_READ_STEP * VOICE_READ_STEP;
    if (bytes > left) bytes = left;
    const uint64_t begin = voice_us();
    const int ok = !fseek(voice.file, (long)(e->bank_offset + voice.read_pos), SEEK_SET) &&
                   fread(voice_slot_data(slot) + voice.read_pos, 1, bytes, voice.file) == bytes;
#ifndef _arch_dreamcast
    if (voice_fake_read_cost) voice_fake_us += voice_fake_read_cost(bytes);
#endif
    const uint32_t took = (uint32_t)(voice_us() - begin);
    if (took > sfx.stats.voice_chunk_us) { sfx.stats.voice_chunk_us = took; sfx.stats.voice_chunk_bytes = bytes; }
    if (took > budget_us) ++sfx.stats.voice_overruns;
    /* Least squares of took = call + bytes * byte over the reads so far, older ones fading (factor 0.97). */
    const double x = bytes, y = took;
    voice.fit_n = 0.97 * voice.fit_n + 1.0; voice.fit_x = 0.97 * voice.fit_x + x; voice.fit_y = 0.97 * voice.fit_y + y;
    voice.fit_xx = 0.97 * voice.fit_xx + x * x; voice.fit_xy = 0.97 * voice.fit_xy + x * y;
    const double var = voice.fit_n * voice.fit_xx - voice.fit_x * voice.fit_x;
    if (voice.fit_n >= 3.0 && var > 1e3 * voice.fit_n * voice.fit_n) {
        double b = (voice.fit_n * voice.fit_xy - voice.fit_x * voice.fit_y) / var;
        double a = (voice.fit_y - b * voice.fit_x) / voice.fit_n;
        if (b < 0.05) b = 0.05;
        if (a < 0.0) a = 0.0;
        voice.byte_us = (float)b; voice.call_us = (float)a;
    } else if (voice.fit_n < 3.0) {
        /* Until the fit has spread: the per-byte cost from this read, the call cost kept. */
        const float b = ((float)took - voice.call_us) / (float)bytes;
        if (b > voice.byte_us) voice.byte_us = b;
    }
    sfx.stats.voice_call_us = (uint32_t)voice.call_us;
    sfx.stats.voice_bytes_per_ms = (uint32_t)(1000.0f / voice.byte_us);
    if (!ok) { voice_read_end(0); return; }
    voice.read_pos += bytes;
    if (voice.read_pos == voice_range(e)) voice_read_end(1);
}

/* The phrases' share of enDjinn's idle wait for the PVR (code/audio/idle_wait.c, the pvr_wait_ready wrapper, which
 * sizes budget_us by the shortest recent wait): one /pc read. Returns the microseconds it took. */
static uint32_t voice_idle(uint32_t budget_us) {
    if (!voice.up || !voice.mainloop || !voice.reading) return 0;
    const uint64_t begin = voice_us();
    voice_idle_read(budget_us);
    return (uint32_t)(voice_us() - begin);
}

/* The chunk in flight, if its DMA has completed: AICAflow releases its channel on this poll. */
static int voice_poll(void) {
    if (!voice.in_flight) return 1;
    bool complete = false;
    if (afx_sample_bank_stream_dma_poll(voice.upload, &complete)) { voice.in_flight = 0; return -1; }
    if (!complete) return 0;
    voice.in_flight = 0;
    voice.uploaded += voice.data_size - voice.uploaded < VOICE_CHUNK ? voice.data_size - voice.uploaded : VOICE_CHUNK;
    return 1;
}

static void voice_unlock(void) {
    if (voice.upload_slot && voice.slot[voice.upload_slot - 1u].state == SLOT_UPLOAD)
        voice.slot[voice.upload_slot - 1u].state = SLOT_READY;
    voice.upload_slot = 0;
}

/* A refused or failed upload: its allocation freed, the phrase counted as dropped. */
static void voice_abandon(void) {
    if (voice.upload) (void)afx_asset_free(voice.upload);
    voice.upload = AFX_ASSET_INVALID;
    voice.in_flight = 0;
    voice.stage = VOICE_IDLE;
    voice_unlock();
    ++sfx.stats.voice_drops;
}

static void voice_upload_step(void) {
    const int polled = voice_poll();
    if (polled < 0) { voice_abandon(); return; }
    if (!polled) return; /* Still on the G2 bus: next update. */
    const unsigned char *data = voice_slot_data(voice.upload_slot - 1u);
    if (voice.uploaded < voice.data_size) {
        const uint32_t left = voice.data_size - voice.uploaded, bytes = left < VOICE_CHUNK ? left : VOICE_CHUNK;
        const int result = afx_sample_bank_stream_dma_begin(voice.upload, voice.uploaded, data + voice.data_offset + voice.uploaded, bytes);
        if (!result) voice.in_flight = 1;
        else if (result != -AFX_BUSY) voice_abandon(); /* BUSY: another DMA on the channel; next update. */
        return;
    }
    if (afx_sample_bank_stream_finish(voice.upload)) { voice_abandon(); return; }
    const voice_entry_t *e = &voice.dir[voice.slot[voice.upload_slot - 1u].phrase - 1u];
    voice.bank = (afx_bank_t){voice.upload, voice.id, voice.data_size};
    voice.upload = AFX_ASSET_INVALID;
    const int flow = afx_bank_flow_upload(&voice.bank, data + voice_align(e->bank_bytes), e->flow_bytes, &voice.flow);
    voice_unlock(); /* The slot keeps the phrase for a repeat. */
    if (flow) { voice.flow = AFX_ASSET_INVALID; voice.stage = VOICE_ENDED; ++sfx.stats.voice_drops; return; }
    if (afx_instance_activate(voice.flow, &voice.instance)) { voice.stage = VOICE_ENDED; ++sfx.stats.voice_drops; return; }
    voice.stage = VOICE_PLAYING;
    ++sfx.stats.voice_starts;
}

/* The queue's first phrase, read into slot: its AFB header (as AICAflow's decode), then the samples' allocation;
 * the DMA starts next update. */
static void voice_upload_begin(unsigned slot) {
    const voice_entry_t *e = &voice.dir[voice.slot[slot].phrase - 1u];
    const unsigned char *h = voice_slot_data(slot);
    const uint32_t offset = le32(h + 16), size = le32(h + 20);
    if (le32(h) != AFX_BANK_MAGIC || le32(h + 24) != e->bank_bytes || offset < 32 || (offset & 31u) || !size ||
        size != e->bank_bytes - offset) {
        voice_slot_free(slot); ++sfx.stats.voice_drops; return;
    }
    const int result = afx_sample_bank_stream_begin(size, &voice.upload);
    if (result == -AFX_BUSY) return;
    /* Out of the queue now, served or refused. */
    const uint32_t latency = voice.updates - voice.queued_at[0];
    for (unsigned i = 1; i < voice.queued; ++i) { voice.queue[i - 1] = voice.queue[i]; voice.queued_at[i - 1] = voice.queued_at[i]; }
    --voice.queued;
    if (result) { voice.upload = AFX_ASSET_INVALID; ++sfx.stats.voice_drops; return; }
    ++voice.served;
    voice.latency_sum += latency;
    if (latency > sfx.stats.voice_latency_max) sfx.stats.voice_latency_max = latency;
    sfx.stats.voice_latency_mean = (uint32_t)(voice.latency_sum / voice.served);
    voice.slot[slot].state = SLOT_UPLOAD; voice.slot[slot].used = voice.updates;
    voice.upload_slot = slot + 1u;
    voice.id = (afx_bank_id_t){le32(h + 8), le32(h + 12)};
    voice.data_offset = offset; voice.data_size = size; voice.uploaded = 0; voice.in_flight = 0;
    voice.stage = VOICE_UPLOADING;
}

static void voice_update(void) {
    if (!voice.up) return;
    const uint64_t begin = voice_us();
    ++voice.updates;
    if (!voice.mainloop && voice.reading && voice.worker_done) { /* the worker's read ended */
        voice.worker_done = 0;
        voice_read_end(voice.worker_ok);
    }
    if (voice.stage == VOICE_PLAYING) {
        afx_instance_status_t status;
        if (afx_instance_status(voice.instance, &status) == -AFX_STALE_GENERATION) voice.stage = VOICE_ENDED;
    }
    if (voice.stage == VOICE_ENDED) {
        /* AICAflow frees an ended instance from its event thread; the flow refuses until then. */
        if (!voice.flow || !afx_asset_free(voice.flow)) {
            voice.flow = AFX_ASSET_INVALID;
            afx_bank_release(&voice.bank);
            voice.bank = (afx_bank_t){0};
            voice.stage = VOICE_IDLE;
        }
    }
    if (voice.stage == VOICE_UPLOADING) voice_upload_step();
    if (voice.queued) {
        const int slot = voice_cached(voice.queue[0], SLOT_READY);
        if (slot >= 0 && voice.stage == VOICE_IDLE) voice_upload_begin((unsigned)slot);
        else if (slot < 0 && voice_cached(voice.queue[0], SLOT_READING) < 0) {
            if (voice.reading && voice.mainloop && voice_wanted(voice.slot[voice.reading - 1u].phrase) != 1) {
                /* A prefetch in the idle wait gives way to the request (a worker's read cannot be stopped). */
                voice_slot_free(voice.reading - 1u);
                voice.reading = 0;
                ++sfx.stats.voice_prefetch_cancels;
            }
            const int place = voice.reading ? -1 : voice_place(voice.queue[0], 1);
            if (place >= 0) voice_read_start(voice.queue[0], place);
        }
    }
    if (!voice.reading) { /* nothing requested to read: the first hinted phrase not yet read */
        for (unsigned i = 0; i < voice.hints; ++i) {
            if (voice_cached(voice.hint[i], -1) >= 0) continue;
            const int place = voice_place(voice.hint[i], 0);
            if (place >= 0) { voice_read_start(voice.hint[i], place); ++sfx.stats.voice_prefetches; break; }
            /* No room without evicting another hinted phrase: try the next hint (a smaller one may fit). */
        }
    }
    const uint32_t took = (uint32_t)(voice_us() - begin);
    if (took > sfx.stats.voice_upload_us) sfx.stats.voice_upload_us = took;
}

/* Hints: the phrases a race is likely to request next, most likely first (race_sound.c, once a tick). */
static void voice_hint(const uint32_t *keys, unsigned n) {
    if (!voice.up) return;
    voice.hints = 0;
    for (unsigned i = 0; i < n && voice.hints < VOICE_HINTS; ++i) {
        const int index = voice_find(keys[i]);
        if (index >= 0) voice.hint[voice.hints++] = (unsigned)index;
    }
}

/* Completes the chunk in flight (at most VOICE_CHUNK bytes on the G2 bus): AICAflow's own loads would spin on
 * its channel otherwise. The phrase continues with its next chunk on the next update. */
static void voice_quiesce(void) {
    while (voice.up && voice.in_flight && !voice_poll()) {
#ifdef _arch_dreamcast
        thd_pass();
#endif
    }
}

static void voice_shutdown(void (*wait_ms)(unsigned)) {
    if (!voice.up) return;
    voice.queued = 0; voice.hints = 0;
    if (voice.mainloop && voice.reading) voice_read_end(0), --sfx.stats.voice_read_failures;
    for (unsigned i = 0; i < 1000 && voice.reading; ++i) { if (wait_ms) wait_ms(1); voice_update(); }
    voice_quiesce();
    if (voice.stage == VOICE_UPLOADING) { voice_abandon(); --sfx.stats.voice_drops; }
    if (voice.stage == VOICE_PLAYING) { (void)afx_instance_stop(voice.instance); voice.stage = VOICE_ENDED; }
    for (unsigned i = 0; i < 1000 && voice.stage != VOICE_IDLE; ++i) {
        voice_update();
        if (voice.stage != VOICE_IDLE && wait_ms) wait_ms(1);
    }
#ifdef _arch_dreamcast
    if (voice.thread) {
        voice.quit = 1;
        sem_signal(&voice.wake);
        thd_join(voice.thread, NULL);
        sem_destroy(&voice.wake);
    }
#endif
    voice_close();
}
#else
static void voice_init(const char *root) { (void)root; }
static int voice_request(uint32_t key) { (void)key; return 0; }
static void voice_update(void) {}
static void voice_quiesce(void) {}
static void voice_hint(const uint32_t *keys, unsigned n) { (void)keys; (void)n; }
static void voice_shutdown(void (*wait_ms)(unsigned)) { (void)wait_ms; }
static uint32_t voice_idle(uint32_t budget_us) { (void)budget_us; return 0; }
#endif

void nm_sfx_quiesce(void) { voice_quiesce(); }
void nm_sfx_hint(const uint32_t *keys, unsigned n) { voice_hint(keys, n); }
uint32_t nm_sfx_idle(uint32_t budget_us) { return voice_idle(budget_us); }

void nm_sfx_command(uint32_t command) {
    if (!sfx.up) return;
    const uint8_t status = (uint8_t)(command >> 16), a = (uint8_t)(command >> 8), b = (uint8_t)command;
    if ((status & 0xf0) == 0xa0 && a >= 0x20 && a <= 0x22) {
        const sfx_entry_t *entry = find(command);
        if (entry && entry->kind == SFX_SEQUENCE) start(entry, 255);
        else if (!voice_request(command)) ++sfx.stats.missing;
    } else if ((status & 0xf0) == 0x90 && b) {
        const sfx_entry_t *entry = find(command & UINT32_C(0xffff00));
        if (entry && entry->kind == SFX_NOTE) start(entry, sfx.gains[b & 127]); else ++sfx.stats.missing;
    } else if (status == 0xbe && a == 0x50) {
        if (b == 0x7e) return; /* The music only. */
        stop_effects();
        if (b != 0x7d) /* The engine layers too, until the next engine controller. */
            for (unsigned l = 0; l < SFX_LAYERS; ++l) sfx.layer[l].want = 0;
    } else if (status == 0xbe && ((a >= 0x16 && a <= 0x1f && a != 0x1a) || (a >= 0x34 && a <= 0x37))) {
        sfx.controls[a] = b & 127;
        for (unsigned i = 0; i < sizeof(layer_samples) / sizeof(layer_samples[0]); ++i)
            if (layer_samples[i][0] == a && layer_samples[i][1] != (b & 127) && sfx.sample_warnings++ < 4)
                printf("Sound effects: engine sample be%02x%02x is not the boot one (%02x): kept\n", a, b, layer_samples[i][1]);
        engine_words();
    }
}

/* Diagnostics of the engine layers on the console (a few lines a race): each layer's flow ending or a command
 * of it the ARM7 refused, once a layer; and, two seconds after the layers come on and when they go off, their
 * states with the AICA's own view of their voices (key on, level, direct send, pitch, envelope). */
#ifdef _arch_dreamcast
#include <dc/g2bus.h>
#define SFX_AICA_REG 0xa0700000u
/* The first channel playing the layer's sample that no earlier layer of this report took (layers 0/5, 3/4 and
 * 6/7 share a sample, each on its own voice). */
static int engine_voice(unsigned l, uint64_t *taken, char *out, size_t size) {
    if (!sfx.bank.asset) return 0;
    const uint32_t want = afx_asset_addr(sfx.bank.asset) + sfx.table[l].sample;
    for (unsigned c = 0; c < 64; ++c) {
        const uint32_t at = SFX_AICA_REG + c * 0x80u, control = g2_read_32(at) & 0xffffu;
        if ((*taken >> c & 1u) || ((control & 0x7fu) << 16 | (g2_read_32(at + 4) & 0xffffu)) != want) continue;
        *taken |= UINT64_C(1) << c;
        g2_write_32(SFX_AICA_REG + 0x280cu, c << 8); /* MSLC: the envelope monitor on this channel */
        (void)g2_read_32(SFX_AICA_REG + 0x2810u);   /* the monitor follows a sample later: read twice */
        const uint32_t eg = g2_read_32(SFX_AICA_REG + 0x2810u) & 0xffffu;
        return snprintf(out, size, " c%u%s t%02lx d%lx p%04lx e%lu/%04lx", c, control & 0x4000u ? "" : " off",
                        (unsigned long)(g2_read_32(at + 0x28) >> 8 & 0xffu), (unsigned long)(g2_read_32(at + 0x24) >> 8 & 15u),
                        (unsigned long)(g2_read_32(at + 0x18) & 0xffffu), (unsigned long)(eg >> 13 & 3u), (unsigned long)(eg & 0x1fffu));
    }
    return snprintf(out, size, " no voice");
}
#else
static int engine_voice(unsigned l, uint64_t *taken, char *out, size_t size) { (void)l; (void)taken; (void)out; (void)size; return 0; }
#endif

static void engine_report(const char *when) {
    char line[1024];
    uint64_t taken = 0;
    int n = snprintf(line, sizeof(line), "Sound effects: engine %s (rpm %u slip %u skid %u; starts %u patches %u put off %u):",
                     when, sfx.controls[0x17], sfx.controls[0x19], sfx.controls[0x37], sfx.stats.layer_starts,
                     sfx.stats.patches, sfx.stats.retries);
    for (unsigned l = 0; l < SFX_LAYERS && n > 0 && (size_t)n < sizeof(line); ++l) {
        const sfx_layer_t *layer = &sfx.layer[l];
        afx_instance_status_t status;
        if (!layer->instance || afx_instance_status(layer->instance, &status)) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " | L%u -", l);
            continue;
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " | L%u s%lu r%lu %04x/%04x/%04x", l, (unsigned long)status.state,
                      (unsigned long)status.result, layer->sent[WORD_PITCH], layer->sent[WORD_MIX], layer->sent[WORD_DIRECT]);
        if ((size_t)n < sizeof(line)) n += engine_voice(l, &taken, line + n, sizeof(line) - (size_t)n);
    }
    puts(line);
}

static void engine_watch(unsigned l) {
    sfx_layer_t *layer = &sfx.layer[l];
    afx_instance_status_t status;
    if (layer->reported || !layer->instance || afx_instance_status(layer->instance, &status)) return;
    if (status.state == AFX_DONE || status.state == AFX_ERROR || status.result) {
        layer->reported = 1;
        printf("Sound effects: engine layer %u: state %lu result %lu detail %lu (position %lu)\n", l, (unsigned long)status.state,
               (unsigned long)status.result, (unsigned long)status.detail, (unsigned long)status.position);
    }
}

void nm_sfx_update(void) {
    if (!sfx.up) return;
    voice_update();
    if (!sfx.words) return;
    static const uint32_t field[WORDS] = {AFX_FIELD_PITCH, AFX_FIELD_DIRECT, AFX_FIELD_MIX}; /* ascending */
    unsigned wanted = 0;
    for (unsigned l = 0; l < SFX_LAYERS; ++l) wanted += sfx.layer[l].want;
    if (!wanted && sfx.engine_updates) { engine_report("off"); sfx.engine_updates = 0; }
    /* Once keyed on, a layer stays keyed until shutdown: off is its level and direct send at zero (TL 255, DISDL 0),
     * on again its words; as the scripted races, which never stop the engine, and no new key-on for each race. */
    for (unsigned l = 0; l < SFX_LAYERS; ++l) {
        sfx_layer_t *layer = &sfx.layer[l];
        if (!layer->want && !layer->instance) continue;
        if (!layer->instance) {
            if (afx_instance_activate(sfx.table[l].flow, &layer->instance)) { layer->instance = 0; ++sfx.stats.dropped; continue; }
            layer->sent_valid = layer->parked = 0;
            ++sfx.stats.layer_starts;
        }
        engine_watch(l);
        /* The ARM7 takes queued commands before it runs a new flow's events, so a patch right after the
         * activation lands before the flow's NOTE, which then installs its setup over it (DIRECT 0). Words go
         * once the flow has parked, all of them the first time. */
        if (!layer->parked) {
            afx_instance_status_t status;
            const int result = afx_instance_status(layer->instance, &status);
            if (result == -AFX_STALE_GENERATION) { layer->instance = 0; continue; }
            if (result || status.state != AFX_PARKED) continue;
            layer->parked = 1; layer->sent_valid = 0;
        }
        uint16_t word[WORDS];
        nm_memcpy(word, layer->word, sizeof(word));
        if (!layer->want) {
            word[WORD_PITCH] = layer->sent_valid ? layer->sent[WORD_PITCH] : word[WORD_PITCH];
            word[WORD_DIRECT] = 0;
            word[WORD_MIX] = (uint16_t)(0xff00u | (word[WORD_MIX] & 0xffu));
        }
        uint32_t mask = 0;
        uint16_t values[WORDS];
        unsigned n = 0;
        for (unsigned w = 0; w < WORDS; ++w)
            if (!layer->sent_valid || word[w] != layer->sent[w]) { mask |= 1u << field[w]; values[n++] = word[w]; }
        if (!mask) continue;
        const int result = afx_instance_patch(layer->instance, 0, mask, values);
        if (!result) {
            nm_memcpy(layer->sent, word, sizeof(layer->sent)); layer->sent_valid = 1; ++sfx.stats.patches;
        } else if (result == -AFX_STALE_GENERATION) layer->instance = 0; /* Ended: started again while wanted. */
        else ++sfx.stats.retries;
    }
    if (wanted && sfx.engine_updates++ == 120) engine_report("on");
}

void nm_sfx_shutdown(void (*wait_ms)(unsigned)) {
    if (!sfx.up) return;
    voice_shutdown(wait_ms);
    for (unsigned l = 0; l < SFX_LAYERS; ++l) sfx.layer[l].want = 0;
    stop_effects();
    /* Every instance that may still run: the engine layers (stopped below) and the ring's. */
    afx_instance_t pending[SFX_LAYERS];
    for (unsigned l = 0; l < SFX_LAYERS; ++l) pending[l] = sfx.layer[l].instance;
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned live = 0;
        afx_instance_status_t status;
        for (unsigned l = 0; l < SFX_LAYERS; ++l) {
            if (sfx.layer[l].instance) {
                const int result = afx_instance_stop(sfx.layer[l].instance);
                if (!result || result == -AFX_STALE_GENERATION) sfx.layer[l].instance = 0;
            }
            if (pending[l] && afx_instance_status(pending[l], &status) == -AFX_STALE_GENERATION) pending[l] = 0;
            live += pending[l] != 0 || sfx.layer[l].instance != 0;
        }
        /* One-shots end by themselves; a stopped one goes stale. Flows still referenced refuse to be freed. */
        int busy = 0;
        for (unsigned k = 0; k < sfx.count && !live; ++k)
            if (sfx.entries[k].flow) {
                if (afx_asset_free(sfx.entries[k].flow)) busy = 1; else sfx.entries[k].flow = AFX_ASSET_INVALID;
            }
        if (!live && !busy) break;
        if (wait_ms) wait_ms(1);
    }
    free_flows();
    printf("Sound effects: shut down (started %u, without a flow %u, refused %u, engine layer starts %u, engine patches %u, put off %u)\n",
           sfx.stats.started, sfx.stats.missing, sfx.stats.dropped, sfx.stats.layer_starts, sfx.stats.patches, sfx.stats.retries);
#ifdef NM_SFX_VOICE_STREAM
    printf("Sound effects: phrases streamed %u, dropped %u, read failures %u; longest read %lu us, longest voices update %lu us\n",
           sfx.stats.voice_starts, sfx.stats.voice_drops, sfx.stats.voice_read_failures,
           (unsigned long)sfx.stats.voice_read_us, (unsigned long)sfx.stats.voice_upload_us);
    printf("Sound effects: phrase reads %u (prefetched %u, cancelled %u), requests found read %u; start latency %lu updates at "
           "most, %lu on average\n", sfx.stats.voice_reads, sfx.stats.voice_prefetches, sfx.stats.voice_prefetch_cancels,
           sfx.stats.voice_hits, (unsigned long)sfx.stats.voice_latency_max, (unsigned long)sfx.stats.voice_latency_mean);
    printf("Sound effects: idle-wait reads: longest call %lu us (%lu bytes), over budget %u; fitted cost %lu us a call + "
           "%lu bytes/ms\n", (unsigned long)sfx.stats.voice_chunk_us, (unsigned long)sfx.stats.voice_chunk_bytes,
           sfx.stats.voice_overruns, (unsigned long)sfx.stats.voice_call_us, (unsigned long)sfx.stats.voice_bytes_per_ms);
#endif
    sfx.up = 0;
}

const nm_sfx_stats_t *nm_sfx_stats(void) { return &sfx.stats; }
