/* What the game keeps between games, and the VMU (include/game_save.h). */
#include "game_save.h"
#include <stdio.h>
#include <string.h>
#include "nm_mem.h"

static nm_save_t live;
static const nm_race_record_seed_t (*record_seeds)[NM_SAVE_SLOTS];
static int use_card;
/* Slots of each table inserted since the last request, kept in step with the insertions' shifts. */
static uint32_t inserted[NM_SAVE_TABLES];
static nm_game_save_status_t status={.boot=NM_SAVE_NO_CARD,.last=NM_SAVE_NO_FILE};

/* Requests for the writer (under `lock` on the console): the newest snapshot, the inserted records not yet on a
 * card, and whether the records were cleared since the last write. */
#define QUEUE_INSERTS 64
static uint8_t next_file[NM_SAVE_FILE_BYTES];
static int next_ready,next_replace,writer_running;
/* The settings changed since the last write (a merge keeps the card's settings otherwise): set count, written. */
static unsigned settings_sets,settings_written;
static nm_save_insert_t queue[QUEUE_INSERTS];
static unsigned queued;
static nm_settings_t next_settings;

#ifdef _arch_dreamcast
#include <kos/mutex.h>
#include <kos/thread.h>
#include <dc/maple.h>
#include <dc/vmufs.h>
#include <stdlib.h>
static mutex_t lock=MUTEX_INITIALIZER;
#define LOCK() mutex_lock(&lock)
#define UNLOCK() mutex_unlock(&lock)

/* The first VMU on the bus (controller A's slot 1 when it holds one). */
static void *vmu_find(void *context) { (void)context;return maple_enum_type(0,MAPLE_FUNC_MEMCARD); }
static int vmu_free_blocks(void *context,void *card) { (void)context;return vmufs_free_blocks(card); }
static int vmu_file_blocks(void *context,void *card,const char *name) {
    (void)context;
    vmu_dir_t *entries=NULL;int count=0;
    if(vmufs_readdir(card,&entries,&count)<0) return -1;
    int blocks=0;
    for(int i=0;i<count;++i)
        if(entries[i].filetype && !strncmp(entries[i].filename,name,sizeof(entries[i].filename)))
            blocks=entries[i].filesize ? entries[i].filesize : 1;
    free(entries);return blocks;
}
static int vmu_read(void *context,void *card,const char *name,uint8_t **data,size_t *size) {
    (void)context;
    void *buffer=NULL;int bytes=0;
    if(vmufs_read(card,name,&buffer,&bytes)<0 || !buffer || bytes<0) return -1;
    *data=buffer;*size=(size_t)bytes;return 0;
}
static void vmu_release(void *context,uint8_t *data) { (void)context;free(data); }
static int vmu_write(void *context,void *card,const char *name,const uint8_t *data,size_t size) {
    (void)context;return vmufs_write(card,name,(void *)data,(int)size,VMUFS_OVERWRITE);
}
static const nm_save_device_t vmu={NULL,vmu_find,vmu_free_blocks,vmu_file_blocks,vmu_read,vmu_release,vmu_write};
static const nm_save_device_t *device=&vmu;
#else
/* Hosts: no lock, the writer runs inside the request, and the card is whatever the test sets (none by default). */
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
static void *no_card(void *context) { (void)context;return NULL; }
static const nm_save_device_t none={NULL,no_card,NULL,NULL,NULL,NULL,NULL};
static const nm_save_device_t *device=&none;
void nm_game_save_device_set(const nm_save_device_t *card) { device=card ? card : &none; }
#endif

static uint8_t work_file[NM_SAVE_FILE_BYTES],scratch[NM_SAVE_FILE_BLOCKS*512u];
static nm_save_insert_t work_queue[QUEUE_INSERTS];
/* The writer: takes the newest request, writes it, repeats until none is left. */
static void *writer(void *unused) {
    (void)unused;
    for(;;) {
        LOCK();
        if(!next_ready) { writer_running=0;UNLOCK();return NULL; }
        nm_memcpy(work_file,next_file,sizeof(work_file));
        const unsigned count=queued;nm_memcpy(work_queue,queue,count*sizeof(*queue));
        const int replace=next_replace;const nm_settings_t settings=next_settings;
        const unsigned sets=settings_sets,changed=sets!=settings_written;
        next_ready=0;
        UNLOCK();
        const nm_save_result_t r=nm_save_store_write(device,work_file,work_queue,replace ? 0 : count,
            changed ? &settings : NULL,!replace,scratch);
        LOCK();
        if(r==NM_SAVE_OK) {
            nm_memmove(queue,queue+count,(queued-count)*sizeof(*queue));queued-=count;
            if(replace) next_replace=0;
            settings_written=sets;
        }
        status.last=r;
        UNLOCK();
        printf("Native save: %s (%u new records)\n",nm_save_result_name(r),count);
    }
}

