#ifdef NATIVE_INTENT_DATA
#include "race_intent_data.h"
#endif
#include "race_movement_check.h"

int main(int argc,char **argv) {
    assert(argc==5);
    return nm_race_movement_check(argv[1],argv[2],argv[3],argv[4]);
}
