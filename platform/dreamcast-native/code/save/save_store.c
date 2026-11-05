/* Loading and writing the save file on a memory card (include/save_store.h). */
#include "save_store.h"
#include <string.h>
#include "nm_mem.h"

const char *nm_save_result_name(nm_save_result_t result) {
    switch(result) {
    case NM_SAVE_OK: return "ok";
    case NM_SAVE_NO_CARD: return "no VMU";
    case NM_SAVE_NO_FILE: return "no save file";
    case NM_SAVE_DAMAGED: return "save file damaged";
    case NM_SAVE_FULL: return "VMU full";
    case NM_SAVE_IO_ERROR: return "VMU error";
    }
    return "?";
}

/* The card's file parsed into *save (unchanged unless NM_SAVE_OK). */
static nm_save_result_t read_card(const nm_save_device_t *d,void *card,nm_save_t *save) {
    const int blocks=d->file_blocks(d->context,card,NM_SAVE_FILE_NAME);
    if(blocks<0) return NM_SAVE_IO_ERROR;
    if(!blocks) return NM_SAVE_NO_FILE;
    uint8_t *data=NULL;size_t size=0;
    if(d->read(d->context,card,NM_SAVE_FILE_NAME,&data,&size)<0 || !data) return NM_SAVE_IO_ERROR;
    const int ok=nm_save_file_parse(data,size,save);
    d->release(d->context,data);
    return ok ? NM_SAVE_OK : NM_SAVE_DAMAGED;
}

nm_save_result_t nm_save_store_load(const nm_save_device_t *d,nm_save_t *save) {
    void *card=d->find(d->context);
    if(!card) return NM_SAVE_NO_CARD;
    return read_card(d,card,save);
}

unsigned nm_save_insert(nm_race_record_t table[NM_SAVE_SLOTS],const nm_race_record_t *record) {
    if(record->time>table[NM_SAVE_SLOTS-1].time) return NM_SAVE_NO_SLOT;
    const uint32_t last_rank=table[NM_SAVE_SLOTS-1].rank;
    table[NM_SAVE_SLOTS-1]=*record;table[NM_SAVE_SLOTS-1].rank=last_rank;
    unsigned at=NM_SAVE_SLOTS-1;
    while(at && table[at].time<=table[at-1].time) {
        const nm_race_record_t old=table[at-1];
        const uint32_t rank=table[at].rank;
        table[at-1]=table[at];table[at-1].rank=old.rank;
        table[at]=old;table[at].rank=rank;
        --at;
    }
    return at;
}

nm_save_result_t nm_save_store_write(const nm_save_device_t *d,const uint8_t *snapshot,
        const nm_save_insert_t *inserts,unsigned count,const nm_settings_t *settings,int merge,uint8_t *scratch) {
    void *card=d->find(d->context);
    if(!card) return NM_SAVE_NO_CARD;
    const int existing=d->file_blocks(d->context,card,NM_SAVE_FILE_NAME);
    const int free_blocks=d->free_blocks(d->context,card);
    if(existing<0 || free_blocks<0) return NM_SAVE_IO_ERROR;
    if(free_blocks+existing<(int)NM_SAVE_FILE_BLOCKS) return NM_SAVE_FULL;
    const uint8_t *file=snapshot;
    static nm_save_t base;
    if(existing && merge) {
        const nm_save_result_t r=read_card(d,card,&base);
        if(r==NM_SAVE_IO_ERROR) return r;
        if(r==NM_SAVE_OK) {
            for(unsigned i=0;i<count;++i) if(inserts[i].table<NM_SAVE_TABLES) {
                const unsigned slot=nm_save_insert(base.records[inserts[i].table],&inserts[i].record);
                if(slot!=NM_SAVE_NO_SLOT) base.selected[inserts[i].table]=(uint8_t)slot;
            }
            if(settings && nm_settings_valid(settings)) base.settings=*settings;
            if(nm_save_file_build(&base,scratch)!=NM_SAVE_FILE_BYTES) return NM_SAVE_IO_ERROR;
            file=scratch;
        }
    }
    /* Whole blocks: the tail after the file is zero. */
    if(file!=scratch) nm_memcpy(scratch,file,NM_SAVE_FILE_BYTES);
    nm_memset(scratch+NM_SAVE_FILE_BYTES,0,NM_SAVE_FILE_BLOCKS*512u-NM_SAVE_FILE_BYTES);
    return d->write(d->context,card,NM_SAVE_FILE_NAME,scratch,NM_SAVE_FILE_BLOCKS*512u)<0 ?
        NM_SAVE_IO_ERROR : NM_SAVE_OK;
}
