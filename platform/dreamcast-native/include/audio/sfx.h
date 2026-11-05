/* Sound effects and voices on the AICA through AICAflow; include/audio/sound.h owns AICAflow
 * and hands every command here before its music rules.
 *
 * Assets (recipes/build_sfx.sh, ROM-derived): ASSET_ROOT/sfx/race.afb (one sample bank) and
 * race.dsx (tools/prepare_sfx.cpp pack: a directory sorted by command, the velocity gains, the
 * engine table and one AFX flow per entry, all bound to race.afb). Loaded once at start-up,
 * before any music, so the bank sits at the bottom of the AICA arena and the recordings above it.
 *
 * What the original's commands do here (src/runtime/native_sound_sequencer.cpp is the model):
 *  - ae20xx..ae22xx, a sequence: its flow is started. A new start leaves earlier ones sounding,
 *    as the board leaves a replaced sequence's voices.
 *  - 9xnnvv (vv nonzero), a note of the effect kit: its flow, at the gain of velocity vv.
 *  - be16..be1f, be34..be37, the engine controllers: the board's update_engines on ten
 *    controlled flows (engine rpm x6, opponent left/right, slip, skid). The words change in
 *    nm_sfx_update, once per updater call and only those that changed (afx_instance_patch).
 *  - be507d: every effect stops (not the engine); other be50xx except 7e: the engine too.
 * Commands without a flow in the bank are counted only; with NM_SFX_VOICE_STREAM (NATIVE_SFX_VOICE_STREAM=1)
 * the announcer's phrases come from ASSET_ROOT/sfx/voices.pak instead (build_sfx.sh --voices stream): one at a
 * time, read by a worker thread, loaded into AICA RAM beside the race bank and the race music, freed when it
 * ends; two wait at most. (--voices resident puts them into the race bank instead: no flag.)
 * Every call comes from the enDjinn updater thread. */
#ifndef NM_AUDIO_SFX_H
#define NM_AUDIO_SFX_H

#include <stdint.h>

typedef struct {
    unsigned flows;        /* flows resident */
    unsigned started;      /* one-shot flows started */
    unsigned missing;      /* effect commands without a flow in the bank */
    unsigned dropped;      /* starts AICAflow refused (no channel or slot) */
    unsigned patches;      /* engine patches sent */
    unsigned retries;      /* engine patches or stops put off to the next update (busy, queue full) */
    unsigned layer_starts; /* engine layers keyed on */
    unsigned voices;       /* NM_SFX_VOICE_STREAM: phrases in sfx/voices.pak */
    unsigned voice_starts, voice_drops, voice_read_failures; /* phrases played; dropped (queue full, AICA refused) */
    uint32_t voice_read_us, voice_upload_us; /* longest read (worker thread), longest updater step of the voices */
    uint32_t voice_chunk_us, voice_chunk_bytes; /* /pc: the longest idle-wait read and its size */
    uint32_t voice_bytes_per_ms;           /* /pc: the measured read throughput */
    uint32_t voice_latency_max, voice_latency_mean; /* updates from a phrase's request to its start */
    unsigned voice_reads, voice_prefetches, voice_prefetch_cancels, voice_hits, voice_overruns; /* reads; requests found read */
    uint32_t voice_call_us;                /* /pc: the fitted cost of a read call (plus bytes at voice_bytes_per_ms) */
} nm_sfx_stats_t;

/* After afx_init. asset_root ends in '/'. 0: no effects (reported); the music is unaffected. */
int nm_sfx_init(const char *asset_root);
/* One packed command (status in bits 23..16). Music commands are ignored here. */
void nm_sfx_command(uint32_t command);
/* Once per updater call: the engine layers' changes. */
void nm_sfx_update(void);
/* Stops everything, waits (wait_ms, at most about a second) until AICAflow has freed the
 * instances, then frees the flows and the bank. Before afx_shutdown. */
void nm_sfx_shutdown(void (*wait_ms)(unsigned));
const nm_sfx_stats_t *nm_sfx_stats(void);
/* Before any other AICAflow load (sound.c's recordings): completes a streamed phrase's G2 chunk in flight,
 * since AICAflow takes one DMA at a time. Nothing without NM_SFX_VOICE_STREAM. */
void nm_sfx_quiesce(void);
/* The announcer's phrases likely to be requested next, most likely first (race_sound.c, once a tick): the
 * streamed phrases prefetch them. Nothing without NM_SFX_VOICE_STREAM. */
void nm_sfx_hint(const uint32_t *keys, unsigned n);
/* The phrases' share of the idle wait for the PVR (code/audio/idle_wait.c): one /pc read within budget_us,
 * returns the microseconds it took (0: nothing to read). Nothing without NM_SFX_VOICE_STREAM. */
uint32_t nm_sfx_idle(uint32_t budget_us);

#endif
