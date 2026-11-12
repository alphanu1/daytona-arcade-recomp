#ifndef PC_ENDJINN_CAPTURE_H
#define PC_ENDJINN_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queue a capture of the next submitted scene. The path is copied. Returns
 * false for an invalid path or while another capture remains pending. */
bool enj_pc_capture_next_frame(const char *path);

/* Number of scenes submitted by the PC backend. */
uint64_t enj_pc_presented_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif
