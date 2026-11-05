#include "sample_profile.h"
#ifdef NATIVE_SAMPLE_PROFILE
#include <arch/irq.h>
#include <arch/timer.h>
#include <kos/irq.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "heap.h"
#include "nm_mem.h"

volatile uint32_t nm_sample_tag;
static uint32_t *samples;
static volatile uint32_t sample_count,sample_dropped;

/* Interrupt context: acknowledge the underflow, store one word. No calls into
 * the allocator, stdio or the thread system. */
static void sample_interrupt(irq_t code,irq_context_t *context,void *data) {
    (void)code;(void)data;
    timer_clear(TMU1);
    const uint32_t count=sample_count;
    if(count>=NM_SAMPLE_CAPACITY) { ++sample_dropped;return; }
    const uint32_t pc=context->pc-UINT32_C(0x8c000000);
    samples[count]=(pc<UINT32_C(0x01000000) ? pc>>1 : UINT32_C(0x00800000))|nm_sample_tag;
    sample_count=count+1;
}

static int sample_running;
int nm_sample_reserve(void) {
    if(samples) return 1;
    samples=nm_heap_alloc("sample profile buffer",32,NM_SAMPLE_CAPACITY*sizeof(*samples));
    if(!samples) { puts("Sample profile: buffer refused; not sampling");return 0; }
    /* Touch every page now, not from the interrupt handler. */
    nm_memset(samples,0,NM_SAMPLE_CAPACITY*sizeof(*samples));
    return 1;
}

int nm_sample_start(void) {
    if(sample_running || !nm_sample_reserve()) return 0;
    sample_count=sample_dropped=0;
    if(irq_set_handler(EXC_TMU1_TUNI1,sample_interrupt,NULL)<0 ||
       timer_prime(TMU1,NM_SAMPLE_RATE_HZ,1)<0) {
        puts("Sample profile: TMU1 refused; not sampling");
        free(samples);samples=NULL;return 0;
    }
    timer_clear(TMU1);
    timer_start(TMU1);
    sample_running=1;
    printf("Sample profile: TMU1 at %u Hz, capacity %u samples (%u bytes)\n",(unsigned)NM_SAMPLE_RATE_HZ,
        (unsigned)NM_SAMPLE_CAPACITY,(unsigned)(NM_SAMPLE_CAPACITY*sizeof(*samples)));
    return 1;
}

void nm_sample_finish(const char *root) {
    if(!samples) return;
    if(!sample_running) { free(samples);samples=NULL;return; }
    sample_running=0;
    timer_stop(TMU1); /* Also masks its interrupt. */
    irq_set_handler(EXC_TMU1_TUNI1,NULL,NULL);
    const uint32_t count=sample_count<=NM_SAMPLE_CAPACITY ? sample_count : NM_SAMPLE_CAPACITY;
    char path[256];
    int written=0;
    if(root && snprintf(path,sizeof(path),"%ssample-profile.bin",root)<(int)sizeof(path)) {
        FILE *file=fopen(path,"wb");
        if(file) {
            const uint32_t header[5]={UINT32_C(0x50534d4e),1,NM_SAMPLE_RATE_HZ,count,sample_dropped};
            written=fwrite(header,sizeof(header),1,file)==1;
            /* dcload transfers are slow and bounded per request: modest chunks. */
            for(uint32_t at=0;written && at<count;at+=4096) {
                const uint32_t chunk=count-at<4096 ? count-at : 4096;
                written=fwrite(samples+at,sizeof(*samples),chunk,file)==chunk;
            }
            if(fclose(file)) written=0;
        }
    }
    printf("Sample profile: %lu samples, %lu dropped, %s %s\n",(unsigned long)count,
        (unsigned long)sample_dropped,written ? "written to" : "NOT written to",path);
    if(!written) {
        /* Fallback for a read-only or absent file server: run-length text of
         * the raw words, 8 per line, still parseable by the report tool. */
        puts("SAMPLE-PROFILE-TEXT begin");
        printf("SAMPLE-PROFILE-TEXT header %u %lu %lu\n",(unsigned)NM_SAMPLE_RATE_HZ,
            (unsigned long)count,(unsigned long)sample_dropped);
        for(uint32_t at=0;at<count;at+=8) {
            printf("SP");
            for(uint32_t i=at;i<count && i<at+8;++i) printf(" %08lx",(unsigned long)samples[i]);
            putchar('\n');
        }
        puts("SAMPLE-PROFILE-TEXT end");
    }
    free(samples);samples=NULL;
}
#else
typedef int nm_sample_profile_disabled;
#endif
