#ifndef DAYTONA_NATIVE_NAME_LABELS_H
#define DAYTONA_NATIVE_NAME_LABELS_H
#include <stdint.h>

typedef struct {
    uint16_t bank,x,y,width,height,texture_width,texture_height,foreground;
} nm_name_label_part_t;
_Static_assert(sizeof(nm_name_label_part_t)==16,"two UI parts per cache line");
/* Load at a scene boundary; banks60..63 must be available. Parts stay borrowed
 * until unload. Static parts followed by10 countdown,12 time and14 rank glyphs.
 * Root includes its trailing slash. No per-frame uploads. */
int nm_name_labels_load(const char *root,const nm_name_label_part_t *parts,unsigned count,unsigned static_count);
/* Prepare record text once after load, before enqueue. Rank is table slot1..24. */
int nm_name_labels_record(unsigned rank,uint32_t total,uint16_t best,int show_best);
/* -1 hides countdown during entrance;0..20 displays original right-aligned text. */
int nm_name_labels_enqueue(int seconds);
int nm_name_labels_unload(void);
#endif
