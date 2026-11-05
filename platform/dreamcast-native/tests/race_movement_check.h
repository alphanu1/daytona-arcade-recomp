/* Local generated-header include and owned DND/DNC/DNR/path directory required.
 * Autonomous movement smoke check, NOT full race/initialization parity. */
#undef NDEBUG /* Checks and their calls must also run in Release builds. */
#include "../include/race_scene.h"
#include "../include/car_detail.h"
#include "../include/finish_actor.h"
#include "../include/race_records.h"
#include "../include/result_actor.h"
#include "../include/name_entry.h"
#ifdef NATIVE_NAME_DATA
#include "name_entry_data.h"
#endif
#include "intro_drive_data.h"
#ifdef NATIVE_RESULT_DATA
#include "result_actor_data.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int nm_race_movement_check(const char *drive_file,const char *contact_file,
        const char *road_file,const char *directory) {
    nm_name_entry_t name={0};uint32_t name_command=0;
    for(unsigned i=0;i<3;++i) {
        assert(nm_name_select(&name,(int8_t)(-127+8*(int)i),&name_command));
        assert(name.glyph=='A'+i && nm_name_accept(&name,1,&name_command)==1);
        assert(name.count==i+1 && name_command==0xae2139u+i);
    }
    assert(!memcmp(name.name,"ABC",3) && name.index==28 && name.glyph=='C');
    assert(nm_name_select(&name,-128,&name_command) && name.index==27 && name.glyph=='}');
    assert(nm_name_accept(&name,1,&name_command)==1 && name.count==2 && !name.name[2] && name_command==0xae2161);
    assert(nm_name_select(&name,-128,&name_command) && name.index==28 && name.glyph=='{');
    assert(nm_name_accept(&name,UINT32_MAX,&name_command)==2 && !name_command);
    const nm_name_entry_t before_name=name;
    assert(nm_name_accept(&name,0,&name_command)==1 && !memcmp(&name,&before_name,sizeof(name)));
    name.count=4;const nm_name_entry_t invalid_name=name;name_command=123;
    assert(!nm_name_select(&name,0,&name_command) && !nm_name_accept(&name,1,&name_command) &&
        !memcmp(&name,&invalid_name,sizeof(name)) && name_command==123);
    assert(!nm_name_select(NULL,0,&name_command) && !nm_name_accept(NULL,0,&name_command));
    puts("PASS native name input:ABC/back/finish, -128 steering edge, preserved state, invalid input; commit separate");
#ifdef NATIVE_NAME_DATA
    _Static_assert(sizeof(nm_name_rules_t)==516,"name rules fixed offline size");
    uint32_t commit_hash=UINT32_C(2166136261);
    const uint32_t notifications[]={0,1,2,UINT32_MAX};
    for(unsigned index=0;index<=40;++index) for(unsigned frame=0;frame<4;++frame)
    for(unsigned n=0;n<4;++n) {
        const uint32_t raw=(index<40 ? nm_name_rules.rules[index].name : 0)|UINT32_C(0x5a000000);
        nm_name_entry_t entry={0};memcpy(entry.name,&raw,4);
        nm_name_commit_t result={.display={0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5}};
        uint32_t record_name=0x12345678;
        assert(nm_name_commit(&entry,&nm_name_rules,frame,notifications[n],&record_name,&result));
        uint32_t written_name;memcpy(&written_name,entry.name,4);
        const uint32_t display0=(uint32_t)result.display[0]|((uint32_t)result.display[1]<<8)|
            ((uint32_t)result.display[2]<<16)|((uint32_t)result.display[3]<<24);
        const uint32_t display1=(uint32_t)result.display[4]|((uint32_t)result.display[5]<<8)|
            ((uint32_t)result.display[6]<<16)|UINT32_C(0xa5000000);
        const uint32_t values[]={written_name,display0,display1,result.timer,record_name,result.phase,result.command};
        for(unsigned i=0;i<7;++i) commit_hash=(commit_hash^values[i])*UINT32_C(16777619);
        assert(result.display[0]==0xa5 && entry.name[3]==0x5a);
        const nm_name_entry_t old_entry=entry;const nm_name_commit_t old_result=result;
        assert(!nm_name_commit(&entry,&nm_name_rules,0,1,NULL,&result) &&
            !memcmp(&entry,&old_entry,sizeof(entry)) && !memcmp(&result,&old_result,sizeof(result)));
    }
    assert(commit_hash==UINT32_C(0xc9fe6f81));
    printf("PASS native name commit:656 cases original227934/227b1c hash=%08lx, offline tables, silent commands\n",(unsigned long)commit_hash);
#endif
    _Static_assert(sizeof(nm_result_actor_t)==96,"eight result positions and two angle arrays");
    _Static_assert(sizeof(nm_result_target_t)==16,"authored result target row");
    const uint32_t time_ticks[]={0,64,3840,38400,UINT32_C(0x40000000)};
    const char *time_text[]={" 0'00\"00"," 0'01\"00"," 1'00\"00","10'00\"00"," 0'00\"00"};
    for(unsigned t=0;t<5;++t) {
        uint16_t glyphs[8];nm_result_time_glyphs(time_ticks[t],glyphs);
        for(unsigned i=0;i<8;++i) assert(glyphs[i]==(0x8000u|(unsigned char)time_text[t][i]));
    }
    uint32_t time_hash=UINT32_C(2166136261),random_time=0;
    for(unsigned sample=0;sample<81920;++sample) {
        uint32_t ticks;
        if(sample<65536) ticks=sample<32768 ? sample : UINT32_C(0xffff0000)|sample;
        else { random_time=random_time*UINT32_C(1664525)+UINT32_C(1013904223);ticks=random_time; }
        uint16_t glyphs[8];nm_result_time_glyphs(ticks,glyphs);
        for(unsigned i=8;i;--i) time_hash=(time_hash^glyphs[i-1])*UINT32_C(16777619);
    }
    assert(time_hash==UINT32_C(0xdc5e24d8)); /* Original18544 glyph-call digest. */
    puts("PASS result time:81920 signed lap/uint32 total inputs, original glyph digest dc5e24d8");
    const nm_result_key_t result_keys[]={{0,2},{27,4},{10000,1}};
    uint32_t result_cursor=0,result_target=0;
    assert(nm_result_target_select(result_keys,3,0,&result_cursor,&result_target));
    assert(!result_cursor && !result_target);
    assert(nm_result_target_select(result_keys,3,1000,&result_cursor,&result_target));
    assert(result_cursor==1 && result_target==2); // Not a catch-up loop.
    assert(nm_result_target_select(result_keys,3,INT32_MIN,&result_cursor,&result_target));
    assert(result_cursor==1 && result_target==2);
    assert(nm_result_target_select(result_keys,3,1000,&result_cursor,&result_target));
    assert(result_cursor==2 && result_target==4);
    assert(nm_result_target_select(result_keys,3,INT32_MAX,&result_cursor,&result_target));
    assert(result_cursor==3 && result_target==1);
    assert(!nm_result_target_select(result_keys,3,1000,&result_cursor,&result_target));
    assert(result_cursor==3 && result_target==1);
    alignas(32) nm_result_actor_t result_state={0},result_expected;
    alignas(32) nm_result_target_t result_targets[8];
    for(unsigned tick=0;tick<1024;++tick) {
        result_expected=result_state;
        for(unsigned i=0;i<8;++i) {
            const float x=(float)((int)(tick%97)-48)*(float)(i+1);
            const float y=(float)((int)(tick%71)-35)*(float)(8-i);
            const float rate=(float)(tick%9)*0.25f;
            result_targets[i]=(nm_result_target_t){{x,y},rate,
                (int16_t)(tick*701u+i*8191u),(int16_t)(tick*991u+i*4093u)};
            const float dx=x-result_state.position[i].x,dy=y-result_state.position[i].y;
            const float factor=rate*0.125f;
            result_expected.position[i].x=result_state.position[i].x+factor*dx;
            result_expected.position[i].y=result_state.position[i].y+factor*dy;
            const int da=(int)result_targets[i].angle-result_state.angle[i];
            const int db=(int)result_targets[i].other_angle-result_state.other_angle[i];
            result_expected.angle[i]=(int16_t)(result_state.angle[i]+(int)floorf((float)da*0.25f));
            result_expected.other_angle[i]=(int16_t)(result_state.other_angle[i]+(int)floorf((float)db*0.25f));
        }
        assert(nm_result_actor_step(&result_state,result_targets));
        assert(!memcmp(&result_state,&result_expected,sizeof(result_state)));
    }
    result_expected=result_state;result_targets[7].rate=NAN;
    assert(!nm_result_actor_step(&result_state,result_targets));
    assert(!memcmp(&result_state,&result_expected,sizeof(result_state)));
    puts("PASS result animation:1024 persistent eight-part vector interpolation/angle ticks, atomic nonfinite rejection");
    const uint32_t clocks[]={0,999,1000,INT32_MAX,UINT32_C(0x80000000),UINT32_MAX};
    const int32_t timers[]={INT32_MIN,-1,0,191,192,193,576,INT32_MAX};
    for(unsigned c=0;c<6;++c) for(unsigned t=0;t<8;++t) for(unsigned skip=0;skip<2;++skip) {
        uint32_t clock=clocks[c];int32_t timer=timers[t];
        nm_result_clock_step(&clock,&timer,skip);
        assert(clock==(skip && t>=4 ? 1000 : clocks[c]+1u));
        assert(timer==(skip && t>=4 ? 192 : timers[t]));
    }
    puts("PASS result clock:96 signed threshold/held skip/wrap cases");
    for(unsigned raw=0;raw<256;++raw) for(unsigned penalty=0;penalty<2;++penalty)
    for(unsigned special=0;special<2;++special) {
        nm_result_playback_t playback={0};nm_result_vec2_t initial[8];
        for(unsigned i=0;i<8;++i) {
            initial[i]=(nm_result_vec2_t){(float)i,-(float)i};
            playback.actor.angle[i]=123;playback.actor.other_angle[i]=(int16_t)(i*8191u);
        }
        playback.clock=123;playback.cursor=91;playback.target=4;
        const int rank=raw<128 ? (int)raw : (int)raw-256;
        assert(nm_result_playback_init(&playback,initial,(int8_t)rank,penalty,(int16_t)special)==
            (rank<=2 && !penalty && special ? 2 : 1));
        assert(!playback.clock && !playback.cursor && !playback.target);
        for(unsigned i=0;i<8;++i) {
            assert(!memcmp(&playback.actor.position[i],initial+i,sizeof(initial[i])));
            assert(!playback.actor.angle[i] && playback.actor.other_angle[i]==(int16_t)(i*8191u));
        }
        const nm_result_playback_t before=playback;initial[7].y=NAN;
        assert(!nm_result_playback_init(&playback,initial,0,0,0) && !memcmp(&playback,&before,sizeof(before)));
    }
    for(unsigned mode=1;mode<=2;++mode) {
        const unsigned count=mode==1 ? 12 : 3;
        for(unsigned i=0;i<count;++i) assert(nm_result_entry_command(mode,i));
        assert(!nm_result_entry_command(mode,count));
        assert(!nm_result_entry_command(mode,UINT32_MAX));
        assert(nm_result_entry_command(mode,count-3)==0xbe3500);
        assert(nm_result_entry_command(mode,count-2)==0xbe3400);
        assert(nm_result_entry_command(mode,count-1)==0xbe3700);
    }
    assert(!nm_result_entry_command(0,0) && !nm_result_entry_command(3,0));
    const int32_t wait_timers[]={INT32_MIN,-1,0,575,576,577,800,INT32_MAX};
    const int16_t wait_fades[]={-32768,-1,0,15,16,17,32767};
    for(unsigned original=1;original<=2;++original)
    for(unsigned t=0;t<8;++t) for(unsigned f=0;f<7;++f) {
        unsigned mode=original;int16_t fade=wait_fades[f];
        assert(nm_result_wait_step(&mode,wait_timers[t],&fade)==
            (2u|(original==1 ? 1u : 0u)|(t<=4 ? 4u : 0u)));
        assert(mode==(t<=4 ? 3u : original));
        assert(fade==(original==2 ? wait_fades[f] : f<=4 ? 0 : wait_fades[f]-16));
        if(mode==3) {
            const int16_t before=fade;
            assert(!nm_result_wait_step(&mode,wait_timers[t],&fade) && fade==before);
        }
    }
    puts("PASS result entry/wait:1024 signed-rank branches, inherited angles,112 fade/timer cases and no same-tick animation");
#ifdef NATIVE_RESULT_DATA
    uint32_t announcement_hash=UINT32_C(2166136261);
    for(unsigned rank=0;rank<44;++rank) {
        uint32_t cursor=0,command;
        for(int32_t timer=800;timer>=576;--timer) {
            assert(nm_result_announcement_step(nm_result_announcements[rank],nm_result_announcement_counts[rank],
                0,timer,&cursor,&command));
            announcement_hash=(announcement_hash^command)*UINT32_C(16777619);
            announcement_hash=(announcement_hash^cursor)*UINT32_C(16777619);
        }
        const uint32_t before=cursor;command=99;
        assert(nm_result_announcement_step(NULL,0,1,768,&cursor,&command) && !command && cursor==before);
        assert(!nm_result_announcement_step(nm_result_announcements[rank],0,0,768,&cursor,&command) && cursor==before);
    }
    printf("Native result announcements:44 ranks9900ticks digest=%08lx\n",(unsigned long)announcement_hash);
    assert(announcement_hash==UINT32_C(0xe2294cb4)); // Original b484, all44 rank tables.
    uint32_t authored_hash=UINT32_C(2166136261);
    for(unsigned variant=0;variant<2;++variant) {
        nm_result_playback_t playback={0};
        for(unsigned i=0;i<8;++i) playback.actor.other_angle[i]=(int16_t)(i*8191u);
        assert(nm_result_playback_init(&playback,nm_result_initial,0,0,(int16_t)variant)==(variant ? 2 : 1));
        int32_t countdown=576;
        for(unsigned tick=0;tick<450;++tick) {
            assert(nm_result_playback_step(&playback,nm_result_keys[variant],21,nm_result_targets,
                sizeof(nm_result_targets)/sizeof(nm_result_targets[0]),&countdown,0));
            assert(playback.clock==tick+1 && countdown==576);
            result_state=playback.actor;
            for(unsigned i=0;i<8;++i) {
                const uint32_t values[]={nm_force_bits(result_state.position[i].x),nm_force_bits(result_state.position[i].y),
                    (uint16_t)result_state.angle[i],(uint16_t)result_state.other_angle[i]};
                for(unsigned v=0;v<4;++v) authored_hash=(authored_hash^values[v])*UINT32_C(16777619);
            }
        }
        const nm_result_playback_t before=playback;
        assert(!nm_result_playback_step(&playback,nm_result_keys[variant],21,nm_result_targets,0,&countdown,1));
        assert(!memcmp(&playback,&before,sizeof(playback)) && countdown==576);
        assert(nm_result_playback_step(&playback,nm_result_keys[variant],21,nm_result_targets,
            sizeof(nm_result_targets)/sizeof(nm_result_targets[0]),&countdown,1));
        assert(playback.clock==1000 && countdown==192);
        const uint32_t cursor=playback.cursor;
        countdown=191;
        assert(nm_result_playback_step(&playback,nm_result_keys[variant],21,nm_result_targets,
            sizeof(nm_result_targets)/sizeof(nm_result_targets[0]),&countdown,1));
        assert(playback.clock==1001 && countdown==191 && playback.cursor<=cursor+1);
    }
    printf("Native authored result animation:900 ticks digest=%08lx\n",(unsigned long)authored_hash);
    assert(authored_hash==UINT32_C(0xee12bcba)); // Actual i960/TGP outputs, both timelines.
#endif
    _Static_assert(sizeof(nm_race_record_t)==24,"record slot matches six source words");
    nm_race_record_t records[24],before_records[24];
    nm_race_record_seed_t seeds[24];
    for(unsigned i=0;i<24;++i) {
        records[i]=(nm_race_record_t){99,99,0x12340000u+i,99,99,99};
        seeds[i]=(nm_race_record_seed_t){i+1,1000+320*i,i&3u,0x414141u+i};
    }
    nm_race_records_reset(records,seeds);
    for(unsigned i=0;i<24;++i) {
        const nm_race_record_t expected={seeds[i].rank,seeds[i].time,0x12340000u+i,
            seeds[i].flags,seeds[i].time,seeds[i].name};
        assert(!memcmp(records+i,&expected,sizeof(expected)));
    }
    static nm_race_scene_t record_scene;
    memset(&record_scene,0,sizeof(record_scene));
    record_scene.phase=25;record_scene.count=40;record_scene.shared.clock=900;
    record_scene.view_ticks[2]=30;record_scene.view_ticks[1]=20;
    record_scene.cars[0].car.checkpoints.reverse=1;
    unsigned record_at=7;uint32_t record_notice=99;
    memcpy(before_records,records,sizeof(records));
    record_scene.cars[0].car.checkpoints.finish_flags=1; // Forced diagnostic, not completion.
    assert(nm_race_scene_record(&record_scene,records,1,&record_at,&record_notice)==1);
    assert(record_at==7 && !record_notice && !memcmp(records,before_records,sizeof(records)));
    record_scene.cars[0].car.checkpoints.finish_flags=2; // Completion after timer clearsbit0.
    record_scene.phase=24;record_notice=99;
    assert(!nm_race_scene_record(&record_scene,records,1,&record_at,&record_notice));
    assert(record_at==7 && record_notice==99 && !memcmp(records,before_records,sizeof(records)));
    record_scene.phase=25;
    assert(nm_race_scene_record(&record_scene,records,1,&record_at,&record_notice)==2);
    assert(record_at==0 && record_notice==1 && records[0].rank==1 && records[0].time==900 &&
        records[0].time_copy==900 && records[0].view==2 && records[0].flags==3 && records[0].name==0x202020);
    puts("PASS phase25 record binding: live clock/view/reverse, phase gate, forced-finish rejection, completed-race insertion");
    for(unsigned i=0;i<24;++i) records[i]=(nm_race_record_t){i,100,0,0,100,0x414141};
    const uint32_t usage[]={4,9,9,2};unsigned selected=7;uint32_t notification=123;
    assert(nm_race_record_update(records,2,100,usage,1,1,&selected,&notification)==2);
    assert(selected==0 && notification==1 && records[0].view==1 && records[0].flags==3 &&
        records[0].time_copy==100 && records[0].name==0x202020);
    for(unsigned i=0;i<24;++i) assert(records[i].rank==i);
    memcpy(before_records,records,sizeof(records));notification=123;
    assert(nm_race_record_update(records,2,101,usage,0,0,&selected,&notification)==1);
    assert(!notification && !selected && !memcmp(records,before_records,sizeof(records)));
    notification=123;
    assert(nm_race_record_update(records,1,0,usage,0,0,&selected,&notification)==1);
    assert(!notification && !selected && !memcmp(records,before_records,sizeof(records)));
    notification=123;
    assert(!nm_race_record_update(records,2,0,NULL,0,0,&selected,&notification));
    assert(notification==123 && !memcmp(records,before_records,sizeof(records)));
    puts("PASS native records: equal-time insertion, fixed ranks, greatest counter/first tie, eligibility and invalid input");
    _Static_assert(sizeof(nm_finish_actor_t)==32,"finish actor fits one cache line");
    alignas(32) nm_finish_actor_t actor;
    assert(nm_finish_actor_init(&actor,0x28));
    uint32_t pose_hash=2166136261u;
    for(unsigned tick=0;tick<384;++tick) {
        const int branch=nm_finish_actor_step(&actor);
        assert(branch==(tick<200 ? 1 : 2));
        assert(actor.timer==(uint16_t)(255-tick));
        const unsigned advances=tick<200 ? tick+1 : 200;
        assert(actor.tiny_y_bits==655*advances && actor.inherited_angle==(uint16_t)(0x28+655*advances));
        for(unsigned part=0;part<4;++part) {
            alignas(32) float pose[12];
            assert(nm_finish_actor_pose(&actor,part,branch,pose));
            for(unsigned i=0;i<12;++i) pose_hash=(pose_hash^nm_force_bits(pose[i]))*16777619u;
        }
    }
    /* Original i960/TGP matrix-read command17,384 ticks, four lists/tick. */
    assert(pose_hash==UINT32_C(0xb69ef930));
    assert(nm_force_bits(actor.approach.x)==0xbe999aca && nm_force_bits(actor.approach.y)==0xb727d000 &&
        nm_force_bits(actor.approach.z)==0x3efffe73);
    actor.tiny_y_bits=0x800000;
    const nm_finish_actor_t invalid_actor=actor;
    assert(!nm_finish_actor_step(&actor) && !memcmp(&actor,&invalid_actor,sizeof(actor)));
    assert(!nm_finish_actor_init(NULL,0) && !nm_finish_actor_step(NULL));
    printf("PASS native finish actor:384 ticks 1536 matrices hash=%08lx raw subnormal preserved\n",(unsigned long)pose_hash);
    for(unsigned raw=0;raw<256;++raw) for(unsigned inhibit=0;inhibit<2;++inhibit) {
        nm_race_finish_entry_t entry={.camera_mode=123,.timer=456,
            .previous_object_flags=UINT32_C(0x80012345),.result_object_flags=UINT32_C(0x00123456),
            .countdown=99,.phase=23};
        assert(nm_race_finish_enter(inhibit,(int8_t)(raw<128 ? (int)raw : (int)raw-256),&entry));
        assert(!entry.camera_mode && entry.timer==384 && entry.countdown==-1 && entry.phase==24);
        assert(entry.previous_object_flags==UINT32_C(0x00012345) &&
            entry.result_object_flags==UINT32_C(0x80123456));
        assert(entry.player_finishing && entry.result_pending && entry.command==9);
        const unsigned winner=!inhibit && !raw;
        assert(entry.audio_count==1+winner && entry.audio_commands[0]==(winner ? 0xae2124u : 0xbe3700u) &&
            entry.audio_commands[1]==(winner ? 0xbe3700u : 0));
        const nm_race_finish_entry_t before=entry;
        assert(!nm_race_finish_enter(inhibit,0,&entry) && !memcmp(&entry,&before,sizeof(entry)));
    }
    assert(!nm_race_finish_enter(0,0,NULL));
    puts("PASS native finish phase entry:512 rank/inhibit cases, one-shot intents and preserved flag bits");
    nm_race_postresult_t post={.timer=7,.name_flags=0x12345678,.phase=29,.cabinet_flags=255};
    assert(nm_race_postresult_step(&post,0) && post.phase==2 && post.timer==1920 &&
        post.name_flags==0x12345678 && post.cabinet_flags==128 && post.return_to_attract && post.audio_command==0xbe1a10);
    const nm_race_postresult_t before_post=post;
    assert(!nm_race_postresult_step(&post,1) && !memcmp(&post,&before_post,sizeof(post)));
    assert(!nm_race_postresult_step(NULL,0));
    post.phase=29;
    assert(nm_race_postresult_step(&post,1) && post.phase==30 && post.timer==1920 &&
        post.start_name && !post.return_to_attract && post.audio_command==0xae1005 && post.name_flags==0x92345678);
    post.timer=1;
    assert(nm_race_postresult_step(&post,0) && post.phase==30 && !post.timer &&
        post.name_flags==0x12345678 && !post.start_name && !post.audio_command);
    post.timer=2;
    assert(nm_race_postresult_step(&post,0) && post.phase==30 && post.timer==1 && !post.return_to_attract);
    post.timer=0;
    assert(nm_race_postresult_step(&post,0) && post.phase==2 && post.timer==UINT32_MAX && post.return_to_attract);
    puts("PASS native post-result control: record branch, signed timer boundaries, one-shot intents/invalid phase");
    nm_drive_params_t *math=nm_drive_load(drive_file);
    assert(math);
    for(unsigned angle=0;angle<65536;++angle) {
        nm_result_actor_t actor={0};
        actor.position[0]=(nm_result_vec2_t){(float)(angle%257)-128.0f,(float)(angle%131)*0.03125f};
        actor.angle[0]=(int16_t)angle;actor.other_angle[0]=(int16_t)(angle*37u);
        float matrix[12],reference[12];
        const float xyz[3]={actor.position[0].x,-0.01f,actor.position[0].y};
        const int16_t angles[3]={0,actor.other_angle[0],actor.angle[0]};
        assert(nm_body_pose_reference(math,xyz,angles,reference));
        assert(nm_result_actor_pose(math,&actor,0,matrix));
        for(unsigned i=0;i<12;++i)
            assert(isfinite(matrix[i]) && (i<9 ? fabsf(matrix[i]-1.2f*reference[i])<=1e-5f :
                nm_force_bits(matrix[i])==nm_force_bits(reference[i])));
        const float before=matrix[0];
        assert(!nm_result_actor_pose(math,&actor,8,matrix) && matrix[0]==before);
        actor.position[0].x=NAN;
        assert(!nm_result_actor_pose(math,&actor,0,matrix) && matrix[0]==before);
    }
    puts("PASS result poses:65536 mixed-angle chains, bounded rotation/exact translation, invalid input unchanged");
    nm_contact_pack_t contact={0};nm_road_pack_t road={0};nm_path_pack_t paths[8]={0};
    nm_scenery_pack_t scenery={0};
    char scenery_file[1024];snprintf(scenery_file,sizeof(scenery_file),"%s/scenery.dnv",directory);
    assert(nm_scenery_load(&scenery,scenery_file));
    assert(math && nm_contact_load(&contact,contact_file) && nm_road_load(&road,road_file));
    for(unsigned i=0;i<8;++i) {
        char name[1024];snprintf(name,sizeof(name),"%s/course0-route%u.dnp",directory,i);
        assert(nm_path_load(paths+i,name));
    }
    assert(road.grid_count==40);
    static nm_race_scene_t scene;
    static nm_race_car_t saved[40];
    nm_road_marks_t marks={calloc(road.profile_count,1),road.profile_count-1,0};assert(marks.values);
    const unsigned variants=
#ifdef NATIVE_INTENT_DATA
        3;
#else
        2;
#endif
    for(unsigned masked=0;masked<variants;++masked) {
    unsigned commands=0;
    uint32_t command_hash=UINT32_C(2166136261);
    assert(nm_race_scene_start(&scene,math,&contact,&road,paths,0,0,&marks,road.profile_count,NULL));
    nm_race_car_t *cars=scene.cars;
    const uint16_t next=cars[0].next,next_next=cars[next].next;
    nm_race_intent_output_t invalid_out={{123},1};
    cars[0].next=scene.count;
    assert(!nm_race_scene_events(&scene,NULL,0,0,NULL,NULL,0,NULL,0,&invalid_out));
    cars[0].next=next;cars[next].next=scene.count;
    assert(!nm_race_scene_events(&scene,NULL,0,0,NULL,NULL,0,NULL,0,&invalid_out));
    cars[next].next=next_next;
    assert(!scene.announcement_cursor && !scene.intents.repeats && invalid_out.count==1 && invalid_out.command[0]==123);
    for(unsigned i=0;i<16;++i) scene.shared.visible_cells[i]=UINT32_MAX;
    if(masked) {
        float view[12]={0},origin[2]={0};int16_t heading=0;uint16_t cell=0;nm_scenery_cells_t cells={0};
        assert(nm_follow_view(math,&cars[0].car,view,&heading));
        assert(nm_follow_scenery_origin(math,&cars[0].car,heading,origin,&cell));
        assert(nm_scenery_select(&scenery,math,cell,(uint16_t)cars[0].car.phase[0],
            origin[0],origin[1],heading,&cells));
        assert(nm_scenery_bitmaps(&cells,scene.shared.visible_cells));
    }
    nm_player_tick_input_t input={.active={.dynamics={.motion_scale=1},.proximity_disabled=1},.phase=22};
    input.active.controls.accel=255;
    memcpy(saved,cars,sizeof(saved));scene.rolling[39]=2;
    assert(!nm_race_movement_step(cars,40,&scene.shared,scene.rolling,&scene.start_flags,math,&nm_intro_speeds,
        &contact,&road,paths,&input,&scene.award,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count));
    assert(!memcmp(saved,cars,sizeof(saved)));scene.rolling[39]=1;
    assert(!nm_race_movement_step(cars,40,&scene.shared,scene.rolling,&scene.start_flags,math,&nm_intro_speeds,
        &contact,&road,paths,&input,&scene.award,&marks,road.profile_count-1,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count));
    assert(!memcmp(saved,cars,sizeof(saved)));
    unsigned released=0,candidates=0,moved=0;
    int16_t depth[44]={0};uint8_t order[44]={0},level[44]={0};
    const nm_player_t *players[40];
    for(unsigned i=0;i<40;++i) { depth[i]=INT16_MAX;order[i]=(uint8_t)i;players[i]=&cars[i].car; }
    unsigned detail_visible=0,high_ids_visible=0;
    uint32_t expected_view_ticks[4]={0};
    for(unsigned tick=0;tick<1800;++tick) {
        if(!(scene.start_flags&1u)) ++expected_view_ticks[scene.cars[0].car.camera_mode&3u];
        uint32_t previous_masks[16];memcpy(previous_masks,scene.shared.visible_cells,sizeof(previous_masks));
        if(nm_race_scene_step(&scene,math,&nm_intro_speeds,&contact,&road,paths,&input,
            0,0,0,tick&3,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count)!=1) {
            fprintf(stderr,"movement rejected tick=%u\n",tick);
            for(unsigned i=0;i<40;++i) fprintf(stderr,"car%u changed=%d profile=%d cell=%u route=%u rolling=%u\n",
                i,memcmp(saved+i,cars+i,sizeof(*cars))!=0,cars[i].car.phase[0],cars[i].car.scenery_cell,cars[i].route,scene.rolling[i]);
            return 1;
        }
        assert(!memcmp(previous_masks,scene.shared.visible_cells,sizeof(previous_masks)));
        assert(!memcmp(expected_view_ticks,scene.view_ticks,sizeof(expected_view_ticks)));
#ifdef NATIVE_INTENT_DATA
        if(masked==2) {
            nm_race_intent_output_t out;
            assert(nm_race_scene_events(&scene,nm_race_intent_rows,57,nm_race_intent_alternate[0],
                nm_race_intent_special[NM_RACE_INTENT_UNIT],nm_race_announcement_rows+nm_race_announcement_offsets[0],
                nm_race_announcement_counts[0],nm_race_announcement_ranks,input.award.penalty,&out));
            commands+=out.count;
            for(unsigned i=0;i<out.count;++i) command_hash=(command_hash^out.command[i])*UINT32_C(16777619);
        }
#endif
        nm_player_effect_timers(&cars[0].car);
        (void)nm_player_contact_clear(&cars[0].car); /* 9148, as code/practice.c */
#ifndef __DREAMCAST__
        if(tick==1199) {
            const nm_player_t *car=&cars[0].car;
            float player_pose[12],intro_pose[12];
            assert(nm_player_part_pose(math,car,0,player_pose));
            assert(nm_intro_body_pose_reference(math,car,intro_pose));
            assert(player_pose[1]>0.29f && intro_pose[1]<-0.25f);
            printf("ATTITUDE tick1200 variant=%u ground-roll=%d bank=%d render-pitch/yaw/roll=%d/%d/%d road-yaw=%d inhibit=%d overlay=%08lx crash=%d/%d/%d side-y=%g/%g\n",
                masked,car->boundary.ground.roll,car->bank,car->render_angles[0],car->render_angles[1],car->render_angles[2],
                car->road_yaw,car->inhibit_angle,(unsigned long)car->boundary.damage.overlay_model,
                car->boundary.damage.angle[0],car->boundary.damage.angle[1],car->boundary.damage.angle[2],
                (double)player_pose[1],(double)intro_pose[1]);
        }
#endif
        moved|=memcmp(saved,cars,sizeof(saved))!=0;
        for(unsigned i=0;i<40;++i) {
            assert(isfinite(cars[i].car.boundary.ground.position[0]));
            assert(cars[i].next<40 && cars[i].previous<40);
            assert(cars[cars[i].next].previous==i && cars[cars[i].previous].next==i);
            candidates+=cars[i].collision_candidates;
        }
        /* Actual strided race owners. Preserve the old all-visible baseline;
         * the second run publishes scenery after movement for the NEXT tick. */
        float view[12]={0},origin[2]={0};int16_t heading=0;uint16_t cell=0;nm_scenery_cells_t cells={0};
        assert(nm_follow_view(math,players[0],view,&heading));
        assert(nm_follow_scenery_origin(math,players[0],heading,origin,&cell));
        assert(nm_scenery_select(&scenery,math,cell,(uint16_t)players[0]->phase[0],
            origin[0],origin[1],heading,&cells));
        if(masked) assert(nm_scenery_bitmaps(&cells,scene.shared.visible_cells));
        int16_t expected_depth[44];uint8_t expected_order[44],requested[44]={0},ordered[44],selected[44];
        memcpy(expected_depth,depth,sizeof(depth));memcpy(expected_order,order,sizeof(order));
        for(unsigned i=0;i<40;++i) {
            const float *p=players[i]->boundary.ground.position;uint16_t car_cell=0;
            assert(nm_contact_scenery_cell(p[0],p[2],&car_cell));
            unsigned visible=0;
            for(unsigned c=0;c<cells.draw_count;++c) visible|=cells.draw[c]==car_cell;
            if(!visible) continue;
            int32_t q[3];
            for(unsigned a=0;a<3;++a) {
                const float value=p[0]*view[a]+p[1]*view[3+a]+p[2]*view[6+a]+view[9+a];
                assert(nm_drive_round_i32(a<2 ? fabsf(value) : value,q+a));
            }
            expected_depth[i]=(int16_t)q[2];requested[i]=(uint8_t)nm_intro_detail_level(q[0],q[1],q[2]);
        }
        for(unsigned end=40;end>1;--end) {
            const unsigned i=end-2;
            if(expected_depth[expected_order[i]]>expected_depth[expected_order[i+1]]) {
                const uint8_t old=expected_order[i];expected_order[i]=expected_order[i+1];expected_order[i+1]=old;
            }
        }
        for(unsigned i=0;i<40;++i) ordered[i]=requested[expected_order[i]];
        assert(nm_intro_detail_assign(ordered,40,selected));
        memcpy(saved,cars,sizeof(saved));
        assert(nm_car_detail_step(view,&cells,players,40,depth,order,level));
        assert(!memcmp(saved,cars,sizeof(saved)) && !memcmp(depth,expected_depth,40*sizeof(*depth)) &&
            !memcmp(order,expected_order,40));
        for(unsigned i=0;i<40;++i) {
            assert(level[order[i]]==selected[i]);
            detail_visible+=level[i]!=0;high_ids_visible+=i>=20 && level[i]!=0;
        }
    }
    for(unsigned i=0;i<40;++i) released+=!scene.rolling[i];
    assert(moved && released==40 && scene.shared.clock && scene.ticks==1800);
    assert(detail_visible && high_ids_visible);
    printf("PASS: borrowed40-car follow-view detail1800ticks visible=%u IDs20..39=%u; scalar depth/order/levels and read-only owners\n",
        detail_visible,high_ids_visible);
    assert(candidates && scene.collisions.pairs && scene.collisions.responses);
    printf("Native race events variant=%u commands=%u hash=%08lx cursor=%u repeats=%u\n",masked,commands,(unsigned long)command_hash,
        (unsigned)scene.announcement_cursor,(unsigned)scene.intents.repeats);
    /* Re-pinned 2026-10-11: the rolling-start sequence (ab78..ad30) now owns
     * signal 501a02=26 on leaving stage3; previously 8 commands, ab8dda88. */
    if(masked==2) assert(commands==9 && command_hash==UINT32_C(0xb0ddc70d) &&
        scene.announcement_cursor==3 && !scene.intents.repeats);
    /* Native cold-start regression, not original scene-transition parity. */
    printf("Native autonomous40-car movement/interactions1800ticks, masked=%u released=%u clock=%u remaining=%u candidate-count-sum=%u pairs=%u responses=%u; no render/full-race parity claim\n",
        masked,released,(unsigned)scene.shared.clock,(unsigned)scene.award.remaining,candidates,
        (unsigned)scene.collisions.pairs,(unsigned)scene.collisions.responses);
    /* Re-pinned 2026-10-11 after the start was corrected against the original
     * (c914..c938 front cars start from rest, phase20/21 entry ticks, 77c4
     * guided mode): the pack now runs into the slow front cars, as it does in
     * the reference trace. Previous totals 8623/8617, 1024/1026, 114. */
    assert(scene.shared.clock==1237 && scene.award.remaining==1963 &&
        candidates==(masked ? 11203u : 12416u) && scene.collisions.pairs==(masked ? 1656u : 1879u) &&
        scene.collisions.responses==(masked ? 448u : 369u));
    /* The final active tick is not skipped. Build an independent composition
     * of timer -> movement -> interactions; next-phase checkpoint gating and
     * old-phase motion flags intentionally differ. Following calls stop. */
    static nm_race_scene_t retained,expected;
    retained=scene;
    uint8_t *retained_marks=malloc(road.profile_count),*expected_values=malloc(road.profile_count);
    assert(retained_marks && expected_values);memcpy(retained_marks,marks.values,road.profile_count);
    const uint32_t retained_timer=marks.timer;
    for(unsigned finish=0;finish<2;++finish) {
        scene=retained;memcpy(marks.values,retained_marks,road.profile_count);marks.timer=retained_timer;
        scene.award.remaining=finish ? 100 : 0;scene.start_flags=0;
        scene.cars[0].car.checkpoints.finish_flags=finish;
        scene.cars[0].car.boundary.damage.flags|=1u<<21; // No award after next phase changes.
        expected=scene;memcpy(expected_values,marks.values,road.profile_count);
        nm_road_marks_t expected_marks={expected_values,marks.count,marks.timer};
        expected.active_phase=22;expected.shared.flags=1u<<22;
        ++expected.view_ticks[expected.cars[0].car.camera_mode&3u];
        expected.phase=nm_race_active_timer(input.award.suppression,0,0,
            &expected.award.remaining,&expected.shared.clock,&expected.cars[0].car.checkpoints.finish_flags);
        nm_player_tick_input_t final_input=input;final_input.phase=expected.phase;
        final_input.active.input_counter=nm_gears_signed(expected.award.remaining);
        expected.shared.tick=(uint16_t)expected.ticks;
        assert(nm_race_movement_step(expected.cars,40,&expected.shared,expected.rolling,&expected.start_flags,
            math,&nm_intro_speeds,&contact,&road,paths,&final_input,&expected.award,&expected_marks,
            road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count));
        assert(nm_race_interactions_step(expected.cars,40,&expected.shared,&expected.order,expected.cooldowns,
            &expected.passing,&expected.collisions,math,&road,input.award.penalty,0,0,input.active.dynamics.curve));
        ++expected.ticks;
        assert(nm_race_scene_step(&scene,math,&nm_intro_speeds,&contact,&road,paths,&input,
            0,0,0,0,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count)==1);
        assert(!memcmp(&scene,&expected,sizeof(scene)) && marks.timer==expected_marks.timer &&
            !memcmp(marks.values,expected_marks.values,road.profile_count));
        assert(scene.phase==(finish ? 23 : 25) && scene.active_phase==22 && scene.ticks==1801 &&
            !scene.award.cue_count && scene.award.remaining==(finish ? 99u : UINT32_MAX));
        for(unsigned id=0;id<scene.count;++id)
            assert(memcmp(scene.cars[id].car.boundary.ground.position,
                retained.cars[id].car.boundary.ground.position,3*sizeof(float)));
        if(!finish) {
        assert(nm_race_scene_step(&scene,math,&nm_intro_speeds,&contact,&road,paths,&input,
            0,0,0,0,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count)==2);
        assert(!memcmp(&scene,&expected,sizeof(scene)));
        } else {
            const uint32_t clock=scene.shared.clock,remaining=scene.award.remaining;
            uint32_t finish_view_ticks[4];memcpy(finish_view_ticks,scene.view_ticks,sizeof(finish_view_ticks));
            for(unsigned tick=0;tick<386;++tick) {
                const uint8_t next=tick==385 ? (scene.cars[0].rank>2 ? 25 : 27) : 24;
                assert(nm_race_scene_step(&scene,math,&nm_intro_speeds,&contact,&road,paths,&input,
                    0,0,0,tick&3,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count)==1);
                assert(scene.active_phase==(tick ? 24 : 23) && scene.phase==next && scene.rolling[0]==3);
                assert(scene.shared.flags==(1u<<scene.active_phase) && scene.shared.timer_hold==1);
                assert(scene.finish.timer==384u-tick && scene.finish_actor.timer==(uint16_t)(255-tick));
                assert(scene.finish_branch==(tick<200 ? 1 : 2) && scene.shared.clock==clock && scene.award.remaining==remaining);
                assert(!memcmp(finish_view_ticks,scene.view_ticks,sizeof(finish_view_ticks)));
                nm_player_effect_timers(&scene.cars[0].car);
            }
            expected=scene;
            assert(nm_race_scene_step(&scene,math,&nm_intro_speeds,&contact,&road,paths,&input,
                0,0,0,0,&marks,road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count)==2);
            assert(!memcmp(&scene,&expected,sizeof(scene)));
        }
    }
    scene=retained;memcpy(marks.values,retained_marks,road.profile_count);marks.timer=retained_timer;
    if(masked==0) {
        scene.rolling[0]=2;scene.shared.timer_hold=0x12345678;expected=scene;
        const nm_road_marks_t opponent_marks={marks.values,road.profile_count,marks.timer};
        nm_player_tick_input_t coast_input=input;coast_input.phase=24;
        for(unsigned tick=0;tick<384;++tick) {
            scene.shared.flags=expected.shared.flags=1u<<(tick ? 24 : 23);
            scene.shared.tick=expected.shared.tick=(uint16_t)(1800+tick);
            // Independent composition: source-checked player, publication,
            // then39 opponents, each seeing the previous car's shared writes.
            nm_player_t *player=&expected.cars[0].car;
            player->checkpoints.clock=expected.shared.clock;
            player->profile_updates=expected.shared.profile_updates;
            player->boundary.damage.sequence_counter=expected.shared.crash_counter;
            nm_player_active_input_t automatic=coast_input.active;
            automatic.dynamics.speed_flags=expected.shared.flags;
            automatic.dynamics.guided_mode=expected.shared.guided_mode;
            automatic.dynamics.guided_scale=expected.cars[0].guided_scale;
            automatic.dynamics.course_slot=road.slot;automatic.dynamics.entity_id=0;
            automatic.dynamics.lap=player->lap.completed;
            uint8_t entered=(uint8_t)(expected.rolling[0]==3);
            assert(nm_player_finish_tick(math,&contact,&road,paths+expected.cars[0].route,
                &automatic,player,&entered,&expected.shared.timer_hold));
            expected.rolling[0]=(uint8_t)(2+entered);
            expected.shared.clock=player->checkpoints.clock;
            expected.shared.profile_updates=player->profile_updates;
            expected.shared.crash_counter=player->boundary.damage.sequence_counter;
            memcpy(expected.shared.heading_circle,player->proximity_circle,sizeof(expected.shared.heading_circle));
            for(unsigned id=1;id<40;++id)
                assert(nm_race_opponent_step(expected.cars,40,id,&expected.shared,math,&nm_intro_speeds,
                    &contact,&road,paths+expected.cars[id].route,&opponent_marks,
                    nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count));
            assert(nm_race_movement_step(scene.cars,40,&scene.shared,scene.rolling,&scene.start_flags,
                math,&nm_intro_speeds,&contact,&road,paths,&coast_input,&scene.award,&marks,
                road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count));
            assert(!memcmp(&scene,&expected,sizeof(scene)) && scene.rolling[0]==3 && scene.shared.timer_hold==1);
            assert(marks.timer==retained_timer && !memcmp(marks.values,retained_marks,road.profile_count));
            nm_player_effect_timers(&scene.cars[0].car);nm_player_effect_timers(&expected.cars[0].car);
        }
        assert(memcmp(scene.cars[0].car.boundary.ground.position,retained.cars[0].car.boundary.ground.position,12));
        assert(memcmp(scene.cars[39].car.boundary.ground.position,retained.cars[39].car.boundary.ground.position,12));
        // Mismatched lifecycle/phase must reject BEFORE modifying any car.
        coast_input.phase=22;expected=scene;
        assert(!nm_race_movement_step(scene.cars,40,&scene.shared,scene.rolling,&scene.start_flags,
            math,&nm_intro_speeds,&contact,&road,paths,&coast_input,&scene.award,&marks,
            road.profile_count,nm_intro_crash_sequences[0].rows,nm_intro_crash_sequences[0].count));
        assert(!memcmp(&scene,&expected,sizeof(scene)));
        scene=retained;
        puts("PASS finish field movement:384 ticks40 cars, entry once, immediate shared publication, marks unchanged; phase entry/render excluded");
    }
    free(retained_marks);free(expected_values);
    puts("PASS phase lifecycle: final active tick preserved, finish23/24 coast386 ticks, timeout/results handoff stops unchanged");
    /* Source19320 predicate19414 observes the trigger BEFORE camera6c18
     * decrements it. A trigger of1 must still create the request this tick. */
    const nm_race_intent_row_t silent[1]={0};
    const nm_race_announcement_row_t trigger[1]={{INT32_MIN,5,3}};
    const uint32_t special[2]={0},ranks[9]={0};nm_race_intent_output_t out;
    scene.start_flags=0;scene.announcement_cursor=0;memset(&scene.intents,0,sizeof(scene.intents));
    cars[0].car.boundary.pivot_signal=cars[0].car.boundary.damage.signal=0;
    cars[0].car.force.trigger_count[1]=1;
    assert(nm_race_scene_events(&scene,silent,1,0,special,trigger,1,ranks,0,&out));
    assert(cars[0].car.boundary.pivot_signal==5 && cars[0].car.force.trigger_count[1]==1 && !out.count);
    nm_player_effect_timers(&cars[0].car);assert(!cars[0].car.force.trigger_count[1]);
    /* Compose the source-checked post-finish pieces on a real retained car.
     * This does not claim phase23 UI/entry or full-field finish integration. */
    if(masked==0) for(unsigned branch=0;branch<4;++branch) {
        nm_player_t coast=cars[0].car,reference=coast;
        coast.boundary.heading=nm_yaw_angle((uint32_t)((int)coast.boundary.ground.road_heading+
            (branch==NM_FINISH_REVERSE ? 20000 : 0)));
        coast.boundary.ground.yaw=coast.boundary.heading;
        coast.path_flags=branch==NM_FINISH_GUIDED ? 1 : 0;
        coast.boundary.damage.spin_ticks=branch==3 ? 8 : 0;reference=coast;
        nm_player_active_input_t automatic=input.active;
        automatic.dynamics.speed_flags=1u<<24;automatic.dynamics.guided_scale=cars[0].guided_scale;
        const nm_path_pack_t *path=paths+cars[0].route;
        uint8_t entered=0;uint32_t hold=0x12345678;
        /* Disabled entry must not read assets or reset pedals/path/timer. */
        automatic.dynamics.speed_flags=0;
        assert(nm_player_finish_tick(NULL,NULL,NULL,NULL,&automatic,&coast,&entered,&hold));
        nm_player_circle_step(&reference);
        assert(!memcmp(&coast,&reference,sizeof(coast)) && !entered && hold==0x12345678);
        automatic.dynamics.speed_flags=1u<<24;
        assert(!nm_player_finish_tick(NULL,NULL,NULL,NULL,&automatic,&coast,&entered,&hold));
        assert(!memcmp(&coast,&reference,sizeof(coast)) && !entered && hold==0x12345678);
        for(unsigned tick=0;tick<32;++tick) {
            if(!tick) assert(nm_player_finish_entry(&reference));
            assert(nm_player_finish_prefix(math,&contact,&road,path,&automatic,&reference));
            nm_player_circle_step(&reference);
            assert(nm_player_finish_tick(math,&contact,&road,path,&automatic,&coast,&entered,&hold));
            assert(!memcmp(&coast,&reference,sizeof(coast)) && entered==1 && hold==1);
            nm_player_effect_timers(&coast);nm_player_effect_timers(&reference);
        }
        printf("PASS native finish tick composition: branch=%u ticks=32 one entry/hold, disabled phase, atomic failure\n",branch);
    }
    }
    free(marks.values);
    for(unsigned i=0;i<8;++i) nm_path_unload(paths+i);
    nm_scenery_unload(&scenery);nm_road_unload(&road);nm_contact_unload(&contact);free(math);
    return 0;
}
