/* NATIVE_DIAG_LATE_CAPTURE (include/late_capture.h): slot pool, frame buffer copy, files. Measurement builds
 * only; host tests compile this file with NM_DIAG_LATE_CAPTURE and a frame buffer in RAM. */
#include "late_capture.h"
#ifdef NM_DIAG_LATE_CAPTURE
#include <stdlib.h>
#include <string.h>
#include "heap.h"
#include "nm_mem.h"
#ifdef __DREAMCAST__
#include <dc/pvr.h>
/* The frame buffer through the 32-bit VRAM area, uncached (P2): as KOS's vram_l. */
static const volatile uint32_t *late_vram(uint32_t addr) {
    return (const volatile uint32_t *)(uintptr_t)(PVR_RAM_BASE|(addr&UINT32_C(0x00ffffff)));
}
#else
const uint8_t *nm_late_host_vram;
static const volatile uint32_t *late_vram(uint32_t addr) { return (const volatile uint32_t *)(const void *)(nm_late_host_vram+addr); }
#endif
_Static_assert(sizeof(nm_late_record_t)==128,"late capture record");
_Static_assert(sizeof(nm_late_header_t)==128,"late capture header");
_Static_assert(NM_LATE_CAPTURE_SLOTS>=NM_LATE_CAPTURE_FIRST && NM_LATE_CAPTURE_FIRST>=1,"late capture slots");

static uint8_t *late_pool;
static nm_late_record_t late_slots[NM_LATE_CAPTURE_SLOTS];
static uint32_t late_eligible,late_evicted;
/* The scores of the N worst eligible late frames so far, worst first (the first N included). */
static uint32_t late_top[NM_LATE_CAPTURE_FIRST];
static unsigned late_top_count;

int nm_late_reserve(void) {
    if(late_pool) return 1;
    const size_t bytes=(size_t)NM_LATE_CAPTURE_SLOTS*NM_LATE_IMAGE_BYTES;
    late_pool=nm_heap_alloc("late capture pool",32,bytes);
    if(!late_pool) { puts("Late capture: pool refused; capturing nothing");return 0; }
    nm_memset(late_pool,0,bytes);
    printf("Late capture: %u slots (first %u late frames kept) of %ux%u %s, %lu bytes, below every asset\n",
        (unsigned)NM_LATE_CAPTURE_SLOTS,(unsigned)NM_LATE_CAPTURE_FIRST,(unsigned)NM_LATE_W,(unsigned)NM_LATE_H,
        (int)NM_LATE_FORMAT==(int)NM_LATE_RGB565 ? "RGB565 (2x2 box)" : "packed 24-bit (B,G,R)",(unsigned long)bytes);
    return 1;
}
int nm_late_ready(void) { return late_pool!=NULL; }
uint32_t nm_late_evicted(void) { return late_evicted; }
nm_late_record_t *nm_late_record(int slot) { return slot>=0 && slot<NM_LATE_CAPTURE_SLOTS ? late_slots+slot : NULL; }

static int late_free_slot(void) {
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) if(late_slots[s].kind==NM_LATE_FREE) return s;
    return -1;
}
void nm_late_release(int slot) {
    if(slot>=0 && slot<NM_LATE_CAPTURE_SLOTS) nm_memset(late_slots+slot,0,sizeof(late_slots[slot]));
}
int nm_late_find(uint32_t seq,unsigned before) {
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) {
        const nm_late_record_t *r=late_slots+s;
        if(r->kind!=NM_LATE_FREE && r->seq==seq && (r->kind==NM_LATE_BEFORE)==(before!=0)) return s;
    }
    return -1;
}
static uint32_t late_score_of(uint32_t seq) {
    const int s=nm_late_find(seq,0);
    return s<0 ? 0 : late_slots[s].score;
}
/* A late picture leaves: its "before" picture goes with it. */
static void late_drop(int slot) {
    const uint32_t seq=late_slots[slot].seq;
    nm_late_release(slot);
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s)
        if(late_slots[s].kind==NM_LATE_BEFORE && late_slots[s].pair_seq==seq) nm_late_release(s);
}
/* The "before" picture whose late frame is least bad (or not decided yet), not that of frame keep. */
static int late_evict_before(uint32_t keep) {
    int best=-1;uint32_t best_score=0;
    for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s) {
        if(late_slots[s].kind!=NM_LATE_BEFORE || late_slots[s].pair_seq==keep) continue;
        const uint32_t score=late_score_of(late_slots[s].pair_seq);
        if(best<0 || score<best_score) { best=s;best_score=score; }
    }
    if(best>=0) { nm_late_release(best);++late_evicted; }
    return best;
}

