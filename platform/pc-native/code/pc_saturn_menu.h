/* The PC build's Saturn-style front end: Circuit Edition's menus in the original Saturn Daytona USA's (1995) look
 * (platform/pc-native/tools/prepare_saturn_menus.py makes the art: ROOT/saturn-menu/). The title before them is the
 * arcade's own (its prompt PRESS START BUTTON in the arcade's font: include/attract_2d.h). */
#ifndef DAYTONA_PC_SATURN_MENU_H
#define DAYTONA_PC_SATURN_MENU_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Circuit Edition's main menu, Options (and its Sound page) and Records in the original's style: from the title's
 * START, and after a race left from the pause. 1 when it started; available: the art is in the asset set. */
int pc_saturn_menu_available(void);
int pc_saturn_menu_start(void);
/* The race's pause (Circuit Edition's EXIT ? YES / NO / RESTART, cursor 0 YES, 1 NO, 2 RESTART) over the frozen race. */
int pc_saturn_pause_load(void);
int pc_saturn_pause_enqueue(unsigned cursor);
int pc_saturn_pause_release(void);
#ifdef __cplusplus
}
#endif
#endif
