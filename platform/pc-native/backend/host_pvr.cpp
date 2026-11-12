/* Daytona USA PC: Tile Accelerator decoder, emulated VRAM and registers.
 * See host_pvr.h.  Texture decoding (twiddle, VQ, mip offsets, 16-bit and
 * YUV formats) is enDjinn's host_pvr.cpp (MIT), now reading VRAM directly. */
#include "host_pvr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

extern "C" {
volatile uint32_t pc_pvr_regs[0x2000 / 4];
}

namespace {

/* ------------------------------------------------------------------ VRAM */

constexpr size_t kVramSize = 8u << 20;
constexpr size_t kVramAlign = 16u << 20; /* low 24 address bits = VRAM offset */

uint8_t *g_vram = nullptr;

uint8_t *vram() {
  if (g_vram == nullptr) {
#if defined(_WIN32)
    g_vram = static_cast<uint8_t *>(_aligned_malloc(kVramAlign, kVramAlign));
#else
    void *p = nullptr;
    if (posix_memalign(&p, kVramAlign, kVramAlign) == 0) g_vram = static_cast<uint8_t *>(p);
#endif
    if (g_vram == nullptr) {
      std::fprintf(stderr, "daytona: cannot reserve emulated VRAM\n");
      std::abort();
    }
    std::memset(g_vram, 0, kVramAlign);
  }
  return g_vram;
}

struct Block {
  size_t size;
  bool used;
  uint64_t revision;
};
std::map<size_t, Block> g_blocks; /* offset -> block, covering all of VRAM */
uint64_t g_texture_revision = 1u;
/* Freed blocks wait for the next scene: a held frame still draws them. */
std::vector<size_t> g_freed;

void blocks_init() {
  if (g_blocks.empty()) g_blocks[0] = {kVramSize, false, 0u};
}

std::map<size_t, Block>::iterator block_at(const void *p) {
  blocks_init();
  const uint8_t *base = vram();
  const uint8_t *q = static_cast<const uint8_t *>(p);
  if (q < base || q >= base + kVramSize) return g_blocks.end();
  const size_t off = static_cast<size_t>(q - base);
  auto it = g_blocks.upper_bound(off);
  if (it == g_blocks.begin()) return g_blocks.end();
  --it;
  return off < it->first + it->second.size ? it : g_blocks.end();
}

void release_block(size_t off) {
  auto it = g_blocks.find(off);
  if (it == g_blocks.end() || !it->second.used) return;
  it->second.used = false;
  auto next = std::next(it);
  if (next != g_blocks.end() && !next->second.used) {
    it->second.size += next->second.size;
    g_blocks.erase(next);
  }
  if (it != g_blocks.begin()) {
    auto prev = std::prev(it);
    if (!prev->second.used) {
      prev->second.size += it->second.size;
      g_blocks.erase(it);
    }
  }
}

/* ------------------------------------------------------- texture decoding */

uint32_t log2_size(uint32_t value) {
  uint32_t result = 0u;
  while (value > 1u) {
    value >>= 1u;
    result++;
  }
  return result;
}

size_t mip_offset(uint32_t pixel_format, bool vq, uint32_t level) {
  static constexpr size_t offsets[] = {0x00006u, 0x00008u, 0x00010u, 0x00030u,
                                       0x000b0u, 0x002b0u, 0x00ab0u, 0x02ab0u,
                                       0x0aab0u, 0x2aab0u, 0xaaab0u};
  size_t offset = offsets[std::min<size_t>(level, 10u)];
  if (vq) return offset / 8u;
  if (pixel_format == PVR_PIXEL_MODE_PAL_4BPP) return offset / 4u;
  if (pixel_format == PVR_PIXEL_MODE_PAL_8BPP) return offset / 2u;
  return offset;
}

size_t level_bytes(uint32_t pixel_format, uint32_t width, uint32_t height) {
  const size_t pixels = static_cast<size_t>(width) * height;
  if (pixel_format == PVR_PIXEL_MODE_PAL_4BPP) return (pixels + 1u) / 2u;
  if (pixel_format == PVR_PIXEL_MODE_PAL_8BPP) return pixels;
  return pixels * 2u;
}

size_t twiddled_index(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  const uint32_t xb = log2_size(width);
  const uint32_t yb = log2_size(height);
  const uint32_t shared = std::min(xb, yb);
  size_t index = 0u;
  for (uint32_t bit = 0u; bit < shared; bit++) {
    index |= static_cast<size_t>((y >> bit) & 1u) << (bit * 2u);
    index |= static_cast<size_t>((x >> bit) & 1u) << (bit * 2u + 1u);
  }
  if (xb > shared) {
    index |= static_cast<size_t>(x >> shared) << (shared * 2u);
  } else if (yb > shared) {
    index |= static_cast<size_t>(y >> shared) << (shared * 2u);
  }
  return index;
}

uint8_t expand4(uint32_t value) { return static_cast<uint8_t>(value * 17u); }
uint8_t expand5(uint32_t value) { return static_cast<uint8_t>((value << 3u) | (value >> 2u)); }
uint8_t expand6(uint32_t value) { return static_cast<uint8_t>((value << 2u) | (value >> 4u)); }

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8u) |
         (static_cast<uint32_t>(b) << 16u) | (static_cast<uint32_t>(a) << 24u);
}

