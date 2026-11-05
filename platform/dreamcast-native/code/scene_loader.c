/* A scene's load in the idle waits of the screen before it (include/scene_loader.h). Outside the LTO unit
 * (Makefile): its code and placement do not touch the race's. */
#include "scene_loader.h"
#include "nm_mem.h"
#include <stdio.h>

#ifdef _arch_dreamcast
#include <arch/timer.h>
#include <kos/fs.h>
#include <kos/sem.h>
#include <kos/thread.h>
#include <stddef.h>
#include <string.h>

#define LOADER_STACK (64u * 1024u)   /* as the updater's own (THD_KERNEL_STACK_SIZE) */
#define PIECE_MIN 2048u              /* the smallest read piece worth a call */
#define PIECE_MAX (32u * 1024u)      /* the largest read piece: about 3 ms at 10 MB/s, 9 ms at the 3.8 MB/s seen once */
#define OPEN_CALLS 2.0f              /* an open costs about two read calls over /pc */

static uint8_t stack[LOADER_STACK] __attribute__((aligned(32)));
static kthread_t *thread;
static semaphore_t go, back;
static volatile int state;           /* 0 none, 1 running, 2 finished */
#ifdef NATIVE_DISC
/* From a disc a read blocks only the thread that makes it (KOS cdrom.c: the GD-ROM's DMA ends in a semaphore wait;
 * fs_iso9660 holds only its own mutex), and a seek takes tens to hundreds of milliseconds, more than any slice. So
 * the job's reads run outside its slices: the job hands the slice back, reads while the updater goes on with its
 * frames, and waits for the next slice to go on; the updater gives no slice while the read runs. The job's own work
 * still runs only inside slices. (Flycast, attract load in the title: 3 late frames, longest 26.9 ms, against 164
 * and 435 ms with the slices' reads; one read a frame then paces the load: 8.4 MB, 1395 reads, 26 s there.) */
static volatile int reading;
#define DISC_PIECE (32u * 1024u)     /* the largest call: the file system's mutex is free between them */
static uint8_t bounce[DISC_PIECE] __attribute__((aligned(32))); /* reads to a buffer that is not 32-byte aligned */
#endif
static nm_loader_job_t job;
static void *job_arg;
static const char *job_name = "";
static int result;
static uint64_t deadline;            /* this slice's end, us */
static uint64_t slice_begin, started_us, finished_us, slice_total_us;
static uint32_t slices, slice_max_us, overruns, reads, opens, handbacks;
static uint32_t read_max_us, read_max_bytes, open_max_us; /* the longest single file calls (dcload holds the CPU in them) */
static uint32_t other_max_us; static const char *other_max_site = ""; /* the longest write (a report line), seek or close */
static uint64_t bytes;
/* The fitted read cost (least squares of took = call + bytes * byte, older reads fading 0.97, as sound.c), kept
 * across jobs; the start values are the hardware's /pc of 18 October (the attract's loads: 0.74 ms a call, 9.9 MB/s). */
static float call_us = 735.0f, byte_us = 0.101f;
static double fit_n, fit_x, fit_y, fit_xx, fit_xy;

/* The longest stretches of the job between two of its points (yield points, file calls), with their sites. */
#define STRETCHES 4
static struct { uint32_t us; const char *from, *to; } stretch[STRETCHES];
static const char *point_site = "start";
static uint64_t point_us;
/* The job's time by the point it ran from (the stage between that point and the next), for the breakdown. */
#define SITES 64
static struct { const char *from; uint64_t us; uint32_t stretches; } site_total[SITES];
static unsigned sites;
/* A yield point without a cost of its own hands back when less time is left than the job's recent stretches between
 * two points take (a fading mean, times two; 0.5..4 ms): the next stretch would most likely pass the slice's end. */
static float stretch_mean_us = 1000.0f;

static uint64_t now_us(void) { return timer_us_gettime64(); }

/* The job passes a point: the stretch since the last one (time in other threads' frames left out). */
static void point(const char *site) {
    const uint64_t t = now_us();
    const uint32_t us = (uint32_t)(t - point_us);
    stretch_mean_us = 0.9f * stretch_mean_us + 0.1f * (float)(us < 20000u ? us : 20000u);
    {
        unsigned i = 0;
        while (i < sites && site_total[i].from != point_site) ++i;
        if (i == sites && sites < SITES) { site_total[sites].from = point_site; site_total[sites].us = 0; site_total[sites].stretches = 0; ++sites; }
        if (i < SITES) { site_total[i].us += us; ++site_total[i].stretches; }
    }
    unsigned at = STRETCHES;
    for (unsigned i = 0; i < STRETCHES; ++i) if (stretch[i].from == point_site && stretch[i].to == site) { at = i; break; }
    if (at < STRETCHES) { if (us > stretch[at].us) stretch[at].us = us; }
    else {
        unsigned low = 0;
        for (unsigned i = 1; i < STRETCHES; ++i) if (stretch[i].us < stretch[low].us) low = i;
        if (us > stretch[low].us) { stretch[low].us = us; stretch[low].from = point_site; stretch[low].to = site; }
    }
    point_site = site; point_us = t;
}
static int in_job(void) { return thread && thd_current == thread; }

