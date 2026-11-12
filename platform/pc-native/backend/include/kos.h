#ifndef PC_ENDJINN_KOS_H
#define PC_ENDJINN_KOS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/purupuru.h>
#include <dc/maple/vmu.h>
#include <dc/perf_monitor.h>
#include <dc/pvr.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/video.h>

#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#define INIT_DEFAULT 0u
#define KOS_INIT_FLAGS(flags)
#define ARCH_EXIT_REBOOT 3

typedef int file_t;

typedef struct vmu_pkg {
  char desc_long[128];
  char desc_short[32];
  char app_id[16];
  int icon_cnt;
  const uint8_t *icon_data;
  int icon_anim_speed;
  size_t data_len;
  const uint8_t *data;
  int eyecatch_type;
  const uint8_t *eyecatch_data;
} vmu_pkg_t;

#define VMUPKG_EC_16COL 1

PC_ENDJINN_BEGIN_DECLS
void arch_set_exit_path(int path);
file_t fs_open(const char *path, int mode);
int fs_close(file_t file);
ssize_t fs_read(file_t file, void *buffer, size_t bytes);
ssize_t fs_write(file_t file, const void *buffer, size_t bytes);
off_t fs_seek(file_t file, off_t offset, int whence);
off_t fs_tell(file_t file);
int fs_unlink(const char *path);
int vmu_pkg_load_icon(vmu_pkg_t *pkg, const char *path);
int vmu_pkg_build(const vmu_pkg_t *pkg, uint8_t **output, int *size);
PC_ENDJINN_END_DECLS

#endif
