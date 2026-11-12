/* Daytona USA PC: the PowerVR2 Tile Accelerator, as seen by the game.
 *
 * The game writes real TA parameters (KOS dc/pvr.h, compiled by KOS's own
 * pvr_prim.c, or prebuilt by the asset tools): 32-byte global parameters
 * (PCW, ISP/TSP, TSP, texture control) and vertex parameters, through
 * pvr_dr_target/pvr_dr_commit.  This decoder reads those words the way the
 * hardware does and turns each strip, sprite and modifier-volume triangle
 * into a QueuedPrimitive for the Vulkan renderer.  Textures are read from an
 * emulated 8 MiB VRAM at the address the texture control word names, with
 * the palette and the cheap-shadow scale in an emulated register file.
 *
 * Derived from enDjinn's backends/host-common/host_pvr.{h,cpp} (MIT), which
 * decoded pc-enDjinn's private header encoding instead.
 */
#ifndef DAYTONA_PC_HOST_PVR_H
#define DAYTONA_PC_HOST_PVR_H

#include <kos.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace enj_host_pvr {

struct QueuedPrimitive {
  float x[4];
  float y[4];
  float z[4];
  float u[4];
  float v[4];
  uint32_t color[4];
  uint32_t count;
  pvr_list_t list;
  pvr_cull_mode_t culling;
  /* ISP/TSP word: PVR_DEPTHCMP_* (NEVER..ALWAYS) and depth write. */
  uint8_t depth_func;
  bool depth_test;  /* false only for PVR_DEPTHCMP_ALWAYS without write */
  bool depth_write;
  /* TSP word. */
  uint8_t blend_src; /* PVR_BLEND_* 0..7 */
  uint8_t blend_dst;
  uint8_t tex_env;   /* PVR_TXRENV_* */
  bool use_alpha;    /* vertex alpha used (PVR_ALPHA_ENABLE) */
  bool ignore_tex_alpha;
  uint8_t uv_clamp;  /* bit 1 U, bit 0 V (PVR_UVCLAMP_*) */
  uint8_t uv_flip;   /* bit 1 U, bit 0 V (PVR_UVFLIP_*) */
  uint8_t mip_bias;  /* 4-bit D multiplier, 2 fraction bits */
  bool supersample;
  bool alpha_cutout; /* punch-through list */
  bool textured;
  pvr_ptr_t texture; /* host address in emulated VRAM */
  uint32_t texture_format; /* texture control word without the address */
  uint32_t texture_width;
  uint32_t texture_height;
  pvr_filter_mode_t texture_filter;
  /* Modifier volumes.  A cheap-shadow receiver is queued twice: area 0
   * (modifier=false) and area 1 (modifier=true), drawn darkened by the
   * shadow scale where the volumes cover it. */
  bool modifier_receiver;
  bool modifier;
  bool modifier_volume;
  bool modifier_volume_last;
  uint32_t modifier_mode;
};

struct DecodedMip {
  uint32_t width;
  uint32_t height;
  std::vector<uint8_t> pixels;
};

struct DecodedTexture {
  bool indexed;
  bool bump;
  uint32_t palette_base;
  std::vector<DecodedMip> mips;
};

const std::vector<QueuedPrimitive> &primitives();
void scene_begin();
void list_begin(pvr_list_t list);
void *dr_target();
void dr_commit(void *ptr);

/* Emulated VRAM. */
uint8_t *vram_base();
void *texture_alloc(size_t size);
void texture_free(pvr_ptr_t ptr);
size_t texture_available();
void texture_load(const void *src, pvr_ptr_t dst, size_t count);
void texture_load_ex(const void *src, pvr_ptr_t dst, uint32_t width,
                     uint32_t height, uint32_t flags);
void vram_touched(const void *dst, size_t count);
uint64_t texture_revision(pvr_ptr_t ptr);

/* Palette RAM lives in the register file (PVR_PALETTE_TABLE_BASE). */
uint64_t palette_revision();
uint64_t palette_revision(uint32_t texture_format);
std::array<uint32_t, 1024> palette_rgba();

/* FPU_SHAD_SCALE: the colour multiplier inside cheap-shadow volumes. */
float shadow_scale();

bool decode_texture(const QueuedPrimitive &primitive, DecodedTexture &decoded);

}  // namespace enj_host_pvr

extern "C" volatile uint32_t pc_pvr_regs[0x2000 / 4];

#endif
