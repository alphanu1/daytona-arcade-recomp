/* Sound on the AICA through AICAflow: see include/audio/sound.h. */
#include "audio/sound.h"
#include "audio/music_map.h"
#include "audio/sfx.h"
#include <aicaflow/bank.h>
#include <aicaflow/host.h>
#include <aicaflow/protocol.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nm_mem.h"

#ifdef NM_SOUND_HOST_TEST
/* tests/test_sound.c supplies AICAflow, the firmware bytes and the clock. */
extern const unsigned char nm_sound_test_firmware[];
extern const unsigned nm_sound_test_firmware_size;
uint64_t nm_sound_test_now_us(void);
void nm_sound_test_wait_ms(unsigned ms);
size_t nm_sound_test_read(void *to, size_t bytes, FILE *file);
#define FIRMWARE nm_sound_test_firmware
#define FIRMWARE_SIZE nm_sound_test_firmware_size
static uint64_t now_us(void) { return nm_sound_test_now_us(); }
static void wait_ms(unsigned ms) { nm_sound_test_wait_ms(ms); }
static size_t file_read(void *to, size_t bytes, FILE *file) { return nm_sound_test_read(to, bytes, file); }
static void *aligned_buffer(size_t bytes) { return nm_alloc(bytes); } /* NM_ALLOC_ALIGN is AFX_UPLOAD_ALIGN (32) */
#else
#include <kos.h>
/* AICAflow's ARM7 firmware, from the same checkout as the linked library
 * (local.cfg.mk: --embed-dir). */
alignas(32) static const unsigned char firmware[] = {
#embed "aicaflow.drv"
};
#define FIRMWARE firmware
#define FIRMWARE_SIZE sizeof(firmware)
#include <malloc.h>
static uint64_t now_us(void) { return timer_us_gettime64(); }
static void wait_ms(unsigned ms) { thd_sleep((int)ms); }
static size_t file_read(void *to, size_t bytes, FILE *file) { return fread(to, 1, bytes, file); }
static void *aligned_buffer(size_t bytes) { return nm_alloc(bytes); } /* NM_ALLOC_ALIGN is AFX_UPLOAD_ALIGN (32) */
/* KOS's own sound driver never starts: enDjinn's startup calls KOS's sound
 * initialisation, which would load KOS's AICA firmware (local.cfg.mk: --wrap). */
int __wrap_snd_init(void) { return 0; }
void __wrap_snd_shutdown(void) {}
#endif

#if defined(NATIVE_DISC) && !defined(NM_SOUND_HOST_TEST)
#define NM_SOUND_IO_WORKER 1 /* the music's file calls on a worker thread (below, "From a disc") */
#endif

/* PLAYING: the instance runs. ENDING: a stop is queued and AICAflow has not freed
 * the instance yet. LOADING: a recording is on its way into AICA RAM, a share a frame. AICAflow (firmware ABI 8) takes the ARM7's status reports by
 * interrupt on its own event thread and recycles an instance that ended by itself:
 * the program never polls, and a frame that only plays asks AICAflow nothing. */
enum { SOUND_OFF, SOUND_IDLE, SOUND_PLAYING, SOUND_ENDING, SOUND_LOADING };
/* The original fade (be1a<interval>): every <interval> ticks of the 868 Hz
 * sequencer clock the music volume drops by 4; below 4 the music stops. From
 * full volume (127) that is 32 steps. Here: a linear gain ramp of that length. */
#define FADE_STEPS 32u
#define SEQUENCER_HZ 868u

static struct {
    char root[192];
    afx_bank_t bank;
    afx_asset_t flow;
    afx_instance_t instance;
    int state;
    uint8_t track, loops;   /* resident recording (0: none) and whether it repeats */
    int wanted;             /* -1: no change; 0: stop; else the recording to play */
    uint8_t wanted_loops;
    uint64_t fade_start_us, fade_us;
    unsigned ignored, unmapped;
    int settling;           /* nm_sound_settle: a load runs without limits */
    int hold;               /* nm_sound_hold: a wanted recording loads but does not start */
} sound = {.state = SOUND_OFF, .wanted = -1};

/* Instances that ended, as AICAflow's end callback reports them from its event
 * thread once it has freed them. The callback only queues the report (it must
 * not wait for anything that waits for an instance to end); nm_sound_update
 * takes it on the updater thread. One producer, one consumer: no lock. */
#define ENDED_CAPACITY 8u
static struct { afx_instance_t instance; uint32_t state, result; } ended[ENDED_CAPACITY];
static uint32_t ended_head, ended_tail; /* written by the event thread / the updater */

static void on_instance_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    (void)user;
    const uint32_t head = ended_head;
    if (head - __atomic_load_n(&ended_tail, __ATOMIC_ACQUIRE) >= ENDED_CAPACITY)
        return; /* Full: the handle is stale anyway, which ENDING notices. */
    ended[head % ENDED_CAPACITY].instance = instance;
    ended[head % ENDED_CAPACITY].state = status->state;
    ended[head % ENDED_CAPACITY].result = status->result;
    __atomic_store_n(&ended_head, head + 1u, __ATOMIC_RELEASE);
}