uint32_t decode_16bit(uint16_t pixel, uint32_t format) {
  switch (format) {
    case PVR_PIXEL_MODE_ARGB1555:
      return rgba(expand5((pixel >> 10u) & 31u), expand5((pixel >> 5u) & 31u),
                  expand5(pixel & 31u), (pixel & 0x8000u) != 0u ? 255u : 0u);
    case PVR_PIXEL_MODE_RGB565:
      return rgba(expand5((pixel >> 11u) & 31u), expand6((pixel >> 5u) & 63u),
                  expand5(pixel & 31u), 255u);
    case PVR_PIXEL_MODE_ARGB4444:
      return rgba(expand4((pixel >> 8u) & 15u), expand4((pixel >> 4u) & 15u),
                  expand4(pixel & 15u), expand4((pixel >> 12u) & 15u));
    default:
      return rgba(static_cast<uint8_t>(pixel), static_cast<uint8_t>(pixel >> 8u), 0u, 255u);
  }
}

uint8_t clamp_byte(int value) { return static_cast<uint8_t>(std::min(std::max(value, 0), 255)); }

void decode_yuv_pair(uint16_t left, uint16_t right, uint32_t *out) {
  const int y0 = left >> 8u;
  const int y1 = right >> 8u;
  const int u = static_cast<int>(left & 0xffu) - 128;
  const int v = static_cast<int>(right & 0xffu) - 128;
  const auto convert = [&](int y) {
    return rgba(clamp_byte(y + ((91881 * v) >> 16)),
                clamp_byte(y - ((22554 * u + 46802 * v) >> 16)),
                clamp_byte(y + ((116130 * u) >> 16)), 255u);
  };
  out[0] = convert(y0);
  out[1] = convert(y1);
}

/* One level's texel stream (twiddled or linear, VQ expanded) from VRAM. */
bool source_level(const uint8_t *tex, size_t available, uint32_t format, uint32_t width,
                  uint32_t height, uint32_t level, std::vector<uint8_t> &bytes) {
  const uint32_t pixel_format = (format >> 27u) & 7u;
  const bool vq = (format & PVR_TXRFMT_VQ_ENABLE) != 0u;
  const bool mipmapped = (format & 0x80000000u) != 0u;
  const size_t size = level_bytes(pixel_format, width, height);
  if (!vq) {
    const size_t offset = mipmapped ? mip_offset(pixel_format, false, level) : 0u;
    if (offset > available || size > available - offset) return false;
    bytes.assign(tex + offset, tex + offset + size);
    return true;
  }
  /* VQ: a 2 KiB codebook (256 entries of 2x2 texels), then the indices. */
  const size_t index_count = (size + 7u) / 8u;
  const size_t offset = 2048u + (mipmapped ? mip_offset(pixel_format, true, level) : 0u);
  if (offset > available || index_count > available - offset) return false;
  bytes.resize(index_count * 8u);
  for (size_t i = 0u; i < index_count; i++) {
    std::memcpy(bytes.data() + i * 8u, tex + static_cast<size_t>(tex[offset + i]) * 8u, 8u);
  }
  return true;
}

