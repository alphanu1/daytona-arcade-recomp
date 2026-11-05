#ifndef DAYTONA_NATIVE_SCENE_LOADER_H
#define DAYTONA_NATIVE_SCENE_LOADER_H
/* A scene's load in the frames of the screen before it (code/scene_loader.c): the load code runs unchanged on a
 * thread of its own that only ever runs while the updater's thread waits for it, in slices handed over from
 * enDjinn's idle wait for the PVR (code/audio/idle_wait.c, after the sound's reads), so the frames go on. A slice
 * ends at the next file call or yield point that would pass its end: every file read of the job (fs_read, KOS) is
 * cut into pieces sized by a fitted cost (a fixed cost a call plus one a byte, as the sound's loads), opening a
 * file waits for a slice with time for it, and nm_loader_yield marks long computations. dcload's /pc calls hold
 * the CPU with interrupts off, which is why the reads must fit the waits. Palette RAM writes of the job are held
 * back (render/materials/palette_ram.h nm_palette_ram_defer) while the screen before it shows its own palettes.
 *
 * Without a Dreamcast (host builds) nm_loader_start runs the job at once. */
#include <stdint.h>

typedef int (*nm_loader_job_t)(void *arg);

/* At boot, before the assets: the thread (its stack is static). 0: no thread; jobs then run at once. */
int nm_loader_init(void);
/* Starts JOB (named for the report); its slices come from the idle waits. 0: a job is running already. */
int nm_loader_start(nm_loader_job_t job, void *arg, const char *name);
/* A job started and not yet finished. */
int nm_loader_busy(void);
/* The finished job's return value (0 also when none finished). */
int nm_loader_result(void);
/* Idle wait: the job may run until DEADLINE_US (timer_us_gettime64). The updater's thread waits meanwhile. */
void nm_loader_idle(uint64_t deadline_us);
/* Runs the rest of the job at once (a credit while it loads); returns its result. */
int nm_loader_finish(void);
/* Inside the job: hands the slice back when COST_US more would pass its end (0: at its end). No-op elsewhere.
 * SITE names the yield point (file:line): the report lists the longest stretches of the job between two points. */
#ifdef _arch_dreamcast
void nm_loader_yield_at(uint32_t cost_us, const char *site);
#else
static inline void nm_loader_yield_at(uint32_t cost_us, const char *site) { (void)cost_us; (void)site; }
#endif
#define NM_LOADER_STRING2(x) #x
#define NM_LOADER_STRING(x) NM_LOADER_STRING2(x)
#define nm_loader_yield(cost_us) nm_loader_yield_at((cost_us), __FILE__ ":" NM_LOADER_STRING(__LINE__))
/* The caller is the job (its thread). */
int nm_loader_in_job(void);
/* The job's figures, one line: wall time, slices, time in slices, reads, bytes, longest slice and overruns; then the
 * longest stretches between two yield points (or file calls) with their sites. */
void nm_loader_report(void);
#endif
