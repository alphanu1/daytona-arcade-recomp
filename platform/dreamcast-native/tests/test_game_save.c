/* Save data, VMS file, card store and the game's save state against a fake VMU (code/save/; no ROM data: the record
 * seeds here are synthetic, laid out as the generated race_record_data.h is).
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/test_game_save.c \
 *    code/save/save_data.c code/save/save_store.c code/save/game_save.c */
#undef NDEBUG
#include "game_save.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static nm_race_record_seed_t seeds[NM_SAVE_TABLES][NM_SAVE_SLOTS];
static void make_seeds(void) {
    static const uint32_t base[NM_SAVE_TABLES]={0x2800,0x3c00,0x3c00};
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) for(unsigned i=0;i<NM_SAVE_SLOTS;++i)
        seeds[t][i]=(nm_race_record_seed_t){.rank=i+1,.time=base[t]+320u*i,.flags=t,.name=0x00414141u+i+(t<<8)};
}

/* A fake VMU: 200 user blocks, one directory of files of whole blocks, faults on demand. */
#define CARD_FILES 4
typedef struct { char name[13];uint8_t *data;size_t size; } file_t;
typedef struct {
    int present,free_blocks,read_fault,write_fault,writes,reads;
    file_t files[CARD_FILES];
} card_t;
static file_t *file_of(card_t *c,const char *name,int create) {
    for(unsigned i=0;i<CARD_FILES;++i) if(c->files[i].data && !strcmp(c->files[i].name,name)) return c->files+i;
    if(!create) return NULL;
    for(unsigned i=0;i<CARD_FILES;++i) if(!c->files[i].data) { strcpy(c->files[i].name,name);return c->files+i; }
    return NULL;
}
static void *card_find(void *context) { card_t *c=context;return c->present ? c : NULL; }
static int card_free(void *context,void *card) { (void)context;return ((card_t *)card)->free_blocks; }
static int card_blocks(void *context,void *card,const char *name) {
    (void)context;const file_t *f=file_of(card,name,0);
    return f ? (int)(f->size/512u) : 0;
}
static int card_read(void *context,void *card,const char *name,uint8_t **data,size_t *size) {
    (void)context;card_t *c=card;const file_t *f=file_of(c,name,0);
    ++c->reads;
    if(!f || c->read_fault) return -1;
    *size=f->size;*data=malloc(*size);memcpy(*data,f->data,*size);return 0;
}
static void card_release(void *context,uint8_t *data) { (void)context;free(data); }
static int card_write(void *context,void *card,const char *name,const uint8_t *data,size_t size) {
    (void)context;card_t *c=card;
    assert(size%512u==0);
    if(c->write_fault) return -4;
    file_t *f=file_of(c,name,1);
    const int old=f->data ? (int)(f->size/512u) : 0;
    if(c->free_blocks+old<(int)(size/512u)) return -7;
    free(f->data);
    f->data=malloc(size);memcpy(f->data,data,size);f->size=size;
    c->free_blocks+=old-(int)(size/512u);++c->writes;return 0;
}
static void card_clear(card_t *c) { for(unsigned i=0;i<CARD_FILES;++i) free(c->files[i].data);memset(c,0,sizeof(*c)); }
static card_t card;
static const nm_save_device_t fake={&card,card_find,card_free,card_blocks,card_read,card_release,card_write};

static void test_crc(void) {
    const uint8_t check[]="123456789";
    assert(nm_save_crc16(check,9)==0x31c3u);          /* CRC-16/XMODEM check value */
    assert(nm_save_crc32(check,9)==UINT32_C(0xcbf43926));
}

static void test_defaults(void) {
    nm_save_t s;nm_save_defaults(&s,seeds);
    assert(nm_save_valid(&s));
    assert(s.settings.difficulty==NM_DIFFICULTY_NORMAL && s.settings.advertise_sound==NM_ADVERTISE_ON);
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) {
        assert(s.selected[t]==NM_SAVE_NO_SLOT);
        for(unsigned i=0;i<NM_SAVE_SLOTS;++i) {
            const nm_race_record_t *r=&s.records[t][i];
            assert(r->rank==i+1 && r->time==seeds[t][i].time && r->time_copy==r->time && r->flags==seeds[t][i].flags &&
                   r->name==seeds[t][i].name && r->view==UINT32_MAX);
        }
    }
    static const uint32_t ta[16]={1280,3840,7680,UINT32_MAX,10240,15360,15360,UINT32_MAX,
                                  1280,3840,7680,UINT32_MAX,10240,15360,15360,UINT32_MAX};
    assert(!memcmp(s.time_attack,ta,sizeof(ta)));
    /* 226a5c: course bytes 0..3 and the sign-extended masks. */
    assert(nm_save_table(0)==0 && nm_save_table(1)==1 && nm_save_table(2)==2 && nm_save_table(3)==2 &&
           nm_save_table(0xffff0005u)==1);
}