bool decode_level(const std::vector<uint8_t> &source, uint32_t format, uint32_t width,
                  uint32_t height, enj_host_pvr::DecodedMip &decoded) {
  const uint32_t pixel_format = (format >> 27u) & 7u;
  const bool indexed = pixel_format == PVR_PIXEL_MODE_PAL_4BPP ||
                       pixel_format == PVR_PIXEL_MODE_PAL_8BPP;
  const bool vq = (format & PVR_TXRFMT_VQ_ENABLE) != 0u;
  const bool twiddled = indexed || vq || (format & PVR_TXRFMT_NONTWIDDLED) == 0u;
  const size_t pixels = static_cast<size_t>(width) * height;
  decoded.width = width;
  decoded.height = height;
  decoded.pixels.resize(pixels * (indexed ? 1u : 4u));

  std::vector<uint32_t> converted;
  if (!indexed) {
    converted.resize(pixels);
    if (pixel_format == PVR_PIXEL_MODE_YUV422) {
      const uint16_t *words = reinterpret_cast<const uint16_t *>(source.data());
      if (twiddled) {
        for (size_t i = 0u; i + 3u < pixels; i += 4u) {
          uint32_t first[2], second[2];
          decode_yuv_pair(words[i], words[i + 2u], first);
          decode_yuv_pair(words[i + 1u], words[i + 3u], second);
          converted[i] = first[0];
          converted[i + 1u] = second[0];
          converted[i + 2u] = first[1];
          converted[i + 3u] = second[1];
        }
      } else {
        for (size_t i = 0u; i + 1u < pixels; i += 2u) decode_yuv_pair(words[i], words[i + 1u], &converted[i]);
      }
    } else {
      for (size_t i = 0u; i < pixels; i++) {
        uint16_t word;
        std::memcpy(&word, source.data() + i * 2u, sizeof(word));
        converted[i] = decode_16bit(word, pixel_format);
      }
    }
  }
  for (uint32_t y = 0u; y < height; y++) {
    for (uint32_t x = 0u; x < width; x++) {
      const size_t dst = static_cast<size_t>(y) * width + x;
      const size_t src = twiddled ? twiddled_index(x, y, width, height) : dst;
      if (indexed) {
        decoded.pixels[dst] = pixel_format == PVR_PIXEL_MODE_PAL_4BPP
            ? static_cast<uint8_t>((source[src >> 1u] >> ((src & 1u) * 4u)) & 15u)
            : source[src];
      } else {
        std::memcpy(decoded.pixels.data() + dst * 4u, &converted[src], 4u);
      }
    }
  }
  return true;
}

/* --------------------------------------------------------- the TA itself */

struct Header {
  bool sprite = false;
  bool textured = false;
  bool uv16 = false;
  bool gouraud = true;
  uint32_t color_format = 0u; /* PVR_CLRFMT_* */
  bool shadow = false;        /* cheap-shadow receiver (modifier, not two-volume) */
  bool two_volume = false;
  uint32_t isp = 0u, tsp = 0u, tcw = 0u;
  uint32_t base_color = 0xffffffffu; /* sprites; intensity face colour */
  float face[4] = {1.f, 1.f, 1.f, 1.f};
  pvr_list_t list = PVR_LIST_OP_POLY;
};

struct Vtx {
  float x, y, z, u, v;
  uint32_t argb;
};

alignas(32) std::array<uint8_t, 96> g_dr_packet{};
uint32_t g_dr_slot = 0u;
pvr_list_t g_current_list = PVR_LIST_OP_POLY;
Header g_header;
std::vector<Vtx> g_strip;
std::array<uint32_t, 8> g_pending{}; /* first half of a 64-byte parameter */
bool g_has_pending = false;
std::vector<enj_host_pvr::QueuedPrimitive> g_primitives;

float as_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