static void slice_end(void) {
    const uint64_t t = now_us();
    const uint32_t took = (uint32_t)(t - slice_begin);
    slice_total_us += took;
    if (took > slice_max_us) slice_max_us = took;
    if (deadline != UINT64_MAX && t > deadline + 500u) ++overruns;
}

/* In the job: the updater's thread takes over until the next slice. */
static void hand_back(void) {
    slice_end();
    ++handbacks;
    sem_signal(&back);
    sem_wait(&go);
    slice_begin = point_us = now_us();
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        sem_wait(&go);
        slice_begin = point_us = now_us();
        point_site = "start";
        result = job(job_arg);
        point("end");
        finished_us = now_us();
        slice_end();
        state = 2;
        sem_signal(&back);
    }
    return NULL;
}

int nm_loader_init(void) {
    if (thread) return 1;
    sem_init(&go, 0);
    sem_init(&back, 0);
    const kthread_attr_t attr = {.stack_size = LOADER_STACK, .stack_ptr = stack, .prio = PRIO_DEFAULT + 1,
                                 .label = "scene loader"};
    thread = thd_create_ex(&attr, worker, NULL);
    if (!thread) { puts("Native scene loader: no thread; scene loads run at once"); return 0; }
    return 1;
}

int nm_loader_start(nm_loader_job_t j, void *arg, const char *name) {
    if (state == 1 || !j) return 0;
    job = j; job_arg = arg; job_name = name ? name : "";
    slices = slice_max_us = overruns = reads = opens = handbacks = 0;
    read_max_us = read_max_bytes = open_max_us = other_max_us = 0; other_max_site = "";
    for (unsigned i = 0; i < STRETCHES; ++i) stretch[i].us = 0, stretch[i].from = stretch[i].to = NULL;
    sites = 0;
    bytes = slice_total_us = 0;
    started_us = now_us();
    if (!thread) {
        result = j(arg);
        finished_us = now_us();
        state = 2;
        return 1;
    }
    state = 1;
    return 1;
}

int nm_loader_busy(void) { return state == 1; }
int nm_loader_result(void) { return state == 2 ? result : 0; }

void nm_loader_idle(uint64_t until) {
    if (state != 1 || !thread || in_job() || now_us() >= until) return;
#ifdef NATIVE_DISC
    if (reading) return;
#endif
    deadline = until;
    ++slices;
    sem_signal(&go);
    sem_wait(&back);
}

int nm_loader_finish(void) {
    while (state == 1 && thread && !in_job()) {
#ifdef NATIVE_DISC
        if (reading) { thd_sleep(1); continue; }
#endif
        deadline = UINT64_MAX;
        ++slices;
        sem_signal(&go);
        sem_wait(&back);
    }
    return nm_loader_result();
}

void nm_loader_yield_at(uint32_t cost_us, const char *site) {
    if (!in_job()) return;
    point(site);
    if (!cost_us) {
        const float reserve = 2.0f * stretch_mean_us;
        cost_us = reserve < 500.0f ? 500u : reserve > 4000.0f ? 4000u : (uint32_t)reserve;
    }
    /* Once: a new slice is taken as it comes (one shorter than the cost would otherwise hold the job forever). */
    if (now_us() + cost_us > deadline) hand_back();
}