static void test_insert(void) {
    nm_race_record_t a[NM_SAVE_SLOTS],b[NM_SAVE_SLOTS];
    nm_save_t s;nm_save_defaults(&s,seeds);
    /* nm_save_insert agrees with nm_race_record_update's insertion for every time around every slot. */
    for(uint32_t time=0x2800-2;time<0x2800+320*24+2;time+=53) {
        memcpy(a,s.records[0],sizeof(a));memcpy(b,s.records[0],sizeof(b));
        const uint32_t counts[4]={1,9,3,9};unsigned selected=99;uint32_t note=7;
        const int r=nm_race_record_update(a,2,time,counts,1,1,&selected,&note);
        const nm_race_record_t record={.time=time,.view=1,.flags=3,.time_copy=time,.name=0x00202020};
        const unsigned slot=nm_save_insert(b,&record);
        assert(r==2 ? slot==selected : slot==NM_SAVE_NO_SLOT);
        assert(!memcmp(a,b,sizeof(a)));
    }
}

static void test_payload_and_file(void) {
    nm_save_t s,t;nm_save_defaults(&s,seeds);
    s.settings=(nm_settings_t){.difficulty=NM_DIFFICULTY_HARDEST,.advertise_sound=NM_ADVERTISE_OFF};
    s.selected[1]=4;s.records[2][7].name=0x00444342;s.records[2][7].view=2;s.time_attack[5]=9999;
    uint8_t payload[NM_SAVE_PAYLOAD_BYTES];
    assert(nm_save_encode(&s,payload)==NM_SAVE_PAYLOAD_BYTES);
    memset(&t,0,sizeof(t));
    assert(nm_save_decode(payload,sizeof(payload),&t) && !memcmp(&s,&t,sizeof(s)));
    /* Every damaged byte, a short buffer and invalid contents are refused, leaving the target unchanged. */
    for(unsigned i=0;i<sizeof(payload);++i) {
        payload[i]^=0x40;memset(&t,0x5a,sizeof(t));
        assert(!nm_save_decode(payload,sizeof(payload),&t));
        for(unsigned k=0;k<sizeof(t);++k) assert(((uint8_t *)&t)[k]==0x5a);
        payload[i]^=0x40;
    }
    assert(!nm_save_decode(payload,sizeof(payload)-1,&t));
    nm_save_t bad=s;bad.records[0][3].rank=9;assert(!nm_save_valid(&bad) && !nm_save_encode(&bad,payload));
    bad=s;bad.settings.difficulty=4;assert(!nm_save_encode(&bad,payload));
    bad=s;bad.selected[0]=24;assert(!nm_save_encode(&bad,payload));
    /* The VMS file: header fields as KOS's vmu_hdr_t lays them out, the CRC over the file with its field zero. */
    static uint8_t file[NM_SAVE_FILE_BLOCKS*512u];
    memset(file,0xee,sizeof(file));
    assert(NM_SAVE_FILE_BYTES==2452 && NM_SAVE_FILE_BLOCKS==5);
    assert(nm_save_file_build(&s,file)==NM_SAVE_FILE_BYTES);
    assert(!memcmp(file,"DAYTONA USA     ",16) && !memcmp(file+16,"RECORDS AND SETTINGS            ",32));
    assert(!memcmp(file+48,"DAYTONA93NATIVE",16));
    assert(file[64]==1 && file[65]==0 && file[68]==0 && file[69]==0);
    assert((file[72]|file[73]<<8|file[74]<<16|(uint32_t)file[75]<<24)==NM_SAVE_PAYLOAD_BYTES);
    for(unsigned i=76;i<96;++i) assert(!file[i]);
    const uint16_t crc=(uint16_t)(file[70]|file[71]<<8);
    file[70]=file[71]=0;assert(nm_save_crc16(file,NM_SAVE_FILE_BYTES)==crc);
    file[70]=(uint8_t)crc;file[71]=(uint8_t)(crc>>8);
    memset(&t,0,sizeof(t));
    assert(nm_save_file_parse(file,sizeof(file),&t) && !memcmp(&s,&t,sizeof(s)));
    /* The icon: 32x32 of the 6 colours of the palette, every colour used. */
    unsigned used=0;
    for(unsigned i=0;i<NM_SAVE_ICON_BYTES;++i) used|=1u<<(file[128+i]>>4)|1u<<(file[128+i]&15u);
    assert(used==0x3fu);
    for(unsigned i=0;i<sizeof(file);++i) {
        file[i]^=0x01;
        const int ok=nm_save_file_parse(file,sizeof(file),&t);
        assert(ok==(i>=NM_SAVE_FILE_BYTES));    /* the block's tail after the file is not part of it */
        file[i]^=0x01;
    }
    assert(!nm_save_file_parse(file,NM_SAVE_FILE_BYTES-1,&t) && !nm_save_file_parse(file,100,&t));
}