/* Loading a recording without a long frame. The file reads happen in enDjinn's idle wait for the PVR
 * (code/audio/idle_wait.c, the pvr_wait_ready wrapper: nm_sound_idle with what the shortest recent wait leaves
 * after the announcer's phrases), never in an update: dcload's file calls hold the whole CPU with interrupts
 * off (on hardware a slow /pc host made such reads in the updater late frames). One step a wait: an open, or a
 * read sized by a fitted cost (a fixed cost a call plus one a byte, least squares over the reads so far, as the
 * phrases' reader) into a staging buffer, whose G2 DMA then runs while the frame goes on and is collected at the
 * next update, before the effects module (AICAflow takes one upload at a time). Then the AFX in reads of the same
 * kind, and its upload (one PIO call) in a wait with time for it; a flow too large for any (the race pieces) waits
 * for a loading pause. nm_sound_settle lifts the limits: there the load runs at once. */
enum { LOAD_BANK_OPEN, LOAD_BANK_HEADER, LOAD_ROOM, LOAD_BANK, LOAD_FLOW_OPEN, LOAD_FLOW_HEADER, LOAD_FLOW, LOAD_FLOW_UPLOAD
#ifdef NM_SOUND_IO_WORKER
     , LOAD_BANK_SEEK /* after an odd header: the seek is a worker call of its own */
#endif
};
#define LOAD_CHUNK_MAX 8192u  /* bytes: the staging buffer, and the largest read (a step at a pause: ~26 ms over /pc) */
#define LOAD_READ_STEP 512u   /* reads in whole steps (all of the rest when it fits) */
/* The flow's upload is one synchronous AICAflow call (a PIO copy: 254,681 bytes took 42.6 ms on hardware). */
#define LOAD_UPLOAD_BYTES_PER_MS 5000u
static struct {
    FILE *file;
    unsigned char *buffer;  /* LOAD_CHUNK_MAX + AFX_UPLOAD_ALIGN, aligned to AFX_UPLOAD_ALIGN */
    unsigned char *flow;
    afx_asset_t asset;      /* the bank while it streams */
    afx_bank_id_t id;
    uint32_t done, size;    /* bytes of the payload (flow) taken, and its size */
    uint32_t dma;           /* bytes of the DMA in flight */
    uint32_t budget;        /* this wait's time, us */
    float call_us, byte_us; /* the fitted read cost (kept across loads) */
    double fit_n, fit_x, fit_y, fit_xx, fit_xy;
    uint64_t start_us;
    uint32_t updates, waits, step_max_us, pause_max_us, upload_us, reads, overruns;
    uint8_t track, step, active, ready;
    uint8_t adpcm;          /* from music/adpcm/ (LOAD_ROOM) */
#ifdef NM_SOUND_IO_WORKER
    uint8_t room_opened;    /* LOAD_ROOM: the flow's open is done (room_file, NULL: no flow file) */
    FILE *room_file;
    uint32_t seek_to;       /* LOAD_BANK_SEEK */
#endif
} load = {.call_us = 1200.0f, .byte_us = 3.0f};

/* The loads of the run, printed by nm_sound_shutdown. */
#define LOAD_REPORTS 16u
typedef struct { uint32_t track, ms, updates, waits, bank, flow, step_max_us, pause_max_us, upload_us, reads, overruns, call_us, bytes_per_ms; } load_report_t;
static load_report_t report[LOAD_REPORTS];
static unsigned loads;

static void unload(void) {
    if (sound.flow) {
        const int result = afx_asset_free(sound.flow);
        if (result) printf("Sound: freeing track %02u's flow failed (%d)\n", sound.track, result);
        sound.flow = AFX_ASSET_INVALID;
    }
    if (sound.bank.asset) {
        const int result = afx_bank_release(&sound.bank);
        if (result) printf("Sound: releasing track %02u's bank failed (%d)\n", sound.track, result);
    }
    sound.bank = (afx_bank_t){0};
    sound.track = sound.loops = 0;
}

/* The staging buffer exists only while a load runs: the heap of a running scene has no room to spare. */
#ifdef NM_SOUND_IO_WORKER
static void io_drain(void);
#endif
static void load_close(void) {
#ifdef NM_SOUND_IO_WORKER
    io_drain(); /* nothing of the worker's touches the buffers below any more */
    if (load.room_file) fclose(load.room_file);
    load.room_file = NULL;
    load.room_opened = 0;
#endif
    if (load.file) fclose(load.file);
    load.file = NULL;
    free(load.flow);
    load.flow = NULL;
    free(load.buffer);
    load.buffer = NULL;
}

/* Drops a load in any step: nothing of it stays resident. */
static void load_abort(void) {
    if (!load.active) return;
    if (load.asset) (void)afx_asset_free(load.asset); /* waits for its DMA */
    load.asset = AFX_ASSET_INVALID;
    load.dma = 0;
    load_close();
    unload();
    load.active = 0;
}

static void load_fail(const char *what, int result) {
    printf("Sound: cannot load track %02u: %s (%d)\n", load.track, what, result);
    load_abort();
}

static FILE *load_open(const char *suffix) {
    char path[sizeof(sound.root) + 24];
    snprintf(path, sizeof(path), "%smusic/%st%02u.%s", sound.root, load.adpcm ? "adpcm/" : "", load.track, suffix);
    FILE *file = fopen(path, "rb");
    if (file) setvbuf(file, NULL, _IONBF, 0); /* reads go straight into the buffers */
    return file;
}

