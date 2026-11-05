/* The native save state against the original's backup RAM (m2segment --save-nvram DIR: DIR/backup_ram.bin; ROM
 * output, never committed). Build with the generated record seeds (NATIVE_RECORD_DATA_DIR):
 *   cc -std=c11 -Iinclude -I$RECORD_DATA tests/check_save_reference.c code/save/save_data.c code/save/save_store.c
 * usage: check_save_reference BACKUP_RAM.bin [COURSE...]
 *   Without COURSE: the RAM of a boot (5234 found no signature and ran 2265cc): every record table, the selected
 *   slots and the time-attack words equal nm_save_defaults.
 *   With COURSE (a course byte per race the run finished, in order): the RAM after those races. For each, the
 *   record the original put at that course's selected slot (*1d00a20.., with the name it was given) is inserted
 *   into the native defaults with nm_save_insert; then every table, every selected slot and the time-attack words
 *   must equal the original's. */
#include "save_store.h"
#include "race_record_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t tables[NM_SAVE_TABLES]={0x360,0x5a0,0x7e0},selections[NM_SAVE_TABLES]={0xa20,0xa24,0xa2c};
static uint8_t ram[0x4000];
static uint32_t word(uint32_t at) { return (uint32_t)ram[at]|ram[at+1]<<8|ram[at+2]<<16|(uint32_t)ram[at+3]<<24; }

static int original(nm_save_t *s) {
    nm_save_defaults(s,nm_race_record_seeds);
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) {
        for(unsigned i=0;i<NM_SAVE_SLOTS;++i) {
            const uint32_t at=tables[t]+24*i;
            s->records[t][i]=(nm_race_record_t){.rank=word(at),.time=word(at+4),.view=word(at+8),.flags=word(at+12),
                .time_copy=word(at+16),.name=word(at+20)};
        }
        const uint32_t pointer=word(selections[t]);
        if(pointer==UINT32_MAX) s->selected[t]=NM_SAVE_NO_SLOT;
        else if(pointer<0x1d00000+tables[t] || (pointer-0x1d00000-tables[t])%24 || (pointer-0x1d00000-tables[t])/24>=24) {
            printf("FAIL: selected pointer %08lx of table %u outside it\n",(unsigned long)pointer,t);return 0;
        } else s->selected[t]=(uint8_t)((pointer-0x1d00000-tables[t])/24);
    }
    for(unsigned i=0;i<NM_SAVE_TIME_ATTACK_WORDS;++i) s->time_attack[i]=word(0xb60+4*i);
    return 1;
}

int main(int argc,char **argv) {
    if(argc<2) { fputs("usage: check_save_reference BACKUP_RAM.bin [COURSE...]\n",stderr);return 2; }
    FILE *f=fopen(argv[1],"rb");
    if(!f || fread(ram,1,sizeof(ram),f)!=sizeof(ram)) { fprintf(stderr,"cannot read 16 KiB from %s\n",argv[1]);return 2; }
    fclose(f);
    nm_save_t arcade,native;
    if(!original(&arcade)) return 1;
    nm_save_defaults(&native,nm_race_record_seeds);
    native.settings=arcade.settings;
    for(int i=2;i<argc;++i) {
        const unsigned t=nm_save_table((unsigned)strtoul(argv[i],NULL,0));
        if(arcade.selected[t]==NM_SAVE_NO_SLOT) { printf("FAIL: no record selected for table %u\n",t);return 1; }
        const unsigned slot=nm_save_insert(native.records[t],&arcade.records[t][arcade.selected[t]]);
        native.selected[t]=(uint8_t)slot;
        printf("race %d: table %u, time %lu, name %06lx, slot %u (original %u)\n",i-1,t,
            (unsigned long)arcade.records[t][arcade.selected[t]].time,(unsigned long)(arcade.records[t][arcade.selected[t]].name&0xffffff),
            slot,arcade.selected[t]);
    }
    unsigned differences=0;
    for(unsigned t=0;t<NM_SAVE_TABLES;++t) {
        if(arcade.selected[t]!=native.selected[t]) {
            printf("FAIL: table %u selected original %u native %u\n",t,arcade.selected[t],native.selected[t]);++differences;
        }
        for(unsigned i=0;i<NM_SAVE_SLOTS;++i) if(memcmp(&arcade.records[t][i],&native.records[t][i],sizeof(nm_race_record_t))) {
            const nm_race_record_t *a=&arcade.records[t][i],*b=&native.records[t][i];
            printf("FAIL: table %u slot %u original %lu/%lu/%lx/%lx/%lu/%06lx native %lu/%lu/%lx/%lx/%lu/%06lx\n",t,i,
                (unsigned long)a->rank,(unsigned long)a->time,(unsigned long)a->view,(unsigned long)a->flags,
                (unsigned long)a->time_copy,(unsigned long)a->name,(unsigned long)b->rank,(unsigned long)b->time,
                (unsigned long)b->view,(unsigned long)b->flags,(unsigned long)b->time_copy,(unsigned long)b->name);
            ++differences;
        }
    }
    for(unsigned i=0;i<NM_SAVE_TIME_ATTACK_WORDS;++i) if(arcade.time_attack[i]!=native.time_attack[i]) {
        printf("FAIL: time-attack word %u (1d00b%02x) original %08lx native %08lx\n",i,0x60+4*i,
            (unsigned long)arcade.time_attack[i],(unsigned long)native.time_attack[i]);++differences;
    }
    /* The original's state through the save file and back. */
    static uint8_t file[NM_SAVE_FILE_BYTES];nm_save_t back;
    if(nm_save_file_build(&arcade,file)!=NM_SAVE_FILE_BYTES || !nm_save_file_parse(file,sizeof(file),&back) ||
       memcmp(&back,&arcade,sizeof(back))) { puts("FAIL: the original's state does not survive the save file");++differences; }
    if(differences) return 1;
    printf("PASS: %u record tables (72 slots), selected slots and 16 time-attack words equal the original's backup RAM "
           "after %d race(s); its state survives the save file\n",NM_SAVE_TABLES,argc-2);
    return 0;
}