static void test_store(void) {
    static uint8_t snapshot[NM_SAVE_FILE_BYTES],scratch[NM_SAVE_FILE_BLOCKS*512u];
    nm_save_t defaults,s,t;nm_save_defaults(&defaults,seeds);
    card_clear(&card);
    s=defaults;assert(nm_save_store_load(&fake,&s)==NM_SAVE_NO_CARD && !memcmp(&s,&defaults,sizeof(s)));
    assert(nm_save_store_write(&fake,snapshot,NULL,0,NULL,1,scratch)==NM_SAVE_NO_CARD);
    card.present=1;card.free_blocks=200;
    assert(nm_save_store_load(&fake,&s)==NM_SAVE_NO_FILE && !memcmp(&s,&defaults,sizeof(s)));
    /* Write, read back. */
    s.settings.difficulty=NM_DIFFICULTY_EASY;
    const nm_race_record_t won={.time=100,.view=3,.flags=0,.time_copy=100,.name=0x00585858};
    s.selected[0]=(uint8_t)nm_save_insert(s.records[0],&won);assert(s.selected[0]==0);
    assert(nm_save_file_build(&s,snapshot)==NM_SAVE_FILE_BYTES);
    assert(nm_save_store_write(&fake,snapshot,NULL,0,&s.settings,1,scratch)==NM_SAVE_OK);
    assert(card.free_blocks==195 && card.writes==1);
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_OK && !memcmp(&s,&t,sizeof(s)));
    /* Overwrite in place on a card with no other free block. */
    card.free_blocks=0;
    assert(nm_save_store_write(&fake,snapshot,NULL,0,&s.settings,1,scratch)==NM_SAVE_OK && card.free_blocks==0);
    /* Damaged on the card: defaults kept on load; the next write replaces it. */
    file_t *f=file_of(&card,NM_SAVE_FILE_NAME,0);
    f->data[900]^=0x10;
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_DAMAGED && !memcmp(&t,&defaults,sizeof(t)));
    assert(nm_save_store_write(&fake,snapshot,NULL,0,&s.settings,1,scratch)==NM_SAVE_OK);
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_OK && !memcmp(&s,&t,sizeof(s)));
    /* Truncated (a file of fewer blocks), another version: damaged. */
    f->size=1024;
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_DAMAGED && !memcmp(&t,&defaults,sizeof(t)));
    f->size=2560;
    f->data[640+4]=2;
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_DAMAGED);
    f->data[640+4]=1;
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_OK);
    /* Read and write faults. */
    card.read_fault=1;t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_IO_ERROR && !memcmp(&t,&defaults,sizeof(t)));
    assert(nm_save_store_write(&fake,snapshot,NULL,0,&s.settings,1,scratch)==NM_SAVE_IO_ERROR);
    card.read_fault=0;card.write_fault=1;
    assert(nm_save_store_write(&fake,snapshot,NULL,0,&s.settings,1,scratch)==NM_SAVE_IO_ERROR);
    card.write_fault=0;
    /* A full card without the file: nothing written. */
    card_clear(&card);card.present=1;card.free_blocks=4;
    assert(nm_save_store_write(&fake,snapshot,NULL,0,&s.settings,1,scratch)==NM_SAVE_FULL && !card.writes);
    assert(nm_save_store_load(&fake,&t)==NM_SAVE_NO_FILE);
    /* Merge: the card holds its own records (another card, or one put in after the boot); the session's inserts
     * go into them, the settings are the session's. Without merge the snapshot replaces them. */
    card.free_blocks=200;
    nm_save_t other=defaults;
    const nm_race_record_t theirs={.time=200,.view=0,.flags=0,.time_copy=200,.name=0x00595959};
    other.selected[2]=(uint8_t)nm_save_insert(other.records[2],&theirs);
    static uint8_t other_file[NM_SAVE_FILE_BYTES];
    assert(nm_save_file_build(&other,other_file)==NM_SAVE_FILE_BYTES);
    assert(nm_save_store_write(&fake,other_file,NULL,0,NULL,1,scratch)==NM_SAVE_OK);
    const nm_save_insert_t inserts[2]={{.table=2,.record={.time=150,.time_copy=150,.name=0x005a5a5a}},
                                       {.table=0,.record=won}};
    assert(nm_save_store_write(&fake,snapshot,inserts,2,&s.settings,1,scratch)==NM_SAVE_OK);
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_OK);
    assert(t.settings.difficulty==NM_DIFFICULTY_EASY && t.records[2][0].name==0x005a5a5a &&
           t.records[2][1].name==0x00595959 && t.records[0][0].name==0x00585858 && t.selected[2]==0 && t.selected[0]==0);
    assert(t.records[2][2].time==seeds[2][0].time && t.records[2][23].time==seeds[2][21].time);
    assert(nm_save_store_write(&fake,snapshot,inserts,2,&s.settings,0,scratch)==NM_SAVE_OK);
    t=defaults;assert(nm_save_store_load(&fake,&t)==NM_SAVE_OK && !memcmp(&s,&t,sizeof(s)));
    card_clear(&card);
}

