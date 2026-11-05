#ifndef DAYTONA_NATIVE_STARTUP_CHECK_H
#define DAYTONA_NATIVE_STARTUP_CHECK_H
/* Optional target startup suites. Included after NM_ASSET_ROOT is defined;
 * no flags means this reduces to a successful no-op. Failures stop startup. */
#ifdef NATIVE_INTENT_DATA
#include "race_intent_data.h"
#endif
#ifdef NATIVE_RACE_MOVEMENT_CHECK
#include "race_movement_check.h"
#endif
#ifdef NATIVE_ROAD_CHECK
#include "road_check.h"
#endif
#ifdef NATIVE_DRIVE_CHECK
#include "drive_check.h"
#endif
#ifdef NATIVE_CONTACT_CHECK
#include "contact_check.h"
#ifdef NATIVE_INTENT_DATA
#include "race_intent_check.h"
#endif
#include "boundary_check.h"
/* Failure-only host fixtures; never inputs to gameplay or replacements for
 * actual target results. Compare generated inputs too (not just outputs). */
static void contact_diagnostic(const nm_contact_pack_t *pack,const nm_drive_params_t *math,int boundary) {
    const char *path=boundary ? NM_ASSET_ROOT "boundary.oracle" : NM_ASSET_ROOT "contact.oracle";
    FILE *f=fopen(path,"rb");
    if(!f) { printf("Diagnostic unavailable: %s\n",path);return; }
    uint32_t header[3];
    const unsigned count=boundary ? 65536 : nm_contact_test_count(pack),words=boundary ? 12 : 8,inputs=boundary ? 8 : 3;
    if(setvbuf(f,NULL,_IOFBF,65536) || fread(header,4,3,f)!=3 ||
       header[0]!=UINT32_C(0x31445046) || header[1]!=count || header[2]!=words) {
        puts("FAIL diagnostic header");fclose(f);return;
    }
    unsigned input_diff=0,output_diff=0,nan_only=0,other=0;
    for(unsigned i=0;i<count;++i) {
        uint32_t expected[12],actual[12];
        if(fread(expected,4,words,f)!=words) { puts("FAIL diagnostic truncated");fclose(f);return; }
        if(boundary) {
            float points[8];nm_boundary_test_input(i,points);memcpy(actual,points,32);
            nm_boundary_hit_t result={{17,-19},0};
            actual[8]=(uint32_t)nm_boundary_intersection(math,points,&result);
            actual[9]=nm_force_bits(result.point[0]);actual[10]=nm_force_bits(result.point[1]);actual[11]=(uint32_t)result.hit;
        } else {
            float input[3];nm_contact_test_input(pack,i,input);memcpy(actual,input,12);
            nm_contact_result_t result={input[2],0x1234,0x5678,0xabcdef};
            actual[3]=(uint32_t)nm_contact_query(pack,math,input[0],input[1],&result);memcpy(actual+4,&result,16);
        }
        const int changed_input=memcmp(actual,expected,inputs*4)!=0;
        const int changed_output=memcmp(actual+inputs,expected+inputs,(words-inputs)*4)!=0;
        input_diff+=changed_input;output_diff+=changed_output;
        int only_nan=changed_output;
        for(unsigned j=inputs;j<words;++j) if(actual[j]!=expected[j]) {
            const int floating=boundary ? j==9 || j==10 : j==4;
            only_nan&=floating && (actual[j]&0x7fffffffu)>0x7f800000u && (expected[j]&0x7fffffffu)>0x7f800000u;
        }
        nan_only+=only_nan;
        if((changed_input || (changed_output && !only_nan)) && other++<8) {
            printf("FP diagnostic %s case=%u input=%d\n",boundary ? "boundary" : "contact",i,changed_input);
            for(unsigned j=0;j<words;++j) printf("  word%u host=%08lx target=%08lx\n",j,(unsigned long)expected[j],(unsigned long)actual[j]);
        }
    }
    if(fgetc(f)!=EOF || ferror(f)) puts("FAIL diagnostic trailing data/read error");
    else printf("FP diagnostic %s cases=%u input_diff=%u output_diff=%u nan_only=%u other=%u\n",
        boundary ? "boundary" : "contact",count,input_diff,output_diff,nan_only,other);
    fclose(f);
}
#endif
#ifdef NATIVE_PATH_CHECK
#include "path_check.h"
#include "guided_check.h"
#endif