/* Bytes for this wait's read of at most `left` (whole steps up to `limit`, or all of it): 0 when it has no time. */
static uint32_t load_chunk(uint32_t left, uint32_t limit) {
#ifdef NM_SOUND_IO_WORKER
    return left < limit ? left : limit; /* the worker's reads do not take the wait's time; the same each step */
#endif
    const float fit = (0.9f * (float)load.budget - load.call_us) / load.byte_us; /* a tenth kept for the fit's error */
    if (fit < (float)LOAD_READ_STEP && fit < (float)left) return 0;
    if (fit >= (float)left && left <= limit) return left;
    const uint32_t bytes = fit >= (float)limit ? limit : (uint32_t)fit / LOAD_READ_STEP * LOAD_READ_STEP;
    return bytes < left ? bytes : left;
}

/* Time in this wait for one file call of a few bytes. */
#ifdef NM_SOUND_IO_WORKER
static int load_call_fits(void) { return 1; }
#else
static int load_call_fits(void) { return 0.9f * (float)load.budget >= load.call_us + 64.0f * load.byte_us; }
#endif

static int load_read(void *to, uint32_t bytes) {
    const uint64_t start = now_us();
    const int ok = file_read(to, bytes, load.file) == bytes;
    const uint32_t took = (uint32_t)(now_us() - start);
    ++load.reads;
    if (!sound.settling && took > load.budget) ++load.overruns;
    /* Least squares of took = call + bytes * byte, older reads fading (0.97), as the phrases' reader (sfx.c). */
    const double x = bytes, y = took;
    load.fit_n = 0.97 * load.fit_n + 1.0; load.fit_x = 0.97 * load.fit_x + x; load.fit_y = 0.97 * load.fit_y + y;
    load.fit_xx = 0.97 * load.fit_xx + x * x; load.fit_xy = 0.97 * load.fit_xy + x * y;
    const double var = load.fit_n * load.fit_xx - load.fit_x * load.fit_x;
    if (load.fit_n >= 3.0 && var > 1e3 * load.fit_n * load.fit_n) {
        double b = (load.fit_n * load.fit_xy - load.fit_x * load.fit_y) / var, a = (load.fit_y - b * load.fit_x) / load.fit_n;
        if (b < 0.05) b = 0.05;
        if (a < 0.0) a = 0.0;
        load.byte_us = (float)b; load.call_us = (float)a;
    } else {
        /* Until the fit has spread: this read's per-byte cost when it is the worse, the call cost kept. */
        const float b = ((float)took - load.call_us) / (float)(bytes ? bytes : 1u);
        if (b > load.byte_us) load.byte_us = b;
    }
    {   /* A read slower than the model: the call cost takes the excess at once (the fit catches up later). */
        const float excess = (float)took - (load.call_us + (float)bytes * load.byte_us);
        if (excess > 0.0f) load.call_us += excess;
    }
    return ok;
}

#ifdef NM_SOUND_IO_WORKER
/* From a disc (make NATIVE_DISC=1) the music's file calls run on a worker thread below the game's priority: a
 * GD-ROM read blocks only the thread that makes it (KOS cdrom.c waits for the DMA on a semaphore; fs_iso9660 holds
 * only its own mutex), and the drive's seeks take tens to hundreds of milliseconds, more than any idle wait has.
 * A step hands its call to the worker and returns; a later wait takes the result and the step goes on. A step
 * repeats the same call until it is done (here the chunks do not depend on the wait's time), so the result is
 * matched to the call. One call at a time; the closes are handed over without waiting for them. */
enum { IO_OPEN = 1, IO_READ, IO_SEEK, IO_CLOSE };
static struct {
    kthread_t *thread;
    semaphore_t wake;
    volatile int busy, done;   /* busy: with the worker; done: its result waits for its step */
    int op, ok;
    FILE *request;             /* the file of the call (READ, SEEK, CLOSE) */
    FILE *file;                /* OPEN's result */
    void *to;
    uint32_t bytes;
    long offset;
    char path[sizeof(sound.root) + 24];
    alignas(32) unsigned char header[128]; /* the headers' reads (the steps' own are on their stack) */
} io;

static void io_run(void) {
    switch (io.op) {
    case IO_OPEN:
        io.file = fopen(io.path, "rb");
        if (io.file) setvbuf(io.file, NULL, _IONBF, 0); /* reads go straight into the buffers */
        io.ok = io.file != NULL;
        break;
    case IO_READ: io.ok = fread(io.to, 1, io.bytes, io.request) == io.bytes; break;
    case IO_SEEK: io.ok = !fseek(io.request, io.offset, SEEK_SET); break;
    case IO_CLOSE: io.ok = !fclose(io.request); break;
    }
}