uint32_t argb_from_floats(float a, float r, float g, float b) {
  const auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
  return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

void unpack_uv16(uint32_t packed, float &u, float &v) {
  u = as_float(packed & 0xffff0000u);
  v = as_float(packed << 16u);
}

bool is_modifier_list(pvr_list_t list) { return list == PVR_LIST_OP_MOD || list == PVR_LIST_TR_MOD; }

void fill_state(enj_host_pvr::QueuedPrimitive &p) {
  const Header &h = g_header;
  p.list = h.list;
  p.depth_func = static_cast<uint8_t>((h.isp >> 29) & 7u);
  p.culling = static_cast<pvr_cull_mode_t>((h.isp >> 27) & 3u);
  p.depth_write = ((h.isp >> 26) & 1u) == 0u; /* bit 26 is "Z write disable" */
  p.depth_test = !(p.depth_func == PVR_DEPTHCMP_ALWAYS && !p.depth_write);
  p.blend_src = static_cast<uint8_t>((h.tsp >> 29) & 7u);
  p.blend_dst = static_cast<uint8_t>((h.tsp >> 26) & 7u);
  p.use_alpha = ((h.tsp >> 20) & 1u) != 0u;
  p.ignore_tex_alpha = ((h.tsp >> 19) & 1u) != 0u;
  p.uv_flip = static_cast<uint8_t>((h.tsp >> 17) & 3u);
  p.uv_clamp = static_cast<uint8_t>((h.tsp >> 15) & 3u);
  p.texture_filter = static_cast<pvr_filter_mode_t>((h.tsp >> 13) & 3u);
  p.supersample = ((h.tsp >> 12) & 1u) != 0u;
  p.mip_bias = static_cast<uint8_t>((h.tsp >> 8) & 15u);
  p.tex_env = static_cast<uint8_t>((h.tsp >> 6) & 3u);
  p.textured = h.textured;
  if (h.textured) {
    p.texture_width = 8u << ((h.tsp >> 3) & 7u);
    p.texture_height = 8u << (h.tsp & 7u);
    const size_t address = static_cast<size_t>(h.tcw & 0x1fffffu) << 3;
    p.texture = vram() + (address & (kVramSize - 1u));
    p.texture_format = h.tcw & 0xffe00000u;
  }
  p.alpha_cutout = h.list == PVR_LIST_PT_POLY;
  p.modifier_receiver = h.shadow && h.list == PVR_LIST_OP_POLY;
  p.modifier = false;
  p.modifier_volume = false;
  p.modifier_volume_last = false;
  p.modifier_mode = 0u;
}

void queue(enj_host_pvr::QueuedPrimitive &p) {
  g_primitives.push_back(p);
  if (p.modifier_receiver) {
    /* Area 1: the same polygon, darkened by the shadow scale where covered. */
    p.modifier = true;
    g_primitives.push_back(p);
  }
}

void queue_triangle(const Vtx &a, const Vtx &b, const Vtx &c) {
  enj_host_pvr::QueuedPrimitive p{};
  p.count = 3u;
  fill_state(p);
  const Vtx *v[3] = {&a, &b, &c};
  for (uint32_t i = 0; i < 3u; i++) {
    p.x[i] = v[i]->x;
    p.y[i] = v[i]->y;
    p.z[i] = v[i]->z;
    p.u[i] = v[i]->u;
    p.v[i] = v[i]->v;
    /* Flat shading: the colour of the triangle's last vertex. */
    p.color[i] = g_header.gouraud ? v[i]->argb : c.argb;
  }
  queue(p);
}

void strip_vertex(const Vtx &vtx, bool end_of_strip) {
  g_strip.push_back(vtx);
  const size_t n = g_strip.size();
  if (n >= 3u) {
    /* Strip triangles keep one winding: odd ones swap their first two. */
    if ((n & 1u) == 0u) {
      queue_triangle(g_strip[n - 2u], g_strip[n - 3u], g_strip[n - 1u]);
    } else {
      queue_triangle(g_strip[n - 3u], g_strip[n - 2u], g_strip[n - 1u]);
    }
  }
  if (end_of_strip) g_strip.clear();
}

void polygon_vertex(const uint32_t *w) {
  Vtx v{};
  v.x = as_float(w[1]);
  v.y = as_float(w[2]);
  v.z = as_float(w[3]);
  const Header &h = g_header;
  if (h.textured) {
    if (h.uv16) {
      unpack_uv16(w[4], v.u, v.v);
    } else {
      v.u = as_float(w[4]);
      v.v = as_float(w[5]);
    }
  }
  switch (h.color_format) {
    case PVR_CLRFMT_ARGBPACKED:
      v.argb = w[6];
      break;
    case PVR_CLRFMT_4FLOATS:
      /* Untextured only: a, r, g, b in words 4..7. */
      v.argb = argb_from_floats(as_float(w[4]), as_float(w[5]), as_float(w[6]), as_float(w[7]));
      break;
    default: { /* intensity: face colour times the vertex intensity */
      const float i = as_float(h.textured ? w[6] : w[4]);
      v.argb = argb_from_floats(h.face[0], h.face[1] * i, h.face[2] * i, h.face[3] * i);
      break;
    }
  }
  strip_vertex(v, (w[0] & 0x10000000u) != 0u);
}

/* Second half of a sprite: words 8..15 of pvr_sprite_txr_t/pvr_sprite_col_t. */
void sprite(const uint32_t *a, const uint32_t *b) {
  enj_host_pvr::QueuedPrimitive p{};
  p.count = 4u;
  fill_state(p);
  p.x[0] = as_float(a[1]); p.y[0] = as_float(a[2]); p.z[0] = as_float(a[3]);
  p.x[1] = as_float(a[4]); p.y[1] = as_float(a[5]); p.z[1] = as_float(a[6]);
  p.x[2] = as_float(a[7]); p.y[2] = as_float(b[0]); p.z[2] = as_float(b[1]);
  p.x[3] = as_float(b[2]); p.y[3] = as_float(b[3]);
  /* D lies in the plane of A, B, C: D = A + C - B. */
  p.z[3] = p.z[0] + p.z[2] - p.z[1];
  if (g_header.textured) {
    unpack_uv16(b[5], p.u[0], p.v[0]);
    unpack_uv16(b[6], p.u[1], p.v[1]);
    unpack_uv16(b[7], p.u[2], p.v[2]);
    p.u[3] = p.u[0] + p.u[2] - p.u[1];
    p.v[3] = p.v[0] + p.v[2] - p.v[1];
  }
  for (uint32_t i = 0; i < 4u; i++) p.color[i] = g_header.base_color;
  queue(p);
}

/* Second half of a modifier-volume triangle (pvr_modifier_vol_t). */
void modifier_triangle(const uint32_t *a, const uint32_t *b) {
  enj_host_pvr::QueuedPrimitive p{};
  p.count = 3u;
  p.list = g_header.list;
  p.culling = static_cast<pvr_cull_mode_t>((g_header.isp >> 27) & 3u);
  p.depth_func = PVR_DEPTHCMP_GREATER;
  p.depth_test = true;
  p.depth_write = false;
  p.modifier_volume = true;
  /* ISP word bits 30..29: 0 normal polygon, 1 inside last polygon, 2 outside last polygon. */
  p.modifier_mode = (g_header.isp >> 29) & 3u;
  p.modifier_volume_last = p.modifier_mode != PVR_MODIFIER_OTHER_POLY;
  p.x[0] = as_float(a[1]); p.y[0] = as_float(a[2]); p.z[0] = as_float(a[3]);
  p.x[1] = as_float(a[4]); p.y[1] = as_float(a[5]); p.z[1] = as_float(a[6]);
  p.x[2] = as_float(a[7]); p.y[2] = as_float(b[0]); p.z[2] = as_float(b[1]);
  g_primitives.push_back(p);
}

void global_parameter(const uint32_t *w) {
  const uint32_t pcw = w[0];
  Header h;
  const uint32_t list = (pcw >> 24) & 7u;
  h.list = list <= PVR_LIST_PT_POLY ? static_cast<pvr_list_t>(list) : g_current_list;
  h.sprite = (pcw >> 29) == 5u;
  h.textured = (pcw & 0x8u) != 0u;
  h.uv16 = (pcw & 0x1u) != 0u;
  h.gouraud = (pcw & 0x2u) != 0u;
  h.color_format = (pcw >> 4) & 3u;
  h.shadow = (pcw & 0x80u) != 0u && (pcw & 0x40u) == 0u;
  h.two_volume = (pcw & 0xc0u) == 0xc0u;
  h.isp = w[1];
  h.tsp = w[2];
  h.tcw = w[3];
  if (h.sprite) {
    h.base_color = w[4];
    h.gouraud = true;
  } else if (h.color_format == PVR_CLRFMT_INTENSITY) {
    h.face[0] = as_float(w[4]);
    h.face[1] = as_float(w[5]);
    h.face[2] = as_float(w[6]);
    h.face[3] = as_float(w[7]);
  } else if (h.color_format == PVR_CLRFMT_INTENSITY_PREV) {
    std::memcpy(h.face, g_header.face, sizeof h.face);
  }
  if (h.two_volume) {
    static unsigned reported = 0;
    if (++reported <= 2) std::fprintf(stderr, "daytona: two-volume polygons are drawn with their outside parameters\n");
  }
  g_header = h;
  g_strip.clear();
  g_has_pending = false;
}

}  // namespace