static int nm_startup_check(void) {
#ifdef NATIVE_PATH_CHECK
    nm_path_pack_t path = {0};
    nm_drive_params_t *path_math = nm_drive_load(NM_ASSET_ROOT "drive.dnd");
    if (!path_math || !nm_path_load(&path,NM_ASSET_ROOT "path.dnp")) {
        free(path_math); puts("FAIL: native path data load"); return 1;
    }
    const uint64_t path_started = timer_ns_gettime64();
    const uint32_t path_hash = nm_path_test_hash(&path,path_math);
    const int path_ok = path_hash == nm_path_expected_hash(path.slot,path.route);
    printf("Native path: course=%lu route=%lu cases=%u hash=%08lx %s check=%llu us payload=%lu bytes (cursor startup check, not gameplay)\n",
        (unsigned long)path.slot,(unsigned long)path.route,nm_path_test_count(&path),(unsigned long)path_hash,
        path_ok ? "PASS" : "FAIL",(unsigned long long)((timer_ns_gettime64()-path_started)/1000),
        (unsigned long)path.records*28);
    const uint64_t guided_started = timer_ns_gettime64();
    const uint32_t guided_hash = nm_guided_test_hash(&path,path_math);
    const int guided_ok = guided_hash == nm_guided_expected_hash(path.slot,path.route);
    printf("Native guided player: cases=%u hash=%08lx %s check=%llu us (includes rejected conversions, not gameplay)\n",
        NM_GUIDED_CHECK_CASES,(unsigned long)guided_hash,guided_ok ? "PASS" : "FAIL",
        (unsigned long long)((timer_ns_gettime64()-guided_started)/1000));
    nm_path_unload(&path); free(path_math);
    if (!path_ok || !guided_ok) return 1;
#endif
#ifdef NATIVE_RACE_MOVEMENT_CHECK
    puts("Native whole-field startup check: 1800 ticks (not visible gameplay)");
    if(nm_race_movement_check(NM_ASSET_ROOT "drive.dnd",NM_ASSET_ROOT "course0.dnc",
        NM_ASSET_ROOT "course0.dnr",NM_ASSET_ROOT)) return 1;
#endif
#ifdef NATIVE_CONTACT_CHECK
    nm_contact_pack_t contact = {0};
    nm_drive_params_t *contact_math = nm_drive_load(NM_ASSET_ROOT "drive.dnd");
    if (!contact_math || !nm_contact_load(&contact,NM_ASSET_ROOT "course.dnc")) {
        free(contact_math); puts("FAIL: native contact data load"); return 1;
    }
    const uint64_t contact_started = timer_ns_gettime64();
    const uint32_t contact_hash = nm_contact_test_hash(&contact,contact_math);
    const uint32_t ground_hash = nm_wheels_test_hash(&contact,contact_math);
    const uint32_t boundary_hash = nm_boundary_test_hash(contact_math);
    const uint32_t boundary_observable_hash = nm_boundary_observable_test_hash(contact_math);
    const uint32_t candidate_hash = nm_collision_candidate_test_hash(contact_math);
    const uint32_t detection_hash = nm_collision_detect_test_hash(contact_math);
    const uint32_t preparation_hash = nm_collision_prepare_test_hash();
    const uint32_t walk_hash = nm_collision_walk_test_hash(contact_math);
    const uint32_t traffic_hash = nm_traffic_order_test_hash();
    const uint32_t traffic_near_hash = nm_traffic_near_test_hash(contact_math);
    const uint32_t timers_hash = nm_race_timers_test_hash(), passing_hash = nm_traffic_pass_test_hash(contact_math);
    const uint32_t body_angles_hash = nm_body_angles_test_hash(contact_math);
    const uint32_t tilt_hash = nm_tilt_test_hash(contact_math);
    const uint32_t road_marks_hash = nm_road_marks_test_hash();
    const uint32_t player_end_hash = nm_player_end_test_hash();
    int intents_ok=nm_race_intents_test();
#ifdef NATIVE_INTENT_DATA
    const uint32_t intent_hash=nm_race_intent_target_hash(nm_race_intent_rows,
        nm_race_intent_alternate,nm_race_intent_special,NULL);
    intents_ok=intents_ok && intent_hash==UINT32_C(0xaafb1c14);
    printf("Native race-intent ownership: 65536 cases hash=%08lx expected=aafb1c14 %s (startup only)\n",
        (unsigned long)intent_hash,intents_ok ? "PASS" : "FAIL");
    const uint32_t announcement_hash=nm_race_announcement_target_hash(nm_race_announcement_rows,
        nm_race_announcement_offsets,nm_race_announcement_counts,nm_race_announcement_ranks);
    const int announcement_ok=announcement_hash==NM_RACE_ANNOUNCEMENT_HASH;
    printf("Native race-announcement creator: 44544 cases hash=%08lx expected=%08lx %s (startup only)\n",
        (unsigned long)announcement_hash,(unsigned long)NM_RACE_ANNOUNCEMENT_HASH,
        announcement_ok ? "PASS" : "FAIL");
    intents_ok=intents_ok && announcement_ok;
    const uint32_t tail_hash=nm_race_intent_target_hash(nm_race_intent_rows,
        nm_race_intent_alternate,nm_race_intent_special,nm_race_announcement_rows);
    const int tail_ok=tail_hash==UINT32_C(0x2069215a);
    printf("Native combined race-event tail: 65536 cases hash=%08lx expected=2069215a %s (startup only)\n",
        (unsigned long)tail_hash,tail_ok ? "PASS" : "FAIL");
    intents_ok=intents_ok && tail_ok;
#endif
    const int contact_ok = contact_hash == nm_contact_expected_hash(contact.slot) && ground_hash == nm_wheels_expected_hash(contact.slot) &&
        boundary_observable_hash == NM_BOUNDARY_OBSERVABLE_HASH && candidate_hash == NM_COLLISION_CANDIDATE_HASH &&
        detection_hash == NM_COLLISION_DETECT_HASH && preparation_hash == NM_COLLISION_PREPARE_HASH &&
        walk_hash == NM_COLLISION_WALK_HASH &&
        traffic_hash == NM_TRAFFIC_ORDER_HASH && traffic_near_hash == NM_TRAFFIC_NEAR_HASH &&
        timers_hash == NM_RACE_TIMERS_HASH && passing_hash == NM_TRAFFIC_PASS_HASH && body_angles_hash == NM_BODY_ANGLES_HASH &&
        tilt_hash == NM_TILT_HASH && road_marks_hash == NM_ROAD_MARKS_HASH && player_end_hash == NM_PLAYER_END_HASH && intents_ok;
    printf("Native silent race intents: 65536 timer values + boundary checks %s (startup only)\n",intents_ok ? "PASS" : "FAIL");
    printf("Native contact: slot=%lu cases=%u hash=%08lx %s check=%llu us payload=%lu bytes (startup test, not frame timing)\n",
        (unsigned long)contact.slot,nm_contact_test_count(&contact),(unsigned long)contact_hash,
        contact_hash==nm_contact_expected_hash(contact.slot) ? "PASS" : "FAIL",(unsigned long long)((timer_ns_gettime64()-contact_started)/1000),
        (unsigned long)contact.payload_bytes);
    printf("Native ground: cases=%lu hash=%08lx expected=%08lx state=%u bytes\n",
        (unsigned long)contact.quad_count*4,(unsigned long)ground_hash,
        (unsigned long)nm_wheels_expected_hash(contact.slot),(unsigned)sizeof(nm_wheels_state_t));
    printf("Native boundary raw scratch: cases=65536 hash=%08lx reference=%08lx (includes unconsumed miss coordinates)\n",
        (unsigned long)boundary_hash,(unsigned long)NM_BOUNDARY_EXPECTED_HASH);
    printf("Native boundary observable: cases=65536 poisoned_states=131072 hash=%08lx expected=%08lx %s (bit-exact hit coordinates)\n",
        (unsigned long)boundary_observable_hash,(unsigned long)NM_BOUNDARY_OBSERVABLE_HASH,
        boundary_observable_hash==NM_BOUNDARY_OBSERVABLE_HASH ? "PASS" : "FAIL");
    printf("Native collision broad-phase: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_COLLISION_CANDIDATE_CASES,(unsigned long)candidate_hash,(unsigned long)NM_COLLISION_CANDIDATE_HASH);
    printf("Native collision detection: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_COLLISION_DETECT_CASES,(unsigned long)detection_hash,(unsigned long)NM_COLLISION_DETECT_HASH);
    printf("Native collision preparation: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_COLLISION_PREPARE_CASES,(unsigned long)preparation_hash,(unsigned long)NM_COLLISION_PREPARE_HASH);
    printf("Native collision walk: cases=%u hash=%08lx expected=%08lx %s (synthetic response mutations, not gameplay)\n",
        NM_COLLISION_WALK_CASES,(unsigned long)walk_hash,(unsigned long)NM_COLLISION_WALK_HASH,
        walk_hash==NM_COLLISION_WALK_HASH ? "PASS" : "FAIL");
    printf("Native traffic ordering: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_TRAFFIC_ORDER_CASES,(unsigned long)traffic_hash,(unsigned long)NM_TRAFFIC_ORDER_HASH);
    printf("Native traffic proximity: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_TRAFFIC_NEAR_CASES,(unsigned long)traffic_near_hash,(unsigned long)NM_TRAFFIC_NEAR_HASH);
    printf("Native body angles: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_BODY_ANGLES_CASES,(unsigned long)body_angles_hash,(unsigned long)NM_BODY_ANGLES_HASH);
    printf("Native tilt: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_TILT_CASES,(unsigned long)tilt_hash,(unsigned long)NM_TILT_HASH);
    printf("Native road marks: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_ROAD_MARKS_CASES,(unsigned long)road_marks_hash,(unsigned long)NM_ROAD_MARKS_HASH);
    printf("Native player end: cases=%u hash=%08lx expected=%08lx (startup test, not gameplay)\n",
        NM_PLAYER_END_CASES,(unsigned long)player_end_hash,(unsigned long)NM_PLAYER_END_HASH);
    if(!contact_ok || boundary_hash!=NM_BOUNDARY_EXPECTED_HASH) contact_diagnostic(&contact,contact_math,1);
    if(!contact_ok) contact_diagnostic(&contact,contact_math,0);
    nm_contact_unload(&contact); free(contact_math);
    printf("Native race timers/passing: %08lx/%08lx expected=%08lx/%08lx (startup only)\n",
        (unsigned long)timers_hash,(unsigned long)passing_hash,(unsigned long)NM_RACE_TIMERS_HASH,(unsigned long)NM_TRAFFIC_PASS_HASH);
    if (!contact_ok) { puts("FAIL: native contact/boundary startup suite");return 1; }
#endif
#ifdef NATIVE_DRIVE_CHECK
    const uint64_t drive_load_started = timer_ns_gettime64();
    nm_drive_params_t *drive = nm_drive_load(NM_ASSET_ROOT "drive.dnd");
    if (!drive) { puts("FAIL: native drive parameter load"); return 1; }
    const uint64_t drive_check_started = timer_ns_gettime64();
    const uint32_t drive_hash = nm_drive_test_hash(drive);
    const uint64_t drive_checked = timer_ns_gettime64();
    const uint32_t axle_hash = nm_axle_test_hash(drive);
    const uint64_t axle_checked = timer_ns_gettime64();
    const uint32_t force_hash = nm_force_test_hash(drive);
    const uint64_t force_checked = timer_ns_gettime64();
    const uint32_t divide_hash = nm_divide_test_hash(drive);
    const uint64_t divide_checked = timer_ns_gettime64();
    const uint32_t forces_hash = nm_forces_test_hash(drive);
    const uint64_t forces_checked = timer_ns_gettime64();
    const uint32_t traction_hash = nm_traction_test_hash(drive);
    const uint64_t traction_checked = timer_ns_gettime64();
    const uint32_t speed_hash = nm_speed_test_hash(drive);
    const uint64_t speed_checked = timer_ns_gettime64();
    const uint32_t yaw_hash = nm_yaw_test_hash(drive);
    const uint64_t yaw_checked = timer_ns_gettime64();
    const uint32_t tires_hash = nm_tires_test_hash(drive);
    const uint64_t tires_checked = timer_ns_gettime64();
    const uint32_t gears_hash = nm_gears_test_hash(drive);
    const uint64_t gears_checked = timer_ns_gettime64();
    const uint32_t resistance_hash = nm_resistance_test_hash(drive);
    const uint32_t proximity_hash = nm_proximity_test_hash(drive);
    const uint64_t proximity_checked = timer_ns_gettime64();
    const uint32_t motion_hash = nm_motion_test_hash(drive);
    const uint64_t motion_checked = timer_ns_gettime64();
    printf("Native drive terms: %u cases hash=%08lx %s params=%zu load=%llu us check=%llu us (not frame timing)\n",
           NM_DRIVE_CHECK_CASES,(unsigned long)drive_hash,
           drive_hash == NM_DRIVE_CHECK_HASH ? "PASS" : "FAIL",sizeof(*drive),
           (unsigned long long)((drive_check_started-drive_load_started)/1000),
           (unsigned long long)((drive_checked-drive_check_started)/1000));
    printf("Native axle load: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_AXLE_CHECK_CASES,(unsigned long)axle_hash,
           axle_hash == NM_AXLE_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((axle_checked-drive_checked)/1000));
    printf("Native force math: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_FORCE_CHECK_CASES,(unsigned long)force_hash,
           force_hash == NM_FORCE_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((force_checked-axle_checked)/1000));
    printf("Native division: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_DIVIDE_CHECK_CASES,(unsigned long)divide_hash,
           divide_hash == NM_DIVIDE_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((divide_checked-force_checked)/1000));
    printf("Native forces: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_FORCES_CHECK_CASES,(unsigned long)forces_hash,
           forces_hash == NM_FORCES_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((forces_checked-divide_checked)/1000));
    printf("Native traction: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_TRACTION_CHECK_CASES,(unsigned long)traction_hash,
           traction_hash == NM_TRACTION_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((traction_checked-forces_checked)/1000));
    printf("Native speed/RPM: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_SPEED_CHECK_CASES,(unsigned long)speed_hash,
           speed_hash == NM_SPEED_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((speed_checked-traction_checked)/1000));
    printf("Native yaw: %u cases hash=%08lx %s check=%llu us (not frame timing; includes rejected inputs)\n",
           NM_YAW_CHECK_CASES,(unsigned long)yaw_hash,
           yaw_hash == NM_YAW_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((yaw_checked-speed_checked)/1000));
    free(drive);
    printf("Native tires: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_TIRES_CHECK_CASES,(unsigned long)tires_hash,
           tires_hash == NM_TIRES_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((tires_checked-yaw_checked)/1000));
    printf("Native gears: %u cases hash=%08lx %s check=%llu us (not frame timing)\n",
           NM_GEARS_CHECK_CASES,(unsigned long)gears_hash,
           gears_hash == NM_GEARS_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((gears_checked-tires_checked)/1000));
    printf("Native force inputs: resistance256 hash=%08lx %s proximity%u hash=%08lx %s check=%llu us (not frame timing)\n",
           (unsigned long)resistance_hash,resistance_hash == NM_RESISTANCE_CHECK_HASH ? "PASS" : "FAIL",
           NM_PROXIMITY_CHECK_CASES,(unsigned long)proximity_hash,
           proximity_hash == NM_PROXIMITY_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((proximity_checked-gears_checked)/1000));
    printf("Native motion: %u cases hash=%08lx %s check=%llu us (normal/spin/held, not guided movement or frame timing)\n",
           NM_MOTION_CHECK_CASES,(unsigned long)motion_hash,motion_hash == NM_MOTION_CHECK_HASH ? "PASS" : "FAIL",
           (unsigned long long)((motion_checked-proximity_checked)/1000));
    if (motion_hash != NM_MOTION_CHECK_HASH || drive_hash != NM_DRIVE_CHECK_HASH || axle_hash != NM_AXLE_CHECK_HASH ||
        force_hash != NM_FORCE_CHECK_HASH || divide_hash != NM_DIVIDE_CHECK_HASH ||
        forces_hash != NM_FORCES_CHECK_HASH || traction_hash != NM_TRACTION_CHECK_HASH ||
        speed_hash != NM_SPEED_CHECK_HASH || yaw_hash != NM_YAW_CHECK_HASH ||
        tires_hash != NM_TIRES_CHECK_HASH || gears_hash != NM_GEARS_CHECK_HASH ||
        resistance_hash != NM_RESISTANCE_CHECK_HASH || proximity_hash != NM_PROXIMITY_CHECK_HASH) return 1;
#endif
#ifdef NATIVE_ROAD_CHECK
    const uint32_t road_hash = nm_road_test_hash();
    printf("Native road lateral/spawn check: %u ROM-free cases hash=%08lx %s\n",
           NM_ROAD_CHECK_CASES, (unsigned long)road_hash,
           road_hash == NM_ROAD_CHECK_HASH ? "PASS" : "FAIL");
    if (road_hash != NM_ROAD_CHECK_HASH) return 1;
    const uint32_t controls_hash = nm_controls_test_hash();
    printf("Native vehicle controls check: %u ROM-free cases hash=%08lx %s\n",
           NM_CONTROLS_CHECK_CASES, (unsigned long)controls_hash,
           controls_hash == NM_CONTROLS_CHECK_HASH ? "PASS" : "FAIL");
    if (controls_hash != NM_CONTROLS_CHECK_HASH) return 1;
    const uint32_t checkpoint_hash = nm_checkpoint_test_hash();
    printf("Native checkpoints: %u ROM-free cases hash=%08lx %s\n",
           NM_CHECKPOINT_CASES,(unsigned long)checkpoint_hash,
           checkpoint_hash == NM_CHECKPOINT_HASH ? "PASS" : "FAIL");
    if (checkpoint_hash != NM_CHECKPOINT_HASH) return 1;
#endif
    return 0;
}
#endif