static void *io_worker(void *unused) {
    (void)unused;
    for (;;) {
        sem_wait(&io.wake);
        io_run();
        __atomic_store_n(&io.done, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&io.busy, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void io_submit(int op, FILE *file, void *to, uint32_t bytes, long offset, const char *suffix) {
    io.op = op; io.request = file; io.file = NULL; io.to = to; io.bytes = bytes; io.offset = offset; io.done = 0;
    if (suffix) snprintf(io.path, sizeof(io.path), "%smusic/%st%02u.%s", sound.root, load.adpcm ? "adpcm/" : "", load.track, suffix);
    if (op == IO_READ) ++load.reads;
    if (!io.thread) { io_run(); io.done = 1; return; } /* no worker: at once */
    __atomic_store_n(&io.busy, 1, __ATOMIC_RELEASE);
    sem_signal(&io.wake);
}

/* 1: the call is done (io.ok, io.file); 0: it is with the worker (handed over now, or still running). */
static int io_call(int op, FILE *file, void *to, uint32_t bytes, long offset, const char *suffix) {
    if (__atomic_load_n(&io.busy, __ATOMIC_ACQUIRE)) return 0;
    if (io.done) {
        io.done = 0;
        if (io.op == op && io.request == file && io.to == to && io.bytes == bytes && io.offset == offset) return 1;
        if (io.op == IO_OPEN && io.file) fclose(io.file); /* a result nobody waits for (not expected) */
    }
    io_submit(op, file, to, bytes, offset, suffix);
    if (io.thread) return 0;
    io.done = 0;
    return 1;
}

/* A close without waiting for it (the worker is idle at each: its last result was just taken). */
static void io_close(FILE *file) {
    io_drain();
    io_submit(IO_CLOSE, file, NULL, 0, 0, NULL);
}

/* Waits for the worker's call (a dropped load: its buffers are freed next) and drops its result. */
static void io_drain(void) {
    while (__atomic_load_n(&io.busy, __ATOMIC_ACQUIRE)) wait_ms(1);
    if (io.done && io.op == IO_OPEN && io.file) fclose(io.file);
    io.done = 0;
}

static void io_init(void) {
    (void)load_open; (void)load_read; /* the idle wait's own calls (dcload) */
    if (io.thread) return;
    sem_init(&io.wake, 0);
    const kthread_attr_t attr = {.stack_size = 16384, .prio = PRIO_DEFAULT + 1, .label = "music reader"};
    if (!(io.thread = thd_create_ex(&attr, io_worker, NULL))) puts("Sound: no thread for the music's reads: they run in the idle wait");
}
#endif

static int load_begin(uint8_t track) {
    if (!(load.buffer = aligned_buffer(LOAD_CHUNK_MAX + AFX_UPLOAD_ALIGN))) return 0;
    load.active = 1;
    load.track = track;
    load.step = LOAD_BANK_OPEN;
    load.done = load.size = load.dma = 0;
    load.asset = AFX_ASSET_INVALID;
    load.start_us = now_us();
    load.updates = load.waits = load.step_max_us = load.pause_max_us = load.upload_us = 0;
    load.reads = load.overruns = load.ready = load.adpcm = 0;
#ifdef NM_SOUND_IO_WORKER
    load.room_opened = 0;
    load.room_file = NULL;
#endif
    return 1;
}

static uint32_t read32le(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One frame's share of the load. 1: the recording is resident (sound.track). */
static int load_step(void) {
    switch (load.step) {
    case LOAD_BANK_OPEN:   /* each step one file call at most (a seek after an odd header) */
        if (!load_call_fits()) return 0;
#ifdef NM_SOUND_IO_WORKER
        if (!io_call(IO_OPEN, NULL, NULL, 0, 0, "afb")) return 0;
        load.file = io.file;
#else
        load.file = load_open("afb");
#endif
        if (!load.file) { load_fail("no bank file", -AFX_BAD_BOUNDS); return 0; }
        load.step = LOAD_BANK_HEADER;
        return 0;
    case LOAD_BANK_HEADER: {
        if (!load_call_fits()) return 0;
        unsigned char header[AFX_BANK_HEADER_BYTES];
#ifdef NM_SOUND_IO_WORKER
        if (!io_call(IO_READ, load.file, io.header, sizeof(header), 0, NULL)) return 0;
        if (!io.ok) { load_fail("bank file unreadable", -AFX_BAD_FORMAT); return 0; }
        nm_memcpy(header, io.header, sizeof(header));
#else
        if (!load_read(header, sizeof(header))) { load_fail("bank file unreadable", -AFX_BAD_FORMAT); return 0; }
#endif
        /* The AFB header as AICAflow's afx_bank_load_file checks it (bank.c decode_header); a short file shows in
         * the reads. */
        const uint32_t offset = read32le(header + 16), bytes = read32le(header + 20), total = read32le(header + 24);
        if (read32le(header) != AFX_BANK_MAGIC || read32le(header + 4) != AFX_BANK_VERSION ||
            (!read32le(header + 8) && !read32le(header + 12)) || offset < AFX_BANK_HEADER_BYTES ||
            (offset & (AFX_UPLOAD_ALIGN - 1u)) || !bytes || total < offset || bytes != total - offset)
            { load_fail("not an AFB bank", -AFX_BAD_FORMAT); return 0; }
#ifdef NM_SOUND_IO_WORKER
        load.seek_to = offset;
#else
        if (offset != AFX_BANK_HEADER_BYTES && fseek(load.file, (long)offset, SEEK_SET))
            { load_fail("bank file unreadable", -AFX_BAD_FORMAT); return 0; }
#endif
        load.id = (afx_bank_id_t){read32le(header + 8), read32le(header + 12)};
        load.size = bytes;
#ifdef NM_SOUND_IO_WORKER
        load.step = offset != AFX_BANK_HEADER_BYTES ? LOAD_BANK_SEEK : LOAD_ROOM;
#else
        load.step = LOAD_ROOM;
#endif
        return 0;
    }
#ifdef NM_SOUND_IO_WORKER
    case LOAD_BANK_SEEK:
        if (!io_call(IO_SEEK, load.file, NULL, 0, (long)load.seek_to, NULL)) return 0;
        if (!io.ok) { load_fail("bank file unreadable", -AFX_BAD_FORMAT); return 0; }
        load.step = LOAD_ROOM;
        return 0;
#endif
    case LOAD_ROOM: {
        /* Room in the AICA RAM for the bank and its flow (the flow's header names its size). With the effects' race
         * bank resident the PCM8 race pieces of Advanced (t04) and Expert (t05) do not fit: their ADPCM copy in
         * music/adpcm/ (recipes/build_assets.sh step music_adpcm) does. */
        if (!load_call_fits()) return 0;
        uint32_t flow = 0;
#ifdef NM_SOUND_IO_WORKER
        if (!load.room_opened) {
            if (!io_call(IO_OPEN, NULL, NULL, 0, 0, "afx")) return 0;
            load.room_file = io.file;
            load.room_opened = 1;
        }
        if (load.room_file) {
            if (!io_call(IO_READ, load.room_file, io.header, AFX_FILE_HEADER_BYTES, 0, NULL)) return 0;
            if (io.ok) flow = read32le(io.header + 8);
            io_close(load.room_file);
            load.room_file = NULL;
        }
        load.room_opened = 0;
#else
        FILE *file = load_open("afx");
        if (file) {   /* read as the loader's reads are (timed, fitted, overruns counted) */
            unsigned char header[AFX_FILE_HEADER_BYTES];
            FILE *const bank_file = load.file;
            load.file = file;
            if (load_read(header, sizeof(header))) flow = read32le(header + 8);
            load.file = bank_file;
            fclose(file);
        }
#endif
        afx_mem_stats_t stats;
        const uint32_t bank = (load.size + 31u) & ~31u, need = bank + ((flow + 31u) & ~31u);
        if (!load.adpcm && !afx_mem_stats(&stats) && (bank > stats.largest_free_block || need > stats.free_bytes)) {
#ifdef NM_SOUND_IO_WORKER
            io_close(load.file);
#else
            fclose(load.file);
#endif
            load.file = NULL;
            load.adpcm = 1;
            load.step = LOAD_BANK_OPEN;
            return 0;
        }
        const int result = afx_sample_bank_stream_begin(load.size, &load.asset);
        if (result) { load_fail("no AICA RAM for the bank", result); return 0; }
        load.step = LOAD_BANK;
        return 0;
    }
    case LOAD_BANK:
        if (load.dma) {
            bool complete = false;
            const int result = afx_sample_bank_stream_dma_poll(load.asset, &complete);
            if (result) { load_fail("bank upload", result); return 0; }
            if (!complete) return 0;
            load.done += load.dma;
            load.dma = 0;
        }
        if (load.done == load.size) {
            const int result = afx_sample_bank_stream_finish(load.asset);
            if (result) { load_fail("bank upload", result); return 0; }
            sound.bank = (afx_bank_t){.asset = load.asset, .id = load.id, .bytes = load.size};
            load.asset = AFX_ASSET_INVALID;
#ifdef NM_SOUND_IO_WORKER
            io_close(load.file);
#else
            fclose(load.file);
#endif
            load.file = NULL;
            free(load.buffer);
            load.buffer = NULL;
            load.step = LOAD_FLOW_OPEN;
            return 0;
        }
        {
            uint32_t bytes = load_chunk(load.size - load.done, LOAD_CHUNK_MAX);
            if (bytes < load.size - load.done) bytes &= ~(AFX_UPLOAD_ALIGN - 1u); /* only the last chunk is short */
            if (!bytes) return 0;
#ifdef NM_SOUND_IO_WORKER
            if (!io_call(IO_READ, load.file, load.buffer, bytes, 0, NULL)) return 0;
            if (!io.ok) { load_fail("bank file short", -AFX_BAD_FORMAT); return 0; }
#else
            if (!load_read(load.buffer, bytes)) { load_fail("bank file short", -AFX_BAD_FORMAT); return 0; }
#endif
            nm_memset(load.buffer + bytes, 0, ((bytes + AFX_UPLOAD_ALIGN - 1u) & ~(AFX_UPLOAD_ALIGN - 1u)) - bytes);
#ifndef NM_SOUND_TEST
            nm_sfx_quiesce(); /* a phrase's chunk in flight: AICAflow takes one DMA at a time */
#endif
            const int result = afx_sample_bank_stream_dma_begin(load.asset, load.done, load.buffer, bytes);
            if (result) { load_fail("bank upload", result); return 0; }
            load.dma = bytes;
        }
        return 0;
    case LOAD_FLOW_OPEN:
        if (!load_call_fits()) return 0;
#ifdef NM_SOUND_IO_WORKER
        if (!io_call(IO_OPEN, NULL, NULL, 0, 0, "afx")) return 0;
        load.file = io.file;
#else
        load.file = load_open("afx");
#endif
        if (!load.file) { load_fail("no flow file", -AFX_BAD_BOUNDS); return 0; }
        load.step = LOAD_FLOW_HEADER;
        return 0;
    case LOAD_FLOW_HEADER: {
        /* The AFX's 80-byte header names the file's length (format.h afx_file_header_t.total_size): no seeks. */
        if (!load_call_fits()) return 0;
        unsigned char header[AFX_FILE_HEADER_BYTES];
#ifdef NM_SOUND_IO_WORKER
        if (!io_call(IO_READ, load.file, io.header, sizeof(header), 0, NULL)) return 0;
        if (!io.ok) { load_fail("flow file unreadable", -AFX_BAD_FORMAT); return 0; }
        nm_memcpy(header, io.header, sizeof(header));
#else
        if (!load_read(header, sizeof(header))) { load_fail("flow file unreadable", -AFX_BAD_FORMAT); return 0; }
#endif
        const uint32_t total = read32le(header + 8);
        if (total < AFX_FILE_HEADER_BYTES || total > (4u << 20) || !(load.flow = nm_alloc(total)))
            { load_fail("flow file unreadable", -AFX_BAD_FORMAT); return 0; }
        nm_memcpy(load.flow, header, sizeof(header));
        load.done = AFX_FILE_HEADER_BYTES;
        load.size = total;
        load.step = load.done == load.size ? LOAD_FLOW_UPLOAD : LOAD_FLOW;
        return 0;
    }
    case LOAD_FLOW: {
        const uint32_t bytes = load_chunk(load.size - load.done, LOAD_CHUNK_MAX);
        if (!bytes) return 0;
#ifdef NM_SOUND_IO_WORKER
        if (!io_call(IO_READ, load.file, load.flow + load.done, bytes, 0, NULL)) return 0;
        if (!io.ok) { load_fail("flow file short", -AFX_BAD_FORMAT); return 0; }
#else
        if (!load_read(load.flow + load.done, bytes)) { load_fail("flow file short", -AFX_BAD_FORMAT); return 0; }
#endif
        load.done += bytes;
        if (load.done == load.size) {
#ifdef NM_SOUND_IO_WORKER
            io_close(load.file);
#else
            fclose(load.file);
#endif
            load.file = NULL;
            load.step = LOAD_FLOW_UPLOAD;
        }
        return 0;
    }
    default: {
        if ((uint64_t)load.budget * LOAD_UPLOAD_BYTES_PER_MS / 1000u < load.size) return 0;
        const uint64_t start = now_us();
        const int result = afx_bank_flow_upload(&sound.bank, load.flow, load.size, &sound.flow);
        load.upload_us = (uint32_t)(now_us() - start);
        if (result) { load_fail("flow upload", result); return 0; }
        load_close();
        load.active = 0;
        sound.track = load.track;
        /* Reported at shutdown: a console write inside a frame masks interrupts while it runs. */
        if (loads < LOAD_REPORTS) {
            report[loads] = (load_report_t){load.track | (uint32_t)load.adpcm << 8, (uint32_t)((now_us() - load.start_us) / 1000), load.updates,
                load.waits, sound.bank.bytes, load.size, load.step_max_us, load.pause_max_us, load.upload_us, load.reads,
                load.overruns, (uint32_t)load.call_us, (uint32_t)(1000.0f / load.byte_us)};
        }
        ++loads;
        return 1;
    }
    }
}

/* One step of the load in an idle wait (or at a pause), timed; the update starts the recording once it is done. */
static uint32_t load_wait(uint32_t budget_us) {
    if (sound.state != SOUND_LOADING || !load.active || load.ready || load.dma) return 0;
    load.budget = budget_us;
    ++load.waits;
    const uint64_t start = now_us();
    const uint32_t reads = load.reads;
    load.ready = (uint8_t)load_step();
    const uint32_t took = (uint32_t)(now_us() - start);
    /* No file call fitted: the model eases by 1/1024 a wait (half in about 12 s), so one slow spell does not stop
     * the load for good; a host that stays slow costs one probe about every dozen seconds. */
    if (load.reads == reads && !sound.settling && load.call_us > 1200.0f) load.call_us -= load.call_us * (1.0f / 1024.0f);
    if (sound.settling) { if (took > load.pause_max_us) load.pause_max_us = took; }
    else if (took > load.step_max_us) load.step_max_us = took;
    return took;
}

/* A DMA of the bank started at the previous update has completed by now: collect it before the effects'
 * module asks AICAflow for an upload of its own (one upload at a time). */
static void load_collect(void) {
    bool complete = false;
    if (afx_sample_bank_stream_dma_poll(load.asset, &complete) || !complete) return;
    load.done += load.dma;
    load.dma = 0;
}

#ifdef NATIVE_SOUND_LOG
/* make NATIVE_SOUND_LOG=1: one line a music request, start and end (what the music did, and when). */
#define SLOG(what, t) printf("Sound log %llu ms: %s %d (state %d, resident %u)\n", (unsigned long long)(now_us() / 1000u), what, (int)(t), (int)sound.state, (unsigned)sound.track)
#else
#define SLOG(what, t) ((void)0)
#endif

/* IDLE: carry out a wanted change. A recording that is not resident starts loading
 * (LOADING); the start follows when it is. */
static void start_wanted(void) {
    const int wanted = sound.wanted;
    if (wanted < 0) return;
    if (!wanted) { sound.wanted = -1; return; }
    if (sound.track != (uint8_t)wanted) {
        unload();
        if (!load_begin((uint8_t)wanted)) {
            printf("Sound: no heap for a staging buffer: track %02u not loaded\n", (unsigned)wanted);
            sound.wanted = -1;
            return;
        }
        sound.state = SOUND_LOADING;
        return;
    }
    if (sound.hold) return; /* resident: the start waits for nm_sound_hold(0) */
    const int result = afx_instance_activate(sound.flow, &sound.instance);
    if (result == -AFX_BUSY) return; /* An earlier lifecycle command is unacknowledged: next update. */
    sound.wanted = -1;
    if (result) { printf("Sound: starting track %02u failed (%d)\n", sound.track, result); return; }
    sound.loops = sound.wanted_loops;
    sound.fade_us = 0;
    SLOG("play", sound.track);
    sound.state = SOUND_PLAYING;
}

/* LOADING: the idle waits do the work; a change of mind drops the load. */
static void loading(void) {
    ++load.updates;
    if (sound.wanted != (int)load.track) { load_abort(); sound.state = SOUND_IDLE; start_wanted(); return; }
#ifdef NM_SOUND_LOAD_SYNC
    /* make NATIVE_MUSIC_LOAD=sync: the whole load in this update, as before (an A/B). */
    while (load.active && !load.ready) { if (load.dma) { wait_ms(1); load_collect(); } else load_wait(1000000u); }
#endif
    if (load.ready) { load.ready = 0; sound.state = SOUND_IDLE; start_wanted(); }
    else if (!load.active) { sound.state = SOUND_IDLE; sound.wanted = -1; } /* failed: reported */
}

static void want(int track, int loops) {
    SLOG("want", track);
    sound.wanted = track;
    sound.wanted_loops = (uint8_t)loops;
}

/* make NATIVE_SOUND=0: AICAflow is not started. A volatile read, so both
 * variants link the same code and the A/B measures the sound work only. */
#ifdef NM_SOUND_OFF
static volatile const int sound_off = 1;
#else
static volatile const int sound_off = 0;
#endif

int nm_sound_init(const char *asset_root) {
    if (sound.state != SOUND_OFF) return 1;
    if (sound_off) { puts("Sound: off (make NATIVE_SOUND=0): AICAflow not started, silent"); return 0; }
    if (!asset_root || strlen(asset_root) >= sizeof(sound.root)) return 0;
    const int result = afx_init(FIRMWARE, FIRMWARE_SIZE);
    if (result == -AFX_TIMEOUT) {
        /* The ARM7 firmware did not boot within ~1 s, or its boot report, the AICA's
         * interrupt to the SH4, did not arrive ~100 ms after (an emulator without that
         * interrupt; a firmware not of the library's ABI). */
        printf("Sound: AICAflow did not start: no boot report from the ARM7 by interrupt (%d, timeout): silent\n", result);
        return 0;
    }
    if (result) { printf("Sound: AICAflow did not start (%d): silent\n", result); return 0; }
    afx_set_end_callback(on_instance_end, NULL);
#ifndef NM_SOUND_TEST
    /* The effects first: their bank stays at the bottom of the AICA arena, the recordings come and go above. */
    nm_sfx_init(asset_root);
#endif
    strcpy(sound.root, asset_root);
#ifdef NM_SOUND_IO_WORKER
    io_init();
#endif
    sound.state = SOUND_IDLE;
    sound.wanted = -1;
    afx_mem_stats_t stats;
    if (!afx_mem_stats(&stats))
        printf("Sound: AICAflow up, %lu bytes of AICA RAM for assets\n", (unsigned long)stats.total_bytes);
#ifdef NM_SOUND_TEST
    /* make NATIVE_SOUND_TEST=NN: recording NN from the start, repeating; no command needed. */
    want(NM_SOUND_TEST, 1);
#endif
    return 1;
}

void nm_sound_command(uint32_t command) {
    if (sound.state == SOUND_OFF || !command) return;
#ifdef NM_SOUND_TEST
    ++sound.ignored;return; /* The playback check plays its one recording only. */
#endif
    nm_sfx_command(command); /* Effects, voices and the engine: audio/sfx.h. */
    const uint8_t status = (uint8_t)(command >> 16), a = (uint8_t)(command >> 8), b = (uint8_t)command;
    const nm_music_entry_t *music = (status & 0xf0) == 0xa0 ? nm_music_find(command) : NULL;
    if (music) want(music->track, music->loops);
    else if ((status & 0xf0) == 0xa0 && a == 0x10 && ++sound.unmapped <= 8) {
        /* Bank 0x10 is all music: a start without a recording is unexpected. Effects
         * and voices (banks 0x20..0x22) are audio/sfx.h's. */
        printf("Sound: music start %06lx has no recording\n", (unsigned long)command);
        ++sound.ignored;
    } else if (status == 0xff || ((status & 0xf0) == 0xb0 && a == 0x50 && b != 0x7d)) want(0, 0);
    /* Results without the podium (b35c..b3c4) silence music channels 0..8 with
     * b00700..b80700; a recording has no channels, so it stops at the first. */
    else if (status == 0xb0 && a == 0x07 && !b) want(0, 0);
    else if ((status & 0xf0) == 0xb0 && a == 0x1a && b && sound.state == SOUND_PLAYING && !sound.fade_us) {
        sound.fade_start_us = now_us();
        sound.fade_us = (uint64_t)FADE_STEPS * b * 1000000u / SEQUENCER_HZ;
    } else ++sound.ignored;
}

/* The current instance ended and AICAflow freed it: its flow and bank are free to go. */
static void instance_ended(int by_itself, uint32_t state, uint32_t result) {
    if (state == AFX_ERROR) printf("Sound: track %02u stopped with error %lu\n", sound.track, (unsigned long)result);
    else if (by_itself && sound.loops && sound.wanted < 0) want(sound.track, 1); /* Again from its start. */
    SLOG(by_itself ? "ended" : "stopped", sound.track);
    sound.instance = 0;
    sound.state = SOUND_IDLE;
}

/* Settles the reports the end callback queued (two loads when there are none). */
static void take_ended(void) {
    uint32_t tail = ended_tail;
    const uint32_t head = __atomic_load_n(&ended_head, __ATOMIC_ACQUIRE);
    for (; tail != head; ++tail) {
        const unsigned at = tail % ENDED_CAPACITY;
        if (sound.instance && ended[at].instance == sound.instance)
            instance_ended(sound.state == SOUND_PLAYING, ended[at].state, ended[at].result);
    }
    __atomic_store_n(&ended_tail, tail, __ATOMIC_RELEASE);
}

/* Not inlined into the updaters (LTO): their code and placement stay those of a build whose sound update
 * was smaller. */
__attribute__((noinline)) void nm_sound_update(void) {
    if (sound.state == SOUND_OFF) return;
    if (sound.state == SOUND_LOADING && load.dma) load_collect();
    nm_sfx_update();
    take_ended();
    /* After a stop the handle goes stale once AICAflow has freed the instance; the
     * end report says so too, this covers a full report queue. A cached status: no AICA access. */
    afx_instance_status_t status;
    if (sound.state == SOUND_ENDING && afx_instance_status(sound.instance, &status) == -AFX_STALE_GENERATION)
        instance_ended(0, AFX_DONE, 0);
    switch (sound.state) {
    case SOUND_PLAYING:
        if (sound.fade_us) {
            const uint64_t elapsed = now_us() - sound.fade_start_us;
            if (elapsed >= sound.fade_us) { sound.fade_us = 0; if (sound.wanted < 0) sound.wanted = 0; }
            else afx_instance_gain(sound.instance, (uint8_t)(255u - elapsed * 255u / sound.fade_us));
        }
        if (sound.wanted >= 0) {
            /* -AFX_BUSY (activation not yet acknowledged): again next update. Stale: it ended
             * meanwhile and its report is on the way. */
            const int result = afx_instance_stop(sound.instance);
            if (!result || result == -AFX_STALE_GENERATION) sound.state = SOUND_ENDING;
        }
        break;
    case SOUND_IDLE:
        start_wanted();
        break;
    case SOUND_LOADING:
        loading();
        break;
    }
}

void nm_sound_hold(int on) { sound.hold = on; }

void nm_sound_settle(void) {
    sound.settling = 1;
#ifdef NM_SOUND_IO_WORKER
    /* The worker's calls: a millisecond's wait each time one is running (a slow disc: up to 30 s). */
    for (unsigned i = 0; i < 30000 && sound.state != SOUND_OFF; ++i) {
#else
    for (unsigned i = 0; i < 4000 && sound.state != SOUND_OFF; ++i) {
#endif
        if (sound.wanted < 0 && (sound.state == SOUND_IDLE || sound.state == SOUND_PLAYING)) break;
        if (sound.hold && sound.state == SOUND_IDLE && sound.track == (uint8_t)sound.wanted) break; /* loaded, held */
        nm_sound_update();
#ifdef NM_SOUND_IO_WORKER
        if (sound.state == SOUND_LOADING && !load.dma && !__atomic_load_n(&io.busy, __ATOMIC_ACQUIRE)) load_wait(1000000u);
#else
        if (sound.state == SOUND_LOADING && !load.dma) load_wait(1000000u);
#endif
        else wait_ms(1);
    }
    sound.settling = 0;
}

uint32_t nm_sound_idle(uint32_t budget_us) { return sound.state == SOUND_OFF ? 0 : load_wait(budget_us); }

int nm_sound_loading(void) { return sound.state == SOUND_LOADING; }

void nm_sound_shutdown(void) {
    if (sound.state == SOUND_OFF) return;
    if (sound.state == SOUND_LOADING) { load_abort(); sound.state = SOUND_IDLE; }
    sound.wanted = 0;
    sound.loops = 0;
    for (unsigned i = 0; i < 1000 && sound.state != SOUND_IDLE; ++i) { nm_sound_update(); wait_ms(1); }
    if (sound.state != SOUND_IDLE) { printf("Sound: track %02u did not stop; AICAflow left running\n", sound.track); return; }
    unload();
    nm_sfx_shutdown(wait_ms);
    afx_set_end_callback(NULL, NULL);
    afx_shutdown();
    for (unsigned i = 0; i < loads && i < LOAD_REPORTS; ++i)
        printf("Sound: track %02lu%s loaded in %lu ms over %lu updates, %lu idle-wait steps (bank %lu bytes, flow %lu bytes;"
               " longest step in a wait %lu us, at a pause %lu us, flow upload %lu us; %lu reads, %lu over budget;"
               " fitted %lu us a call + %lu bytes/ms)\n",
               (unsigned long)(report[i].track & 255u), report[i].track >> 8 ? " (ADPCM)" : "",
               (unsigned long)report[i].ms, (unsigned long)report[i].updates,
               (unsigned long)report[i].waits, (unsigned long)report[i].bank, (unsigned long)report[i].flow,
               (unsigned long)report[i].step_max_us, (unsigned long)report[i].pause_max_us, (unsigned long)report[i].upload_us,
               (unsigned long)report[i].reads, (unsigned long)report[i].overruns, (unsigned long)report[i].call_us,
               (unsigned long)report[i].bytes_per_ms);
    printf("Sound: shut down (%u commands ignored, %u loads)\n", sound.ignored, loads);
    sound.state = SOUND_OFF;
}