namespace enj_host_pvr {

const std::vector<QueuedPrimitive> &primitives() { return g_primitives; }

void scene_begin() {
  for (size_t off : g_freed) release_block(off);
  g_freed.clear();
  g_primitives.clear();
  g_strip.clear();
  g_has_pending = false;
}

void list_begin(pvr_list_t list) {
  g_current_list = list;
  g_header.list = list;
  g_strip.clear();
  g_has_pending = false;
}

void *dr_target() {
  g_dr_slot ^= 1u;
  return g_dr_packet.data() + 32u * (g_dr_slot + 1u);
}

void dr_commit(void *ptr) {
  if (ptr == nullptr) return;
  uint32_t w[8];
  std::memcpy(w, ptr, sizeof w);
  if (g_has_pending) {
    g_has_pending = false;
    if (g_header.sprite) {
      sprite(g_pending.data(), w);
    } else if (is_modifier_list(g_header.list)) {
      modifier_triangle(g_pending.data(), w);
    }
    /* Two-volume vertices: the second half carries the inside area. */
    return;
  }
  const uint32_t type = w[0] >> 29;
  switch (type) {
    case 4u: /* polygon or modifier-volume header */
    case 5u: /* sprite header */
      global_parameter(w);
      return;
    case 7u: /* vertex */
      if (g_header.sprite || is_modifier_list(g_header.list)) {
        std::memcpy(g_pending.data(), w, sizeof w);
        g_has_pending = true;
        return;
      }
      polygon_vertex(w);
      if (g_header.two_volume) {
        /* 64-byte vertex: skip its inside half. */
        g_pending = {};
        g_has_pending = true;
      }
      return;
    default: /* end of list, user clip, object list set */
      g_strip.clear();
      return;
  }
}

uint8_t *vram_base() { return vram(); }

void *texture_alloc(size_t size) {
  blocks_init();
  if (size == 0u) return nullptr;
  /* As KOS's pvr_mem: 32-byte granules plus one granule of bookkeeping per
   * block. The game checks pvr_mem_available() against that exactly (the sky
   * loader: four pages cost four times the file size, 32-byte header included). */
  const size_t need = ((size + 31u) & ~size_t{31u}) + 32u;
  for (auto it = g_blocks.begin(); it != g_blocks.end(); ++it) {
    if (it->second.used || it->second.size < need) continue;
    const size_t off = it->first;
    const size_t rest = it->second.size - need;
    it->second = {need, true, ++g_texture_revision};
    if (rest) g_blocks[off + need] = {rest, false, 0u};
    return vram() + off;
  }
  std::fprintf(stderr, "daytona: emulated VRAM full (%zu bytes wanted)\n", size);
  return nullptr;
}

void texture_free(pvr_ptr_t ptr) {
  auto it = block_at(ptr);
  if (it != g_blocks.end() && it->second.used) g_freed.push_back(it->first);
}

size_t texture_available() {
  blocks_init();
  size_t total = 0u;
  for (const auto &[off, block] : g_blocks) {
    if (!block.used) total += block.size;
  }
  return total;
}

void vram_touched(const void *dst, size_t count) {
  if (count == 0u) return;
  const uint8_t *p = static_cast<const uint8_t *>(dst);
  for (auto it = block_at(p); it != g_blocks.end(); ++it) {
    if (vram() + it->first >= p + count) break;
    it->second.revision = ++g_texture_revision;
  }
}

void texture_load(const void *src, pvr_ptr_t dst, size_t count) {
  if (src == nullptr || dst == nullptr) return;
  uint8_t *d = static_cast<uint8_t *>(dst);
  if (d < vram() || d + count > vram() + kVramSize) return;
  std::memcpy(d, src, count);
  vram_touched(d, count);
}

/* KOS pvr_texture.c's twiddling loader (Marcus' tatest / Vincent Penne). */
#define TWIDTAB(x) ((x & 1) | ((x & 2) << 1) | ((x & 4) << 2) | ((x & 8) << 3) | ((x & 16) << 4) | \
                    ((x & 32) << 5) | ((x & 64) << 6) | ((x & 128) << 7) | ((x & 256) << 8) | ((x & 512) << 9))
