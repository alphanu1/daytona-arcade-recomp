/* Compile with -Icode -I<generated-header-directory>; run with main_data.bin.
 * ROM-derived data stays local. */
#include "intro_drive_data.h"
#include <assert.h>
#include <stdio.h>

static uint32_t rom_word(FILE *file,uint32_t address) {
    unsigned char b[4];
    assert(address>=0x2000000 && address<=0x3fffffc && !(address&3));
    assert(!fseek(file,(long)(address-0x2000000),SEEK_SET));
    assert(fread(b,1,4,file)==4);
    return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
}
int main(int argc,char **argv) {
    assert(argc==2); // Independently check generated rows against owned main_data.bin.
    FILE *rom=fopen(argv[1],"rb");assert(rom);
    assert(sizeof(nm_intro_crash_models)/sizeof(nm_intro_crash_models[0])==2);
    for(unsigned level=0;level<2;++level) {
        assert(nm_intro_crash_models[level]<sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0]));
        assert(nm_intro_model_ids[nm_intro_crash_models[level]]==rom_word(rom,level ? 0x288f5ec : 0x288f600));
    }
    assert(sizeof(nm_intro_special_phase_models)/sizeof(nm_intro_special_phase_models[0])==8);
    for(unsigned phase=0;phase<8;++phase) {
        /* Independently observed d144 table: each of four descriptors lasts two ticks. */
        const unsigned binding=nm_intro_special_phase_models[phase];
        assert(binding<sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0]));
        assert(nm_intro_model_ids[binding]==rom_word(rom,0x288d670+20*(phase/2)));
    }
    unsigned crash_rows=0;
    assert(sizeof(nm_crash_row_t)==12);
    assert(sizeof(nm_intro_crash_sequences)/sizeof(nm_intro_crash_sequences[0])==3);
    assert(nm_intro_crash_sequences[0].rows==nm_intro_crash_sequences[1].rows);
    for(unsigned s=0;s<3;++s) {
        const uint32_t sequence=rom_word(rom,rom_word(rom,0x2896e58+4*s)+4);
        const unsigned count=rom_word(rom,sequence);
        assert(count==nm_intro_crash_sequences[s].count);
        assert(!((uintptr_t)nm_intro_crash_sequences[s].rows&31));
        for(unsigned r=0;r<count;++r) {
            const nm_crash_row_t *row=nm_intro_crash_sequences[s].rows+r;
            const uint32_t a=rom_word(rom,sequence+8+16*r),b=rom_word(rom,sequence+12+16*r),c=rom_word(rom,sequence+16+16*r);
            assert(row->model_key==a && row->marker==(uint16_t)b);
            assert((uint16_t)row->angle[0]==(b>>16) && (uint16_t)row->angle[1]==(uint16_t)c && (uint16_t)row->angle[2]==(c>>16));
            ++crash_rows;
        }
    }
    assert(!fclose(rom));
    printf("PASS: %u source crash rows, aligned deduplicated native arrays\n",crash_rows);
    _Static_assert(sizeof(nm_intro_spawn_t)==16,"spawn ABI");
    _Static_assert(sizeof(nm_intro_event_t)==16,"event ABI");
    _Static_assert(sizeof(nm_intro_body_models[0])==32,"aligned model-choice row");
    enum { bindings=sizeof(nm_intro_model_ids)/sizeof(nm_intro_model_ids[0]) };
    assert(bindings==110);
    assert(nm_intro_shadow_model<bindings);
    assert(nm_intro_model_ids[nm_intro_shadow_model]==0x8d9e42);
    assert(sizeof(nm_intro_body_models)/sizeof(nm_intro_body_models[0])==20);
    assert(!((uintptr_t)nm_intro_body_models&31));
    for(unsigned i=0;i<bindings;++i) {
        assert(i<2 ? nm_intro_model_ids[i]==(i ? 0x241u : 0x1c8u) :
               (nm_intro_model_ids[i]>=0x800000 && nm_intro_model_ids[i]<0xc00000));
        if(i) assert(nm_intro_model_ids[i-1]<nm_intro_model_ids[i]);
    }
    for(unsigned car=0;car<20;++car) for(unsigned choice=0;choice<8;++choice)
        assert(nm_intro_body_models[car][choice]<bindings);
    assert(nm_intro_special_model<bindings);
    for(unsigned i=0;i<4;++i) {
        assert(nm_intro_wheel_models[i]<bindings && nm_intro_selector_models[i]<bindings);
    }
    assert(sizeof(nm_intro_wheel_phase_models)/sizeof(nm_intro_wheel_phase_models[0])==16);
    for(unsigned i=0;i<16;++i) assert(nm_intro_wheel_phase_models[i]<bindings);
    assert(sizeof(nm_intro_window_models)/sizeof(nm_intro_window_models[0])==2);
    assert(nm_intro_window_models[0]==0 && nm_intro_window_models[1]==1);
    assert(sizeof(nm_intro_stages)/sizeof(nm_intro_stages[0])==2);
    assert(nm_intro_stages[0].cars==20 && nm_intro_stages[1].cars==21);
    assert(nm_intro_stages[1].spawn[20].kind==2);
    unsigned ticks=0;
    for(unsigned stage=0;stage<2;++stage) {
        const nm_intro_stage_data_t *data=nm_intro_stages+stage;
        assert(!((uintptr_t)data->spawn&31) && !((uintptr_t)data->events&31));
        assert(!((uintptr_t)data->keys&31) && !((uintptr_t)data->times&31));
        assert(data->key_count==(stage ? 11 : 6));
        nm_intro_cursor_t camera={0};
        for(unsigned tick=0;tick<10000;++tick) {
            nm_intro_interval_t interval;
            assert(nm_intro_camera_step(data->times,data->key_count,&camera,&interval));
            assert(interval.key+1<data->key_count && interval.elapsed<interval.duration);
        }
        nm_player_t cars[44]={0};nm_intro_cursor_t cursor={0};
        for(unsigned i=0;i<data->cars;++i) {
            assert(data->spawn[i].route<8 && data->spawn[i].kind<=2);
            assert(isfinite(data->spawn[i].speed));
        }
        for(unsigned tick=0;;++tick) {
            assert(tick<1100);
            uint32_t next=UINT32_MAX;
            const int result=nm_intro_events_step(data->events,data->event_count,cars,data->cars,&cursor,&next);
            assert(result);++ticks;
            if(result==2) { assert(next==1);break; }
        }
    }
    assert(ticks==1604);
    assert(!((uintptr_t)&nm_intro_speeds&31));
    assert(nm_intro_speeds.desired_divisor>0 && isfinite(nm_intro_speeds.desired_divisor));
    for(unsigned i=0;i<512;++i) assert(isfinite(nm_intro_speeds.response[i]));
    puts("PASS: generated intro data,32-byte alignment,41spawns,24events,17camera keys,1604timed updates");
    return 0;
}
