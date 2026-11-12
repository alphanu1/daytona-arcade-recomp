/* PC: the platform-independent image type KOS's PVR header names. */
#ifndef PC_KOS_IMG_H
#define PC_KOS_IMG_H
#include <stdint.h>
#include <stddef.h>
typedef struct kos_img {
    void *data;
    uint32_t w, h;
    uint32_t fmt;
    size_t byte_count;
} kos_img_t;
#define KOS_IMG_FMT_I(x) ((x) & 0xffff)
#define KOS_IMG_FMT_MASK 0xffff
#endif