void nm_loader_report(void) {
    if (!state) return;
    printf("Native scene load %s: %s in %lu ms; %lu slices (%lu ms in them, longest %lu us, %lu past their end), "
           "%lu reads of %lu bytes, %lu opens; read cost %lu us a call + %lu bytes/ms\n", job_name,
           state == 2 ? (result ? "done" : "FAILED") : "running",
           (unsigned long)(((state == 2 ? finished_us : now_us()) - started_us) / 1000u), (unsigned long)slices,
           (unsigned long)(slice_total_us / 1000u), (unsigned long)slice_max_us, (unsigned long)overruns,
           (unsigned long)reads, (unsigned long)bytes, (unsigned long)opens, (unsigned long)call_us,
           (unsigned long)(byte_us > 0.0f ? 1000.0f / byte_us : 0.0f));
    printf("Native scene load %s: longest file calls: a read of %lu bytes in %lu us, an open in %lu us, %s in %lu us\n",
           job_name, (unsigned long)read_max_bytes, (unsigned long)read_max_us, (unsigned long)open_max_us,
           other_max_site[0] ? other_max_site : "no other", (unsigned long)other_max_us);
    for (unsigned i = 0; i < STRETCHES; ++i)
        if (stretch[i].from)
            printf("Native scene load %s: %lu us from %s to %s without a yield point\n", job_name, (unsigned long)stretch[i].us,
                   stretch[i].from, stretch[i].to);
    /* The eight stages the job spent most of its time in (by the point each ran from). */
    for (unsigned n = 0; n < 8; ++n) {
        unsigned best = SITES;
        for (unsigned i = 0; i < sites; ++i) if (site_total[i].us && (best == SITES || site_total[i].us > site_total[best].us)) best = i;
        if (best == SITES) break;
        printf("Native scene load %s: %lu ms in %lu stretches from %s\n", job_name, (unsigned long)(site_total[best].us / 1000u),
               (unsigned long)site_total[best].stretches, site_total[best].from);
        site_total[best].us = 0;
    }
}

static void fit(uint32_t n, uint32_t took) {
    const double x = n, y = took;
    fit_n = 0.97 * fit_n + 1.0; fit_x = 0.97 * fit_x + x; fit_y = 0.97 * fit_y + y;
    fit_xx = 0.97 * fit_xx + x * x; fit_xy = 0.97 * fit_xy + x * y;
    const double var = fit_n * fit_xx - fit_x * fit_x;
    if (fit_n >= 3.0 && var > 1e3 * fit_n * fit_n) {
        const double b = (fit_n * fit_xy - fit_x * fit_y) / var, a = (fit_y - b * fit_x) / fit_n;
        /* Within what /pc has ever plausibly cost (one slow host call must not make every piece look too large for
         * any slice; the pieces then still progress, see fs_read): 0.1..8 ms a call, 0.5..20 MB/s. */
        if (b > 0.0 && a > 0.0) {
            byte_us = b < 0.05 ? 0.05f : b > 2.0 ? 2.0f : (float)b;
            call_us = a < 100.0 ? 100.0f : a > 8000.0 ? 8000.0f : (float)a;
        }
    }
}

/* KOS's file calls (newlib's read and open go through them), at link time (local.cfg.mk --wrap). */
ssize_t __real_fs_read(file_t hnd, void *buffer, size_t cnt);
ssize_t __wrap_fs_read(file_t hnd, void *buffer, size_t cnt);
#ifdef NATIVE_DISC
/* The read itself, in pieces; a piece to an unaligned buffer goes through the aligned bounce buffer, so the file
 * system reads whole sectors by DMA and does not take them one at a time through its sector cache. */
static ssize_t disc_read(file_t hnd, void *buffer, size_t cnt) {
    size_t done = 0;
    while (done < cnt) {
        size_t piece = cnt - done < DISC_PIECE ? cnt - done : DISC_PIECE;
        uint8_t *const to = (uint8_t *)buffer + done;
        const int direct = !((uintptr_t)to & 31u);
        const uint64_t t0 = now_us();
        const ssize_t r = __real_fs_read(hnd, direct ? to : bounce, piece);
        const uint32_t took = (uint32_t)(now_us() - t0);
        if (took > read_max_us) { read_max_us = took; read_max_bytes = (uint32_t)piece; }
        ++reads;
        if (r < 0) return done ? (ssize_t)done : r;
        if (!direct) nm_memcpy(to, bounce, (size_t)r);
        fit((uint32_t)piece, took);
        bytes += (uint64_t)r;
        done += (size_t)r;
        if ((size_t)r < piece) break;
    }
    return (ssize_t)done;
}
#endif
ssize_t __wrap_fs_read(file_t hnd, void *buffer, size_t cnt) {
    if (!in_job()) return __real_fs_read(hnd, buffer, cnt);
#ifdef NATIVE_DISC
    point("fs_read");
    if (deadline == UINT64_MAX) return disc_read(hnd, buffer, cnt); /* nm_loader_finish: nothing to give way to */
    reading = 1;
    slice_end();
    ++handbacks;
    sem_signal(&back);
    const ssize_t r = disc_read(hnd, buffer, cnt);
    reading = 0;
    sem_wait(&go);
    slice_begin = point_us = now_us();
    return r;
#endif
    size_t done = 0;
    int handed = 0; /* this piece waited for a slice once already */
    while (done < cnt) {
        const size_t left = cnt - done;
        size_t piece = PIECE_MAX;
        point("fs_read");
        if (deadline != UINT64_MAX) {
            const uint64_t t = now_us();
            const float room = t < deadline ? 0.9f * (float)(deadline - t) - call_us : 0.0f;
            const float fits = room > 0.0f ? room / byte_us : 0.0f;
            /* Once: the next slice reads at least PIECE_MIN as it comes. Waiting again whenever the fitted cost says no
             * piece fits a slice would never read again when it says so of every slice (a slow host call in the fit),
             * and nothing but a read corrects the fit: the job then stood still and the title waited for it forever. */
            if (fits < (float)PIECE_MIN && fits < (float)left && !handed) { hand_back(); handed = 1; continue; }
            piece = fits >= (float)PIECE_MAX ? PIECE_MAX : (size_t)fits & ~(size_t)511u;
            if (piece < PIECE_MIN) piece = PIECE_MIN;
        }
        handed = 0;
        if (piece > left) piece = left;
        const uint64_t t0 = now_us();
        const ssize_t r = __real_fs_read(hnd, (uint8_t *)buffer + done, piece);
        const uint32_t took = (uint32_t)(now_us() - t0);
        fit((uint32_t)piece, took);
        if (took > read_max_us) { read_max_us = took; read_max_bytes = (uint32_t)piece; }
        ++reads;
        if (r < 0) return done ? (ssize_t)done : r;
        bytes += (uint64_t)r;
        done += (size_t)r;
        if ((size_t)r < piece) break;
    }
    return (ssize_t)done;
}