static int writer_start(void) {
#ifdef _arch_dreamcast
    const kthread_attr_t attributes={.create_detached=true,.stack_size=16384,.label="vmu save"};
    return thd_create_ex(&attributes,writer,NULL)!=NULL;
#else
    writer(NULL);return 1;
#endif
}

void nm_game_save_boot(const nm_race_record_seed_t seeds[NM_SAVE_TABLES][NM_SAVE_SLOTS],int card) {
    record_seeds=seeds;use_card=card;
    nm_save_defaults(&live,seeds);
    nm_memset(inserted,0,sizeof(inserted));
    queued=0;next_ready=next_replace=0;settings_sets=settings_written=0;
    status=(nm_game_save_status_t){.boot=use_card ? nm_save_store_load(device,&live) : NM_SAVE_NO_CARD,
        .last=NM_SAVE_NO_FILE};
    printf("Native save: boot %s%s; difficulty %s, advertise sound %s\n",nm_save_result_name(status.boot),
        use_card ? "" : " (scripted: defaults, never written)",
        nm_settings_difficulty_name(live.settings.difficulty),nm_settings_advertise_name(live.settings.advertise_sound));
}

const nm_settings_t *nm_game_settings(void) { return &live.settings; }

int nm_game_settings_set(const nm_settings_t *value) {
    if(!nm_settings_valid(value)) return 0;
    if(memcmp(value,&live.settings,sizeof(*value))) { LOCK();++settings_sets;UNLOCK(); }
    live.settings=*value;return 1;
}

nm_race_record_t *nm_game_records(unsigned course) { return live.records[nm_save_table(course)]; }

void nm_game_record_inserted(unsigned course,unsigned slot) {
    if(slot>=NM_SAVE_SLOTS) return;
    const unsigned t=nm_save_table(course);
    /* The insertion moved slots slot..22 one down and dropped slot 23. */
    const uint32_t below=inserted[t]&((UINT32_C(1)<<slot)-1u);
    inserted[t]=(below|((inserted[t]>>slot)<<(slot+1))|(UINT32_C(1)<<slot))&((UINT32_C(1)<<NM_SAVE_SLOTS)-1u);
    live.selected[t]=(uint8_t)slot;
}

void nm_game_records_clear(void) {
    if(!record_seeds) return;
    const nm_settings_t settings=live.settings;
    nm_save_defaults(&live,record_seeds);live.settings=settings;
    nm_memset(inserted,0,sizeof(inserted));
    LOCK();queued=0;next_replace=1;UNLOCK();
}

void nm_game_save_request(void) {
    if(!use_card) return;
    LOCK();
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) {
        for(unsigned slot=0;slot<NM_SAVE_SLOTS;++slot) if(inserted[t]>>slot&1u) {
            /* Full (64 races without a card): the snapshot still has them, a card's own save would not. */
            if(queued<QUEUE_INSERTS) queue[queued++]=(nm_save_insert_t){.table=(uint8_t)t,.record=live.records[t][slot]};
        }
        inserted[t]=0;
    }
    next_settings=live.settings;
    next_ready=nm_save_file_build(&live,next_file)==NM_SAVE_FILE_BYTES;
    const int start=next_ready && !writer_running;
    if(start) writer_running=1;
    UNLOCK();
    if(start && !writer_start()) { LOCK();writer_running=0;status.last=NM_SAVE_IO_ERROR;UNLOCK(); }
}

nm_game_save_status_t nm_game_save_status(void) {
    LOCK();
    nm_game_save_status_t s=status;s.busy=writer_running;
    UNLOCK();
    return s;
}
