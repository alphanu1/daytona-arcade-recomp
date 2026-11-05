/* code/scene_loader.c on the host (KOS shimmed on pthreads and a simulated clock, tests/kos_shim): a job's file reads
 * cut into pieces that fit each idle slice, its yield points handing the slice back, strict alternation with the
 * updater's thread, the rest at once (a credit), a second job after the first. The file calls cost what the loader's
 * start values say (1.2 ms a call + 7.4 bytes/us), so the pieces must end inside their slice. */
#define _arch_dreamcast 1
#include "../code/scene_loader.c"
#include <assert.h>
#include <string.h>

uint64_t shim_clock_us;
_Thread_local kthread_t *shim_self;

enum { FILE_BYTES = 1500000, SMALL_BYTES = 200000, CPU_STEPS = 30, CPU_US = 4000, FRAME_US = 16683, WORK_US = 1500,
       IDLE_US = 13000 };
static uint8_t file_data[FILE_BYTES], buffer[FILE_BYTES];
static size_t file_at;
static int open_calls, main_thread_reads, outside_slice;
static uint64_t slice_end_us; /* the slice the job holds, for the checks (0: none) */

ssize_t __real_fs_read(file_t hnd, void *to, size_t cnt) {
    (void)hnd;
    if (!shim_self) ++main_thread_reads;
    else if (!slice_end_us) ++outside_slice;
    shim_clock_us += 1200u + (uint64_t)((double)cnt * 0.135);
    const size_t left = FILE_BYTES - file_at, n = cnt < left ? cnt : left;
    memcpy(to, file_data + file_at, n);
    file_at += n;
    return (ssize_t)n;
}
file_t __real_fs_open(const char *fn, int mode) {
    (void)fn; (void)mode;
    ++open_calls; file_at = 0; shim_clock_us += 2000u;
    return 3;
}

/* The job's other file calls (a report line, a seek, a close): a call's cost each. */
static int other_calls;
ssize_t __real_fs_write(file_t hnd, const void *from, size_t cnt) {
    (void)hnd; (void)from;
    if (shim_self && !slice_end_us) ++outside_slice;
    ++other_calls; shim_clock_us += 1200u; return (ssize_t)cnt;
}
off_t __real_fs_seek(file_t hnd, off_t offset, int whence) {
    (void)hnd; (void)whence;
    if (shim_self && !slice_end_us) ++outside_slice;
    ++other_calls; shim_clock_us += 1200u; return offset;
}
int __real_fs_close(file_t hnd) {
    (void)hnd;
    if (shim_self && !slice_end_us) ++outside_slice;
    ++other_calls; shim_clock_us += 1200u; return 0;
}

static int job_done;
static int test_job(void *arg) {
    const size_t total = (size_t)(uintptr_t)arg;
    (void)__wrap_fs_open("/pc/a", 0);
    if (__wrap_fs_read(3, buffer, total) != (ssize_t)total) return 0;
    for (unsigned i = 0; i < CPU_STEPS; ++i) { nm_loader_yield(0); shim_clock_us += CPU_US; }
    (void)__wrap_fs_open("/pc/b", 0);
    static uint8_t small[SMALL_BYTES];
    if (__wrap_fs_read(3, small, SMALL_BYTES) != SMALL_BYTES) return 0;
    if (__wrap_fs_seek(3, 0, 0) != 0 || __wrap_fs_write(1, "x", 1) != 1 || __wrap_fs_close(3)) return 0;
    job_done = 1;
    return memcmp(buffer, file_data, total) == 0 && !memcmp(small, file_data, SMALL_BYTES);
}

/* One idle wait of the updater: the slice until `until`; returns how far past it the job ran (us). */
static uint64_t idle(uint64_t until) {
    slice_end_us = until;
    nm_loader_idle(until);
    slice_end_us = 0;
    return shim_clock_us > until ? shim_clock_us - until : 0;
}

int main(void) {
    for (size_t i = 0; i < FILE_BYTES; ++i) file_data[i] = (uint8_t)(i * 2654435761u >> 13);
    assert(nm_loader_init());
    /* A job in the frames of a screen: every read piece ends inside its slice, CPU steps by at most one step. */
    assert(nm_loader_start(test_job, (void *)(uintptr_t)FILE_BYTES, "test") && nm_loader_busy() && !nm_loader_start(test_job, NULL, "x"));
    unsigned frames = 0;
    uint64_t worst_cpu = 0, frame = 0;
    while (nm_loader_busy()) {
        assert(++frames < 2000);
        frame += FRAME_US;
        shim_clock_us = frame + WORK_US;            /* the update */
        const uint64_t past = idle(frame + WORK_US + IDLE_US);
        if (past > worst_cpu) worst_cpu = past;
        assert(past <= CPU_US);
        if (shim_clock_us < frame + FRAME_US) shim_clock_us = frame + FRAME_US;
    }
    assert(nm_loader_result() == 1 && job_done && open_calls == 2 && !main_thread_reads && !outside_slice);
    nm_loader_report();
    /* Reads alone: about 1.5 MB at 7.4 bytes/us is 200 ms, plus the calls; the CPU 120 ms; at 13 ms a frame. */
    assert(frames >= 25 && frames <= 60);
    /* A read outside a job passes straight through. */
    uint8_t one[16];
    file_at = 0; assert(__wrap_fs_read(3, one, sizeof(one)) == (ssize_t)sizeof(one) && main_thread_reads == 1);
    /* A credit while it loads: two slices, then the rest at once. */
    job_done = 0;
    assert(nm_loader_start(test_job, (void *)(uintptr_t)FILE_BYTES, "credit"));
    for (unsigned i = 0; i < 2; ++i) { frame += FRAME_US; shim_clock_us = frame; (void)idle(frame + IDLE_US); }
    assert(nm_loader_busy() && !job_done);
    slice_end_us = UINT64_MAX;
    assert(nm_loader_finish() == 1 && job_done && !nm_loader_busy());
    slice_end_us = 0;
    /* Idle with no job, or a slice already over: nothing runs. */
    nm_loader_idle(shim_clock_us + 10000u);
    assert(nm_loader_start(test_job, (void *)(uintptr_t)1000, "late"));
    const uint64_t before = shim_clock_us;
    nm_loader_idle(shim_clock_us);  /* no time left */
    assert(shim_clock_us == before && nm_loader_busy());
    slice_end_us = UINT64_MAX;
    assert(nm_loader_finish() == 1);
    /* A fitted cost that says no piece fits any slice (a slow host call) still reads: a piece a slice. */
    job_done = 0;
    assert(nm_loader_start(test_job, (void *)(uintptr_t)FILE_BYTES, "poisoned fit"));
    unsigned poisoned = 0;
    for (; nm_loader_busy() && poisoned < 2000; ++poisoned) {
        call_us = 1e6f; byte_us = 1e3f; /* as if every call took a second */
        frame += FRAME_US; shim_clock_us = frame; (void)idle(frame + IDLE_US);
        if (shim_clock_us < frame + FRAME_US) shim_clock_us = frame + FRAME_US;
    }
    assert(!nm_loader_busy() && nm_loader_result() == 1 && job_done);
    call_us = 735.0f; byte_us = 0.101f;
    printf("PASS: scene loader: %u frames for 1.7 MB and 120 ms of work, reads cut to their slices (none past them), "
           "yield points within one step (%lu us at most), strict alternation, the rest at once, idle without a job\n",
           frames, (unsigned long)worst_cpu);
    return 0;
}
