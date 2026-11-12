/* PC: the parts of KOS's arch/arch.h the PVR headers use. A retail console:
 * 8 MiB of video RAM. */
#ifndef PC_ARCH_ARCH_H
#define PC_ARCH_ARCH_H
#define ARCH_NAME "Dreamcast (PC)"
#define ARCH_EXIT_RETURN 1
#define ARCH_EXIT_MENU 2
#define ARCH_EXIT_REBOOT 3
#define HW_TYPE_RETAIL 0x0
#define HW_TYPE_SET5 0x9
static inline int hardware_sys_mode(int *region) { if (region) *region = 0; return HW_TYPE_RETAIL; }
#endif
