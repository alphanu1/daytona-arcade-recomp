#ifndef DAYTONA_NATIVE_SAMPLE_PROFILE_H
#define DAYTONA_NATIVE_SAMPLE_PROFILE_H
#include <stdint.h>

/* NATIVE_SAMPLE_PROFILE: statistical SH4 profile of a (scripted) run.
 * TMU1, the one timer channel KOS leaves free, interrupts at
 * NM_SAMPLE_RATE_HZ; the handler stores the interrupted PC and one tag byte in
 * a buffer allocated before anything is loaded (nm_sample_reserve: 1.92 MB
 * at the bottom of the heap, below every asset, so the scenes that are
 * loaded and released around it leave no hole it would have to fit).
 * TMU0 (scheduler), TMU2 (clock) and the
 * performance counters are not touched. Nothing is allocated or printed while
 * sampling; a full buffer only counts dropped samples. The buffer is written
 * to <asset root>sample-profile.bin after the engine has shut down.
 *
 * Sample word: bits 0..22 (PC-0x8c000000)>>1, bit 23 PC outside main RAM,
 * bits 24..25 frame section, bits 26..28 scene phase.
 * File: "NMSP", version 1, rate Hz, sample count, dropped count, then words.
 * tools/sample_profile_report.py turns it into a report with the ELF. */
enum { NM_SAMPLE_RATE_HZ=1997, NM_SAMPLE_CAPACITY=480000 };
enum { NM_SAMPLE_OTHER, NM_SAMPLE_UPDATER, NM_SAMPLE_WAIT, NM_SAMPLE_REGISTRATION };
enum { NM_SAMPLE_PHASE_OTHER, NM_SAMPLE_PHASE_ROLLING, NM_SAMPLE_PHASE_RACE, NM_SAMPLE_PHASE_FINISH,
       NM_SAMPLE_PHASE_PODIUM, NM_SAMPLE_PHASE_RESULTS, NM_SAMPLE_PHASE_ATTRACT };

#ifdef NATIVE_SAMPLE_PROFILE
extern volatile uint32_t nm_sample_tag; /* Already shifted: section<<24 | phase<<26. */
static inline void nm_sample_section(unsigned section) {
    nm_sample_tag=(nm_sample_tag&~(UINT32_C(3)<<24))|((uint32_t)section<<24);
}
static inline void nm_sample_phase(unsigned phase) {
    nm_sample_tag=(nm_sample_tag&~(UINT32_C(7)<<26))|((uint32_t)phase<<26);
}
/* The buffer alone, at program start. Returns 0 if it is refused. */
int nm_sample_reserve(void);
/* Start sampling (reserves the buffer if nm_sample_reserve was not called);
 * returns 0 (and samples nothing) if the buffer is refused. */
int nm_sample_start(void);
/* Stop the timer, then write and report. Call after the engine loop returned. */
void nm_sample_finish(const char *root);
#else
static inline void nm_sample_section(unsigned section) { (void)section; }
static inline void nm_sample_phase(unsigned phase) { (void)phase; }
static inline int nm_sample_reserve(void) { return 1; }
static inline int nm_sample_start(void) { return 1; }
static inline void nm_sample_finish(const char *root) { (void)root; }
#endif
#endif