#define TWIDOUT(x, y) (TWIDTAB((y)) | (TWIDTAB((x)) << 1))
void texture_load_ex(const void *src, pvr_ptr_t dst, uint32_t w, uint32_t h, uint32_t flags) {
  if (src == nullptr || dst == nullptr) return;
  const uint32_t min = std::min(w, h), mask = min - 1u;
  const bool invert = (flags & PVR_TXRLOAD_INVERT_Y) != 0u;
  uint16_t *vtex = static_cast<uint16_t *>(dst);
  size_t bytes = 0u;
  switch (flags & PVR_TXRLOAD_FMT_MASK) {
    case PVR_TXRLOAD_4BPP: {
      const uint8_t *pixels = static_cast<const uint8_t *>(src);
      for (uint32_t y = 0; y < h; y += 2) {
        const uint32_t yout = invert ? (h - 1) - y : y;
        for (uint32_t x = 0; x < w; x += 2) {
          vtex[TWIDOUT((x & mask) / 2, (yout & mask) / 2) + (x / min + yout / min) * min * min / 4] =
              (pixels[(x + y * w) >> 1] & 15) | ((pixels[(x + (y + 1) * w) >> 1] & 15) << 4) |
              ((pixels[(x + y * w) >> 1] >> 4) << 8) | ((pixels[(x + (y + 1) * w) >> 1] >> 4) << 12);
        }
      }
      bytes = static_cast<size_t>(w) * h / 2;
      break;
    }
    case PVR_TXRLOAD_8BPP: {
      const uint8_t *pixels = static_cast<const uint8_t *>(src);
      for (uint32_t y = 0; y < h; y += 2) {
        const uint32_t yout = invert ? (h - 1) - y : y;
        for (uint32_t x = 0; x < w; x++) {
          vtex[TWIDOUT((yout & mask) / 2, x & mask) + (x / min + yout / min) * min * min / 2] =
              pixels[y * w + x] | (pixels[(y + 1) * w + x] << 8);
        }
      }
      bytes = static_cast<size_t>(w) * h;
      break;
    }
    default: {
      const uint16_t *pixels = static_cast<const uint16_t *>(src);
      for (uint32_t y = 0; y < h; y++) {
        const uint32_t yout = invert ? (h - 1) - y : y;
        for (uint32_t x = 0; x < w; x++) {
          vtex[TWIDOUT(x & mask, yout & mask) + (x / min + yout / min) * min * min] = pixels[y * w + x];
        }
      }
      bytes = static_cast<size_t>(w) * h * 2;
      break;
    }
  }
  vram_touched(dst, bytes);
}
#undef TWIDTAB
#undef TWIDOUT