file_t __real_fs_open(const char *fn, int mode);
file_t __wrap_fs_open(const char *fn, int mode);
file_t __wrap_fs_open(const char *fn, int mode) {
    if (!in_job()) return __real_fs_open(fn, mode);
    nm_loader_yield_at((uint32_t)(OPEN_CALLS * call_us), "fs_open"); ++opens;
    const uint64_t t0 = now_us();
    const file_t f = __real_fs_open(fn, mode);
    const uint32_t took = (uint32_t)(now_us() - t0);
    if (took > open_max_us) open_max_us = took;
    return f;
}

/* The job's other file calls (local.cfg.mk --wrap): a line of its report written to dcload's console, a seek, a
 * close are /pc calls too, a call's cost each: a yield point before each, so they wait for a slice with room. */
static void other_end(const char *site, uint64_t t0) {
    const uint32_t took = (uint32_t)(now_us() - t0);
    if (took > other_max_us) { other_max_us = took; other_max_site = site; }
}
ssize_t __real_fs_write(file_t hnd, const void *buffer, size_t cnt);
ssize_t __wrap_fs_write(file_t hnd, const void *buffer, size_t cnt);
ssize_t __wrap_fs_write(file_t hnd, const void *buffer, size_t cnt) {
    if (!in_job()) return __real_fs_write(hnd, buffer, cnt);
    nm_loader_yield_at((uint32_t)call_us, "fs_write");
    const uint64_t t0 = now_us();
    const ssize_t r = __real_fs_write(hnd, buffer, cnt);
    other_end("a write", t0);
    return r;
}
off_t __real_fs_seek(file_t hnd, off_t offset, int whence);
off_t __wrap_fs_seek(file_t hnd, off_t offset, int whence);
off_t __wrap_fs_seek(file_t hnd, off_t offset, int whence) {
    if (!in_job()) return __real_fs_seek(hnd, offset, whence);
    nm_loader_yield_at((uint32_t)call_us, "fs_seek");
    const uint64_t t0 = now_us();
    const off_t r = __real_fs_seek(hnd, offset, whence);
    other_end("a seek", t0);
    return r;
}
int __real_fs_close(file_t hnd);
int __wrap_fs_close(file_t hnd);
int __wrap_fs_close(file_t hnd) {
    if (!in_job()) return __real_fs_close(hnd);
    nm_loader_yield_at((uint32_t)call_us, "fs_close");
    const uint64_t t0 = now_us();
    const int r = __real_fs_close(hnd);
    other_end("a close", t0);
    return r;
}

int nm_loader_in_job(void) { return in_job(); }
#else
/* Host builds: the job runs at once. */
static int result, state;
int nm_loader_init(void) { return 0; }
int nm_loader_start(nm_loader_job_t job, void *arg, const char *name) {
    (void)name;
    if (!job) return 0;
    result = job(arg); state = 2; return 1;
}
int nm_loader_busy(void) { return 0; }
int nm_loader_result(void) { return state == 2 ? result : 0; }
void nm_loader_idle(uint64_t deadline_us) { (void)deadline_us; }
int nm_loader_finish(void) { return nm_loader_result(); }
void nm_loader_report(void) {}
int nm_loader_in_job(void) { return 0; }
#endif
