#pragma once
#include <stdint.h>
int nm_result_labels_load(const char *root);
void nm_result_labels_unload(void);
/* Writes the label palettes (entries 960..). Call once at the result hand-off,
 * after nm_result_materials_palettes_apply; repeated calls are no-ops. */
int nm_result_labels_palettes_apply(void);
int nm_result_labels_enqueue(unsigned rank,uint32_t total,int16_t best,int16_t scroll);
