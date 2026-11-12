#ifndef PC_ENDJINN_DC_PERF_MONITOR_H
#define PC_ENDJINN_DC_PERF_MONITOR_H

#include <stdio.h>

#include <pc_endjinn/types.h>

#define PMCR_OPERAND_CACHE_READ_MISS_MODE 0x04
#define PMCR_INSTRUCTION_CACHE_MISS_MODE 0x08

PC_ENDJINN_BEGIN_DECLS
void perf_monitor_init(int event1, int event2);
void perf_monitor_print(FILE *output);
PC_ENDJINN_END_DECLS

#endif
