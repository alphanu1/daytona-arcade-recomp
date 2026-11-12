#ifndef PC_ENDJINN_TYPES_H
#define PC_ENDJINN_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t uint32;
typedef uint8_t uint8;

#ifdef __cplusplus
#define PC_ENDJINN_BEGIN_DECLS extern "C" {
#define PC_ENDJINN_END_DECLS }
#else
#define PC_ENDJINN_BEGIN_DECLS
#define PC_ENDJINN_END_DECLS
#endif

#endif
