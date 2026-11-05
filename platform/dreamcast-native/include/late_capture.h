#ifndef DAYTONA_NATIVE_LATE_CAPTURE_H
#define DAYTONA_NATIVE_LATE_CAPTURE_H
#include <stdint.h>
#include <stdio.h>

/* NATIVE_DIAG_LATE_CAPTURE=N (measurement build, needs NATIVE_DIAG_DROP_LOG=1; local.cfg.mk): pictures of the
 * frames the drop log finds late, copied from the frame buffer into a pool of image slots allocated before any
 * asset (nm_late_reserve, like the sample profile's buffer: nothing is allocated while the race runs), and
 * written to <asset root><name>.bin and .csv when the run returns to the loader. tools/late_capture_report.py
 * turns them into PNGs with a caption bar and a contact sheet.
 *
 * Which frame, when and from which buffer: code/practice.c (drop log). This file holds the slot pool, the copy
 * from VRAM and the file writer; it reads no race state.
 *
 * Image: default 320x240 RGB565 (each pixel the mean of a 2x2 block of the 640x480 frame, rounded, then the top
 * 5/6/5 bits), 153,600 bytes; NATIVE_DIAG_LATE_CAPTURE_FULL=1: the frame buffer as it is, 640x480 packed
 * 24-bit (bytes B, G, R per pixel, PM_RGB888P), 921,600 bytes.
 *
 * Slots: NM_LATE_CAPTURE_FIRST (N) for the first N late frames that are eligible (not a load, not caused by a
 * capture), never evicted; NM_LATE_CAPTURE_SLOTS (N + spare) in all. Beyond the first N, a late frame is kept
 * when it is among the N worst eligible late frames so far (score: vblanks of its interval, then registration
 * plus render time); it takes a free slot, else a "before" picture's slot, else the slot of the least bad
 * kept frame beyond the first N if that one is less bad. "Before" pictures (the frame shown just before a late
 * one) only take free slots and give them up to late frames.
 *
 * File <name>.bin, little-endian: nm_late_header_t (128 bytes); the name tables (header.names_bytes: strings
 * separated by NUL, groups kinds, phases, scenes, causes, sources in that order, each group ended by an empty
 * string; padded to 4 bytes); header.images records (nm_late_record_t, 128 bytes each, in run order); the
 * images in the same order (header.image_bytes each). <name>.csv: one row per image, the same fields by name. */
enum { NM_LATE_FREE, NM_LATE_FIRST, NM_LATE_WORST, NM_LATE_BEFORE, NM_LATE_KINDS };
enum { NM_LATE_RGB565=1, NM_LATE_BGR888=2 };
enum { NM_LATE_SRC_W=640, NM_LATE_SRC_H=480 };
enum { NM_LATE_TORN=1u, NM_LATE_UNTIMED=2u, NM_LATE_COPIED=4u };   /* record flags */
#define NM_LATE_MAGIC UINT32_C(0x434c4d4e)      /* "NMLC" */

typedef struct {
    uint32_t kind,seq,pair_seq,run,tick,phase,scene,vbl,cause,upd_us,ticks,wait_us,reg_us,rnd_us;
    int32_t late_us;
    uint32_t other_us,prev_rnd_us;
    int32_t camera_mode;
    uint32_t camera_transition,copy_us,copy_scene,since_flip_us,source,flags,fb_addr,score,image,flip_vbl;
    uint32_t reserved[4];
} nm_late_record_t;

typedef struct {
    uint32_t magic,version,width,height,format,image_bytes,slots,first_n,images,names_bytes,period_ns;
    uint32_t late,eligible,not_kept,missed,copies,copy_us_total,copy_us_max,affected,torn,before_images,run_ticks;
    uint32_t evicted,reserved[9];
} nm_late_header_t;

typedef struct { const char *const *names; unsigned count; } nm_late_names_t;

#ifdef NM_DIAG_LATE_CAPTURE
#ifdef NM_LATE_CAPTURE_FULL_RES
enum { NM_LATE_W=640, NM_LATE_H=480, NM_LATE_FORMAT=NM_LATE_BGR888, NM_LATE_IMAGE_BYTES=640*480*3 };
#else
enum { NM_LATE_W=320, NM_LATE_H=240, NM_LATE_FORMAT=NM_LATE_RGB565, NM_LATE_IMAGE_BYTES=320*240*2 };
#endif
#ifndef NM_LATE_CAPTURE_FIRST
#define NM_LATE_CAPTURE_FIRST NM_DIAG_LATE_CAPTURE
#endif
#ifndef NM_LATE_CAPTURE_SLOTS
#define NM_LATE_CAPTURE_SLOTS NM_LATE_CAPTURE_FIRST
#endif
/* The pool, before any asset. Returns 0 (and the build captures nothing) if it is refused. */
int nm_late_reserve(void);
int nm_late_ready(void);
/* An eligible late frame: its slot, or -1 when it is not kept. */
int nm_late_take_late(uint32_t seq,uint32_t score);
/* A "before" picture for late frame pair: a free slot or -1. */
int nm_late_take_before(uint32_t seq,uint32_t pair);
void nm_late_release(int slot);
/* The slot of a kept picture of frame seq (kind NM_LATE_FIRST/WORST or BEFORE), or -1. */
int nm_late_find(uint32_t seq,unsigned before);
nm_late_record_t *nm_late_record(int slot);
/* Copy the frame buffer at fb_addr (the PVR's frame buffer address, 32-bit area) into the slot. */
void nm_late_copy(int slot,uint32_t fb_addr);
uint32_t nm_late_evicted(void);
/* Write <root><name>.bin and .csv; header counts filled by the caller; names: kinds, phases, scenes, causes,
 * sources. Returns 1 when both files were written. */
int nm_late_write(const char *root,const char *name,nm_late_header_t *header,const nm_late_names_t names[5]);
/* The 2x2 box of a 640x480 packed 24-bit frame (B, G, R bytes, read as little-endian words) into 320x240
 * RGB565. */
void nm_late_downscale(uint16_t *dst,const volatile uint32_t *src);
#ifndef __DREAMCAST__
/* Host tests: frame buffer addresses are offsets into this buffer. */
extern const uint8_t *nm_late_host_vram;
#endif
#endif
#endif
