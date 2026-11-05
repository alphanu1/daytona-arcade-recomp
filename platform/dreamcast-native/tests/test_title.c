/* cc -std=c11 platform/dreamcast-native/tests/test_title.c -o build/test-native-title
 * build/test-native-title */
#include "../include/title.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const int16_t motion[] = {-20, 3, 0}; /* Synthetic, not ROM data. */
    assert(title_tick(17383) == 0 && title_tick(17384) == 1);
    assert(title_tick(17384 * 1000ULL) == 1000);
    assert(title_pose(NULL, 0, UINT64_MAX).footer == 0);
    assert(title_pose(motion, 3, 0).logo_x == -20);
    assert(title_pose(motion, 3, 1).logo_x == 3);
    assert(!title_pose(motion, 3, 1).footer);
    assert(title_pose(motion, 3, 2).footer);
    assert(title_pose(motion, 3, UINT64_MAX).logo_x == 0);
    for (unsigned tick = 0; tick < 33; ++tick) assert(!title_pose(motion, 3, tick).prompt);
    for (unsigned tick = 33; tick < 97; ++tick)
        assert(title_pose(motion, 3, tick).prompt == (((tick - 33) / 8) % 2 == 0));
    puts("PASS: native title clock, motion bounds, footer and 8-tick prompt");
}
