/* Hardware input diagnostic; wrap title start too for natural timeout -> loader. */
#ifdef NM_RACE_CONTROLS_CHECK
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int8_t joyx,joyy;uint8_t ltrigger,rtrigger;unsigned buttons; } enj_ctrlr_state_t;
#else
#include <enDjinn/enj_state.h>
#include <stdio.h>
#endif

enj_ctrlr_state_t **__real_enj_ctrl_get_states(void);
size_t enj_ctrl_states_length(void);
#ifdef NM_RACE_CONTROLS_CHECK
static uint64_t timer_ns_gettime64(void);
static void enj_state_flag_shutdown(void *unused);
#endif

int __wrap_nm_title_mode_start(void) {
    puts("Natural timeout diagnostic: title handoff reached; returning to loader");
    enj_state_flag_shutdown(NULL);
    return 1;
}

enj_ctrlr_state_t **__wrap_enj_ctrl_get_states(void) {
    static int announced,clock_started,timed_out;
    static uint64_t first_ns;
    const uint64_t now=timer_ns_gettime64();
    if(!clock_started) { first_ns=now;clock_started=1; }
    /* Eight laps include authored time extensions; this only bounds failed tests. */
    if(!timed_out && now-first_ns>=UINT64_C(300000000000)) {
        timed_out=1;
        puts("FAIL: natural timeout diagnostic watchdog300s; expected title handoff missing");
        enj_state_flag_shutdown(NULL);
    }
    enj_ctrlr_state_t **states=__real_enj_ctrl_get_states();
    for(size_t i=0;i<enj_ctrl_states_length();++i) if(states[i]) {
        states[i]->joyx=0;states[i]->ltrigger=0;states[i]->rtrigger=255;
        if(!announced) {
            puts("Race input diagnostic: SYNTHETIC full gas, centered steering; not physical calibration");
            announced=1;
        }
        break;
    }
    return states;
}

#ifdef NM_RACE_CONTROLS_CHECK
static enj_ctrlr_state_t *states[4];
static uint64_t now_ns;
static unsigned shutdowns;
static uint64_t timer_ns_gettime64(void) { return now_ns; }
static void enj_state_flag_shutdown(void *unused) { assert(!unused);++shutdowns; }
enj_ctrlr_state_t **__real_enj_ctrl_get_states(void) { return states; }
size_t enj_ctrl_states_length(void) { return 4; }
int main(void) {
    assert(__wrap_enj_ctrl_get_states()==states); /* Engine getter precedes first Maple update. */
    enj_ctrlr_state_t first={-127,23,200,17,0x1234},second={9,19,7,11,0x5678};
    states[1]=&first;states[3]=&second;
    for(unsigned i=0;i<10;++i) {
        first.joyx=-127;first.ltrigger=200;first.rtrigger=17;
        assert(__wrap_enj_ctrl_get_states()==states);
        assert(first.joyx==0 && first.ltrigger==0 && first.rtrigger==255);
        assert(first.joyy==23 && first.buttons==0x1234);
        assert(second.joyx==9 && second.ltrigger==7 && second.rtrigger==11);
    }
    assert(!shutdowns);
    assert(__wrap_nm_title_mode_start()==1 && shutdowns==1);
    now_ns=UINT64_C(300000000000)-1;
    __wrap_enj_ctrl_get_states();assert(shutdowns==1);
    ++now_ns;
    __wrap_enj_ctrl_get_states();assert(shutdowns==2);
    __wrap_enj_ctrl_get_states();assert(shutdowns==2);
    puts("PASS: controller selection/buttons; title handoff exit; watchdog exact boundary/one shot");
}
#endif