/* The game's save state over the fake card: boot, a race's record, the name, the request, the next boot. */
static void test_game(void) {
    card_clear(&card);card.present=1;card.free_blocks=200;
    nm_game_save_device_set(&fake);
    nm_game_save_boot(seeds,1);
    assert(nm_game_save_status().boot==NM_SAVE_NO_FILE && nm_game_settings()->difficulty==NM_DIFFICULTY_NORMAL);
    nm_settings_t set={.difficulty=NM_DIFFICULTY_HARD,.advertise_sound=NM_ADVERTISE_ON};
    assert(nm_game_settings_set(&set));
    set.difficulty=7;assert(!nm_game_settings_set(&set) && nm_game_settings()->difficulty==NM_DIFFICULTY_HARD);
    /* Two races on course 2 (Advanced) before a request, the second faster: both reach the card. */
    unsigned slot;uint32_t note;const uint32_t counts[4]={0,5,0,0};
    nm_race_record_t *advanced=nm_game_records(2);
    assert(advanced==nm_game_records(3) && advanced!=nm_game_records(1));
    assert(nm_race_record_update(advanced,2,0x3c00+320*5+1,counts,0,0,&slot,&note)==2 && slot==6 && !note);
    nm_game_record_inserted(2,slot);advanced[slot].name=0x00414141;
    assert(nm_race_record_update(advanced,2,0x3c00-5,counts,0,0,&slot,&note)==2 && slot==0 && note==1);
    nm_game_record_inserted(2,slot);advanced[slot].name=0x00424242;
    nm_game_save_request();
    assert(card.writes==1 && nm_game_save_status().last==NM_SAVE_OK && !nm_game_save_status().busy);
    nm_game_save_boot(seeds,1);
    assert(nm_game_save_status().boot==NM_SAVE_OK && nm_game_settings()->difficulty==NM_DIFFICULTY_HARD);
    advanced=nm_game_records(2);
    assert(advanced[0].name==0x00424242 && advanced[7].name==0x00414141 && advanced[7].rank==8);
    /* No card at the boot, a card with these records put in later: the new record joins them. */
    card.present=0;nm_game_save_boot(seeds,1);assert(nm_game_save_status().boot==NM_SAVE_NO_CARD);
    nm_race_record_t *beginner=nm_game_records(0);
    assert(nm_race_record_update(beginner,2,1000,counts,0,0,&slot,&note)==2 && slot==0);
    nm_game_record_inserted(0,slot);beginner[0].name=0x00434343;
    nm_game_save_request();assert(nm_game_save_status().last==NM_SAVE_NO_CARD && card.writes==1);
    card.present=1;nm_game_save_request();
    assert(nm_game_save_status().last==NM_SAVE_OK && card.writes==2);
    nm_game_save_boot(seeds,1);
    /* The session did not set the settings: the card keeps its own. */
    assert(nm_game_records(0)[0].name==0x00434343 && nm_game_records(2)[0].name==0x00424242 &&
           nm_game_settings()->difficulty==NM_DIFFICULTY_HARD);
    set.difficulty=NM_DIFFICULTY_EASY;assert(nm_game_settings_set(&set));
    card.present=0;nm_game_save_request();card.present=1;
    nm_game_save_request();nm_game_save_boot(seeds,1);assert(nm_game_settings()->difficulty==NM_DIFFICULTY_EASY);
    /* A clear: the card gets the reset records, not a merge. */
    nm_game_records_clear();nm_game_save_request();
    nm_game_save_boot(seeds,1);
    assert(nm_game_records(0)[0].name==seeds[0][0].name && nm_game_records(2)[0].name==seeds[2][0].name);
    /* Scripted builds: defaults, never a write. */
    const int writes=card.writes;
    nm_game_save_boot(seeds,0);assert(nm_game_records(0)[0].name==seeds[0][0].name);
    nm_game_save_request();assert(card.writes==writes);
    nm_game_save_device_set(NULL);card_clear(&card);
}

int main(void) {
    make_seeds();
    test_crc();test_defaults();test_insert();test_payload_and_file();test_store();test_game();
    puts("PASS: save data, VMS file (header, icon, CRC), fake VMU store (absent, empty, full, damaged, truncated, "
         "faults, merge) and the game's save state round trip");
    return 0;
}
