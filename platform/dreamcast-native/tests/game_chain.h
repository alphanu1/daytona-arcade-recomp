/* Host harnesses only: what one run of a scene leaves the next, so that a game after a race, or one started from the
 * demo or the title, is run scene by scene as the console runs them, each harness with the packs of its course:
 *   trace_race_native ... --frontend TRACE --leave A             the first game, through the results
 *   trace_attract_native ... --enter A --credit TRACE --leave B  the attract after it, to the next credit
 *   trace_race_native ... --enter B --frontend TRACE             the next game: its select and its race
 * The file is a native struct: only for the harness binaries of one build, ROM-derived, under build/. */
#ifndef DAYTONA_NATIVE_GAME_CHAIN_H
#define DAYTONA_NATIVE_GAME_CHAIN_H
#include "frontend_pool.h"
#include <stdio.h>

typedef struct {
    uint32_t magic;   /* NM_CHAIN_MAGIC */
    uint32_t counter; /* 5010b0 of the last iteration run: the game's phase 29, or the credit */
    uint32_t course;  /* 501460 */
    uint32_t notification; /* 501398 of the game's record: the trace's notify= until the next game's phase 25 */
    nm_entity_pool_t pool;
} nm_game_chain_t;
enum { NM_CHAIN_MAGIC=0x434e4847 };

static inline int nm_chain_write(const char *path,const nm_game_chain_t *state) {
    FILE *out=fopen(path,"wx");
    if(!out) return 0;
    const int ok=fwrite(state,sizeof(*state),1,out)==1;
    return !fclose(out) && ok;
}
static inline int nm_chain_read(const char *path,nm_game_chain_t *state) {
    FILE *in=fopen(path,"r");
    if(!in) return 0;
    const int ok=fread(state,sizeof(*state),1,in)==1 && fgetc(in)==EOF && state->magic==NM_CHAIN_MAGIC;
    return !fclose(in) && ok;
}
#endif