int nm_late_take_late(uint32_t seq,uint32_t score) {
    if(!late_pool) return -1;
    const int first=late_eligible<NM_LATE_CAPTURE_FIRST;
    ++late_eligible;
    const int among=late_top_count<NM_LATE_CAPTURE_FIRST || score>late_top[NM_LATE_CAPTURE_FIRST-1];
    if(among) {
        unsigned at=late_top_count<NM_LATE_CAPTURE_FIRST ? late_top_count++ : NM_LATE_CAPTURE_FIRST-1;
        while(at && late_top[at-1]<score) { late_top[at]=late_top[at-1];--at; }
        late_top[at]=score;
    }
    if(!first && !among) return -1;
    int slot=late_free_slot();
    if(slot<0) slot=late_evict_before(seq);
    if(slot<0) {
        /* Least bad picture beyond the first N; a first-N frame may take it whatever its score (the slots
         * hold at least N pictures, so this happens only once the first N are in and some are evicted). */
        int least=-1;
        for(int s=0;s<NM_LATE_CAPTURE_SLOTS;++s)
            if(late_slots[s].kind==NM_LATE_WORST && (least<0 || late_slots[s].score<late_slots[least].score)) least=s;
        if(least>=0 && (first || late_slots[least].score<score)) { late_drop(least);++late_evicted;slot=least; }
    }
    if(slot<0) return -1;
    nm_late_record_t *r=late_slots+slot;
    nm_memset(r,0,sizeof(*r));
    r->kind=first ? NM_LATE_FIRST : NM_LATE_WORST;r->seq=r->pair_seq=seq;r->score=score;
    return slot;
}
int nm_late_take_before(uint32_t seq,uint32_t pair) {
    if(!late_pool) return -1;
    const int slot=late_free_slot();
    if(slot<0) return -1;
    nm_late_record_t *r=late_slots+slot;
    nm_memset(r,0,sizeof(*r));
    r->kind=NM_LATE_BEFORE;r->seq=seq;r->pair_seq=pair;
    return slot;
}

static inline uint16_t late_565(uint32_t r,uint32_t g,uint32_t b) {
    r=(r+2)>>2;g=(g+2)>>2;b=(b+2)>>2;
    return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}
/* Four pixels are three words: [B0 G0 R0 B1] [G1 R1 B2 G2] [R2 B3 G3 R3] (little-endian). Each word of
 * the frame is read once, as a 32-bit load (the uncached VRAM read is what costs). */