uint64_t texture_revision(pvr_ptr_t ptr) {
  auto it = block_at(ptr);
  return it == g_blocks.end() ? 1u : it->second.revision;
}

namespace {
uint64_t palette_hash(uint32_t first, uint32_t count) {
  uint64_t h = 1469598103934665603ull ^ (pc_pvr_regs[PVR_PALETTE_CFG / 4] & 3u);
  for (uint32_t i = first; i < first + count; i++) {
    h = (h ^ pc_pvr_regs[(PVR_PALETTE_TABLE_BASE / 4) + i]) * 1099511628211ull;
  }
  return h | 1u;
}
}  // namespace

uint64_t palette_revision() { return palette_hash(0u, 1024u); }

uint64_t palette_revision(uint32_t texture_format) {
  const uint32_t pixel_format = (texture_format >> 27u) & 7u;
  if (pixel_format == PVR_PIXEL_MODE_PAL_4BPP) return palette_hash(((texture_format >> 21u) & 0x3fu) * 16u, 16u);
  if (pixel_format == PVR_PIXEL_MODE_PAL_8BPP) return palette_hash(((texture_format >> 25u) & 0x3u) * 256u, 256u);
  return 0u;
}

std::array<uint32_t, 1024> palette_rgba() {
  std::array<uint32_t, 1024> result{};
  const uint32_t format = pc_pvr_regs[PVR_PALETTE_CFG / 4] & 3u;
  for (size_t i = 0u; i < result.size(); i++) {
    const uint32_t value = pc_pvr_regs[(PVR_PALETTE_TABLE_BASE / 4) + i];
    switch (format) {
      case PVR_PAL_ARGB1555:
        result[i] = decode_16bit(static_cast<uint16_t>(value), PVR_PIXEL_MODE_ARGB1555);
        break;
      case PVR_PAL_RGB565:
        result[i] = decode_16bit(static_cast<uint16_t>(value), PVR_PIXEL_MODE_RGB565);
        break;
      case PVR_PAL_ARGB4444:
        result[i] = decode_16bit(static_cast<uint16_t>(value), PVR_PIXEL_MODE_ARGB4444);
        break;
      default:
        result[i] = rgba(static_cast<uint8_t>(value >> 16u), static_cast<uint8_t>(value >> 8u),
                         static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 24u));
        break;
    }
  }
  return result;
}

