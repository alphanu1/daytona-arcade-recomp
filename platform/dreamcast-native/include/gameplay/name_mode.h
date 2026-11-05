#ifndef DAYTONA_GAMEPLAY_NAME_MODE_H
#define DAYTONA_GAMEPLAY_NAME_MODE_H
#include "drive.h"
#include "race_records.h"
/* Called at an empty render-queue boundary after retiring race resources.
 * Borrowed math, record and failure owner must outlive the enDjinn mode. */
int nm_name_mode_start(const char *root,nm_drive_params_t *math,
    uint32_t notification,nm_race_record_t *record,uint16_t best_lap,
    unsigned completed,int *failed);
#endif