void nm_late_downscale(uint16_t *dst,const volatile uint32_t *src) {
    for(unsigned y=0;y<NM_LATE_SRC_H/2;++y) {
        const volatile uint32_t *a=src+y*2u*(NM_LATE_SRC_W*3u/4u),*b=a+NM_LATE_SRC_W*3u/4u;
        uint16_t *out=dst+y*(NM_LATE_SRC_W/2u);
        for(unsigned x=0;x<NM_LATE_SRC_W/4u;++x,a+=3,b+=3,out+=2) {
            const uint32_t a0=a[0],a1=a[1],a2=a[2],b0=b[0],b1=b[1],b2=b[2];
            const uint32_t blue01=(a0&255u)+(a0>>24)+(b0&255u)+(b0>>24);
            const uint32_t green01=((a0>>8)&255u)+(a1&255u)+((b0>>8)&255u)+(b1&255u);
            const uint32_t red01=((a0>>16)&255u)+((a1>>8)&255u)+((b0>>16)&255u)+((b1>>8)&255u);
            const uint32_t blue23=((a1>>16)&255u)+((a2>>8)&255u)+((b1>>16)&255u)+((b2>>8)&255u);
            const uint32_t green23=(a1>>24)+((a2>>16)&255u)+(b1>>24)+((b2>>16)&255u);
            const uint32_t red23=(a2&255u)+(a2>>24)+(b2&255u)+(b2>>24);
            out[0]=late_565(red01,green01,blue01);out[1]=late_565(red23,green23,blue23);
        }
    }
}
void nm_late_copy(int slot,uint32_t fb_addr) {
    if(!late_pool || slot<0 || slot>=NM_LATE_CAPTURE_SLOTS) return;
    void *dst=late_pool+(size_t)slot*NM_LATE_IMAGE_BYTES;
    const volatile uint32_t *src=late_vram(fb_addr);
#ifdef NM_LATE_CAPTURE_FULL_RES
    uint32_t *out=dst;
    for(unsigned i=0;i<NM_LATE_IMAGE_BYTES/4u;i+=8,src+=8,out+=8) {
        const uint32_t w0=src[0],w1=src[1],w2=src[2],w3=src[3],w4=src[4],w5=src[5],w6=src[6],w7=src[7];
        out[0]=w0;out[1]=w1;out[2]=w2;out[3]=w3;out[4]=w4;out[5]=w5;out[6]=w6;out[7]=w7;
    }
#else
    nm_late_downscale(dst,src);
#endif
    late_slots[slot].fb_addr=fb_addr;late_slots[slot].flags|=NM_LATE_COPIED;
}

static const char *late_name(const nm_late_names_t *names,uint32_t value) {
    return value<names->count && names->names[value] ? names->names[value] : "?";
}
static int late_order(const void *a,const void *b) {
    const nm_late_record_t *x=*(const nm_late_record_t *const *)a,*y=*(const nm_late_record_t *const *)b;
    if(x->seq!=y->seq) return x->seq<y->seq ? -1 : 1;
    return (x->kind==NM_LATE_BEFORE)-(y->kind==NM_LATE_BEFORE);
}
int nm_late_write(const char *root,const char *name,nm_late_header_t *header,const nm_late_names_t names[5]) {
    if(!late_pool) return 0;
    nm_late_record_t *order[NM_LATE_CAPTURE_SLOTS];unsigned images=0;
    for(unsigned s=0;s<NM_LATE_CAPTURE_SLOTS;++s) if(late_slots[s].kind!=NM_LATE_FREE && (late_slots[s].flags&NM_LATE_COPIED)) order[images++]=late_slots+s;
    qsort(order,images,sizeof(order[0]),late_order);
    for(unsigned i=0;i<images;++i) order[i]->image=i;
    char table[1024];size_t used=0;
    for(unsigned g=0;g<5;++g) {
        for(unsigned i=0;i<names[g].count;++i) {
            const char *text=names[g].names[i] ? names[g].names[i] : "?";const size_t n=strlen(text)+1;
            if(used+n+1>sizeof(table)) return 0;
            nm_memcpy(table+used,text,n);used+=n;
        }
        table[used++]=0;
    }
    while(used&3u) table[used++]=0;
    header->magic=NM_LATE_MAGIC;header->version=1;header->width=NM_LATE_W;header->height=NM_LATE_H;
    header->format=NM_LATE_FORMAT;header->image_bytes=NM_LATE_IMAGE_BYTES;header->slots=NM_LATE_CAPTURE_SLOTS;
    header->first_n=NM_LATE_CAPTURE_FIRST;header->images=images;header->names_bytes=(uint32_t)used;
    header->evicted=late_evicted;
    char path[256];int ok=0;
    if(snprintf(path,sizeof(path),"%s%s.bin",root,name)<(int)sizeof(path)) {
        FILE *file=fopen(path,"wb");
        if(file) {
            ok=fwrite(header,sizeof(*header),1,file)==1 && fwrite(table,1,used,file)==used;
            for(unsigned i=0;ok && i<images;++i) ok=fwrite(order[i],sizeof(*order[i]),1,file)==1;
            /* dcload transfers are slow and bounded per request: modest chunks. */
            for(unsigned i=0;ok && i<images;++i) {
                const uint8_t *pixels=late_pool+(size_t)(order[i]-late_slots)*NM_LATE_IMAGE_BYTES;
                for(uint32_t at=0;ok && at<NM_LATE_IMAGE_BYTES;at+=16384) {
                    const uint32_t chunk=NM_LATE_IMAGE_BYTES-at<16384 ? NM_LATE_IMAGE_BYTES-at : 16384;
                    ok=fwrite(pixels+at,1,chunk,file)==chunk;
                }
            }
            if(fclose(file)) ok=0;
        }
    }
    printf("Late capture: %u pictures %s %s\n",images,ok ? "written to" : "NOT written to",path);
    int csv_ok=0;
    if(snprintf(path,sizeof(path),"%s%s.csv",root,name)<(int)sizeof(path)) {
        FILE *file=fopen(path,"w");
        if(file) {
            csv_ok=fprintf(file,"image,kind,seq,pair_seq,run,tick,phase,scene,vbl,cause,upd_us,ticks,wait_us,reg_us,rnd_us,late_us,"
                "other_us,prev_rnd_us,camera_mode,camera_transition,copy_us,copy_scene,since_flip_us,source,torn,untimed,"
                "fb_addr,score,flip_vbl,offset\n")>0;
            const uint32_t images_at=(uint32_t)(sizeof(*header)+used+images*sizeof(nm_late_record_t));
            for(unsigned i=0;csv_ok && i<images;++i) {
                const nm_late_record_t *r=order[i];
                csv_ok=fprintf(file,"%u,%s,%lu,%lu,%lu,%lu,%s,%s,%lu,%s,%lu,%lu,%lu,%lu,%lu,%ld,%lu,%lu,%ld,%lu,%lu,%lu,%lu,%s,%u,%u,"
                    "%08lx,%lu,%lu,%lu\n",i,late_name(names+0,r->kind),(unsigned long)r->seq,(unsigned long)r->pair_seq,
                    (unsigned long)r->run,(unsigned long)r->tick,late_name(names+1,r->phase),late_name(names+2,r->scene),
                    (unsigned long)r->vbl,late_name(names+3,r->cause),(unsigned long)r->upd_us,(unsigned long)r->ticks,
                    (unsigned long)r->wait_us,(unsigned long)r->reg_us,(unsigned long)r->rnd_us,(long)r->late_us,
                    (unsigned long)r->other_us,(unsigned long)r->prev_rnd_us,(long)r->camera_mode,(unsigned long)r->camera_transition,
                    (unsigned long)r->copy_us,(unsigned long)r->copy_scene,(unsigned long)r->since_flip_us,late_name(names+4,r->source),
                    (unsigned)(r->flags&NM_LATE_TORN ? 1 : 0),(unsigned)(r->flags&NM_LATE_UNTIMED ? 1 : 0),(unsigned long)r->fb_addr,
                    (unsigned long)r->score,(unsigned long)r->flip_vbl,(unsigned long)(images_at+i*NM_LATE_IMAGE_BYTES))>0;
            }
            if(fclose(file)) csv_ok=0;
        }
    }
    printf("Late capture: index %s %s\n",csv_ok ? "written to" : "NOT written to",path);
    return ok && csv_ok;
}
#endif