float shadow_scale() {
  const uint32_t reg = pc_pvr_regs[PVR_CHEAP_SHADOW / 4];
  return (reg & 0x100u) != 0u ? static_cast<float>(reg & 0xffu) / 256.0f : 1.0f;
}

bool decode_texture(const QueuedPrimitive &primitive, DecodedTexture &decoded) {
  if (!primitive.textured || primitive.texture == nullptr || primitive.texture_width == 0u ||
      primitive.texture_height == 0u) {
    return false;
  }
  const uint8_t *tex = static_cast<const uint8_t *>(primitive.texture);
  const uint8_t *base = vram();
  if (tex < base || tex >= base + kVramSize) return false;
  const size_t available = static_cast<size_t>(base + kVramSize - tex);
  const uint32_t format = primitive.texture_format;
  const uint32_t pixel_format = (format >> 27u) & 7u;
  decoded = {};
  decoded.indexed = pixel_format == PVR_PIXEL_MODE_PAL_4BPP || pixel_format == PVR_PIXEL_MODE_PAL_8BPP;
  decoded.bump = pixel_format == PVR_PIXEL_MODE_BUMP;
  decoded.palette_base = pixel_format == PVR_PIXEL_MODE_PAL_4BPP ? ((format >> 21u) & 0x3fu) * 16u
                                                                  : ((format >> 25u) & 0x03u) * 256u;
  uint32_t width = primitive.texture_width;
  const uint32_t height = primitive.texture_height;
  const bool linear = !decoded.indexed && (format & PVR_TXRFMT_VQ_ENABLE) == 0u &&
                      (format & PVR_TXRFMT_NONTWIDDLED) != 0u;
  if (linear && (format & PVR_TXRFMT_X32_STRIDE) != 0u) {
    const uint32_t stride = (pc_pvr_regs[PVR_TEXTURE_MODULO / 4] & 31u) * 32u;
    if (stride) width = stride;
  }
  const bool mipmapped = (format & 0x80000000u) != 0u && !linear;
  const uint32_t top = log2_size(std::max(width, height));
  const uint32_t mip_count = mipmapped ? top + 1u : 1u;
  for (uint32_t mip = 0u; mip < mip_count; mip++) {
    const uint32_t w = std::max(1u, width >> mip);
    const uint32_t h = std::max(1u, height >> mip);
    std::vector<uint8_t> source;
    if (!source_level(tex, available, format, w, h, mipmapped ? log2_size(std::max(w, h)) : 0u, source)) {
      return false;
    }
    DecodedMip level;
    if (!decode_level(source, format, w, h, level)) return false;
    decoded.mips.push_back(std::move(level));
  }
  return !decoded.mips.empty();
}

}  // namespace enj_host_pvr
