/* Hardware transition diagnostic only; link with --wrap=enj_ctrl_get_states. */
#include <enDjinn/enj_ctrl.h>
#include <stdio.h>

enj_ctrlr_state_t **__real_enj_ctrl_get_states(void);

enj_ctrlr_state_t **__wrap_enj_ctrl_get_states(void) {
    static uint64_t first_ns;
    static int sent;
    enj_ctrlr_state_t **states=__real_enj_ctrl_get_states();
    const uint64_t now=timer_ns_gettime64();
    if(!first_ns) first_ns=now;
    if(!sent && now-first_ns>=UINT64_C(5000000000)) {
        sent=1;
        for(size_t i=0;i<enj_ctrl_states_length();++i) if(states[i]) {
            states[i]->button.START=ENJ_BUTTON_DOWN_THIS_FRAME;
            puts("Transition diagnostic: SYNTHETIC START after5s; not physical input proof");
            return states;
        }
        puts("FAIL: transition diagnostic requires a connected controller");
    }
    return states;
}
