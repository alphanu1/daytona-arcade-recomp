/* enDjinn's frame waits in pvr_wait_ready for the PVR before the scene's registration (local.cfg.mk links it with
 * --wrap=pvr_wait_ready). That wait is the CPU's idle time: the file reads that hold the whole CPU with
 * interrupts off (dcload's /pc) run there, never in an update, sized to the shortest wait of the last frames (read
 * and wait together) less a margin, so the frame's work is not delayed. First the announcer's phrases (audio/sfx.h
 * nm_sfx_idle, short and time-critical), then the music's loads (audio/sound.h nm_sound_idle) with what is left,
 * then a scene load running in the frames of the screen before it (scene_loader.h) until the same end. */
#include "audio/sfx.h"
#include "audio/sound.h"
#include "scene_loader.h"
#ifdef _arch_dreamcast
#include <arch/timer.h>
#include <stdint.h>

#define IDLE_FRAMES 16u     /* waits whose shortest sizes the next reads */
#define IDLE_MARGIN_US 1000u

static uint32_t idle_us[IDLE_FRAMES];
static unsigned idle_next;

/* model_renderer.c (weak: a build without the model renderer has no cache). */
unsigned nm_render_world_stream_idle(unsigned budget_us) __attribute__((weak));

int __real_pvr_wait_ready(void);
int __wrap_pvr_wait_ready(void);
int __wrap_pvr_wait_ready(void) {
    /* A scene load's job runs only inside this wait of the updater's thread: the PVR's next scene is not its to
     * wait for (the screen it loads behind does not draw what it loads). */
    if (nm_loader_in_job()) return 0;
    const uint64_t begin = timer_us_gettime64();
    uint32_t shortest = UINT32_MAX;
    for (unsigned i = 0; i < IDLE_FRAMES; ++i) if (idle_us[i] < shortest) shortest = idle_us[i];
    if (shortest > IDLE_MARGIN_US) {
        const uint32_t budget = shortest - IDLE_MARGIN_US, used = nm_sfx_idle(budget);
        if (used < budget) (void)nm_sound_idle(budget - used);
        if (nm_loader_busy()) nm_loader_idle(begin + budget);
    }
    /* Then the world's stream record cache (render/models/model_renderer.h), with what is left; it counts the frames
     * also when nothing is left. Never while a scene load runs: the load builds the renderer's tables. */
    if (nm_render_world_stream_idle && !nm_loader_busy()) {
        const uint32_t spent = (uint32_t)(timer_us_gettime64() - begin);
        const uint32_t left = shortest > IDLE_MARGIN_US && shortest - IDLE_MARGIN_US > spent ? shortest - IDLE_MARGIN_US - spent : 0;
        (void)nm_render_world_stream_idle(left);
    }
    const int result = __real_pvr_wait_ready();
    idle_us[idle_next++ % IDLE_FRAMES] = (uint32_t)(timer_us_gettime64() - begin);
    return result;
}
#endif
