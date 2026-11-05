/* Palette shading probe: a static comparison sheet for a person and a capture
 * card. Per texture (rows) and polygon luma (column groups), three 64x64
 * quads over the same 32x32 texels at 2x:
 *   a  PAL4 index texture with that polygon's 16 exact colours as palette,
 *   b  the same index texture with a colour-independent gain-bucket palette
 *      and a per-polygon vertex colour (PVR modulate),
 *   c  the current pack's anchor texture (VQ16) with its modulation colour.
 * Each texture has a point-sampled row and a bilinear row. The bottom row
 * shows, at 1x for texture 0 and the middle luma: a, b, and two VQ+PAL4
 * encodings with 4x4-texel codebook entries (16 nibbles in 8 bytes; low
 * nibble first, then high nibble first) and an 8x8 index map.
 * Unattended: the six states (2 pages x 3 palette formats) are held for
 * PROBE_HOLD_FRAMES each, each change is logged, then the program exits.
 * With a pad: A next page, B next palette format (stops the automatic run;
 * START then exits).
 * Magenta 8x8 squares mark the corners for tools/check_palette_probe.py.
 * Data: tools/prepare_palette_probe.py (ROM-derived, generated locally). */
#include <enDjinn/enj_enDjinn.h>
#include <kos.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "palette_probe_data.h"

static pvr_ptr_t index_texture[2][3];
static pvr_ptr_t vq_texture[2][2];
static enj_texture_info_t anchor[2][9];
static int anchor_loaded[2][9];
enum { PROBE_HOLD_FRAMES = 240 }; /* 4 s at 60 Hz. */
static unsigned page, mode, applied_mode = 99, frame, manual, announced = 99;
static const int palette_formats[3] = {PVR_PAL_ARGB8888, PVR_PAL_ARGB1555, PVR_PAL_RGB565};
static const char *const palette_names[3] = {"ARGB8888", "ARGB1555", "RGB565"};

static void upload_palettes(void) {
  const int format = palette_formats[mode];
  pvr_set_pal_format(format);
  for (unsigned p = 0; p < sizeof(probe_palettes) / sizeof(probe_palettes[0]); ++p)
    for (unsigned i = 0; i < 16; ++i) {
      const unsigned r = probe_palettes[p][3 * i], g = probe_palettes[p][3 * i + 1], b = probe_palettes[p][3 * i + 2];
      uint32_t color = 0xff000000u | r << 16 | g << 8 | b;
      if (format == PVR_PAL_ARGB1555) color = 0x8000u | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
      else if (format == PVR_PAL_RGB565) color = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
      pvr_set_pal_entry(p * 16 + i, color);
    }
  applied_mode = mode;
  printf("Palette probe: palette format %s, %u banks of 16 uploaded\n", palette_names[mode],
         (unsigned)(sizeof(probe_palettes) / sizeof(probe_palettes[0])));
}

static void quad(pvr_poly_cxt_t *context, float x, float y, float size, float u, float v, uint32_t argb) {
  context->gen.culling = PVR_CULLING_NONE;
  pvr_poly_hdr_t *header = (pvr_poly_hdr_t *)pvr_dr_target();
  pvr_poly_compile(header, context);
  pvr_dr_commit(header);
  for (unsigned corner = 0; corner < 4; ++corner) {
    const float dx = corner >= 2 ? 1.0f : 0.0f, dy = corner & 1 ? 0.0f : 1.0f;
    pvr_vertex_t *vertex = (pvr_vertex_t *)pvr_dr_target();
    vertex->flags = corner == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vertex->x = (x + dx * size) * ENJ_XSCALE;
    vertex->y = y + dy * size;
    vertex->z = 1.0f;
    vertex->u = dx * u;
    vertex->v = dy * v;
    vertex->argb = argb;
    vertex->oargb = 0;
    pvr_dr_commit(vertex);
  }
}

static void textured(float x, float y, float size, int format, unsigned width, unsigned height, pvr_ptr_t texture,
                     int filter, float u, float v, uint32_t argb) {
  pvr_poly_cxt_t context;
  pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY, format, width, height, texture, filter);
  context.txr.uv_clamp = PVR_UVCLAMP_UV;
  quad(&context, x, y, size, u, v, argb);
}

static void render(void *unused) {
  (void)unused;
  static const float markers[4][2] = {{8, 8}, {624, 8}, {8, 464}, {624, 464}};
  for (unsigned m = 0; m < 4; ++m) {
    pvr_poly_cxt_t context;
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    quad(&context, markers[m][0], markers[m][1], 8, 0, 0, 0xffff00ffu);
  }
  for (unsigned t = 0; t < 3; ++t)
    for (unsigned filtered = 0; filtered < 2; ++filtered)
      for (unsigned l = 0; l < 3; ++l) {
        const unsigned cell = t * 3 + l, bank = (page * 9 + cell) * 2;
        const probe_cell_t *c = &probe_cells[page][cell];
        const float x = PROBE_ORIGIN_X + (float)(l * 3) * PROBE_CELL;
        const float y = PROBE_ORIGIN_Y + (float)(t * 2 + filtered) * PROBE_CELL;
        const int filter = filtered ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE;
        const int pal4 = PVR_TXRFMT_PAL4BPP;
        textured(x, y, PROBE_CELL, pal4 | PVR_TXRFMT_4BPP_PAL(bank), 32, 32, index_texture[page][t], filter, 1, 1,
                 0xffffffffu);
        textured(x + PROBE_CELL, y, PROBE_CELL, pal4 | PVR_TXRFMT_4BPP_PAL(bank + 1), 32, 32, index_texture[page][t],
                 filter, 1, 1, 0xff000000u | (uint32_t)c->vertex[0] << 16 | (uint32_t)c->vertex[1] << 8 | c->vertex[2]);
        if (anchor_loaded[page][cell])
          /* As the game does: pack textures carry a short VQ codebook, and
           * the address handed to the PVR is adjusted for the missing head. */
          textured(x + 2 * PROBE_CELL, y, PROBE_CELL, anchor[page][cell].pvrformat, c->width, c->height,
                   fDtAdjustPVRPointer(&anchor[page][cell].hdr, anchor[page][cell].ptr), filter, 32.0f / c->width,
                   32.0f / c->height, c->modulation);
      }
  {
    const unsigned bank = (page * 9 + 1) * 2; /* Texture 0, middle luma. */
    const probe_cell_t *c = &probe_cells[page][1];
    const float y = PROBE_ORIGIN_Y + 6.0f * PROBE_CELL + 8;
    const int pal4 = PVR_TXRFMT_PAL4BPP;
    textured(PROBE_ORIGIN_X, y, 32, pal4 | PVR_TXRFMT_4BPP_PAL(bank), 32, 32, index_texture[page][0], PVR_FILTER_NONE, 1, 1,
             0xffffffffu);
    textured(PROBE_ORIGIN_X + 64, y, 32, pal4 | PVR_TXRFMT_4BPP_PAL(bank + 1), 32, 32, index_texture[page][0],
             PVR_FILTER_NONE, 1, 1,
             0xff000000u | (uint32_t)c->vertex[0] << 16 | (uint32_t)c->vertex[1] << 8 | c->vertex[2]);
    for (unsigned variant = 0; variant < 2; ++variant)
      if (vq_texture[page][variant])
        textured(PROBE_ORIGIN_X + 128 + 64.0f * variant, y, 32,
                 pal4 | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_4BPP_PAL(bank), 32, 32, vq_texture[page][variant],
                 PVR_FILTER_NONE, 1, 1, 0xffffffffu);
  }
}

static void labels(void *unused) {
  (void)unused;
  char line[96];
  enj_font_scale_set(1);
  snprintf(line, sizeof(line), "palette probe: state %u/6  page %u (%s)  palette %s", page * 3 + mode + 1, page,
           page ? "road" : "car", palette_names[mode]);
  enj_qfont_write(line, 32, 14, PVR_LIST_PT_POLY);
}

static void update(void *unused) {
  (void)unused;
  enj_ctrlr_state_t **controllers = enj_ctrl_get_states();
  for (int i = 0; i < 4; ++i) {
    if (controllers[i] == NULL) continue;
    if (controllers[i]->button.A == ENJ_BUTTON_DOWN_THIS_FRAME) { page ^= 1; manual = 1; }
    if (controllers[i]->button.B == ENJ_BUTTON_DOWN_THIS_FRAME) { mode = (mode + 1) % 3; manual = 1; }
    if (manual && controllers[i]->button.START == ENJ_BUTTON_DOWN_THIS_FRAME) { enj_state_flag_shutdown(NULL); return; }
    break;
  }
  if (!manual) {
    const unsigned state = frame / PROBE_HOLD_FRAMES;
    if (state >= 6) {
      puts("Palette probe: all six states shown; exiting");
      enj_state_flag_shutdown(NULL);
      return;
    }
    page = state / 3; mode = state % 3;
    ++frame;
  }
  if (mode != applied_mode) upload_palettes();
  if (page * 3 + mode != announced) {
    announced = page * 3 + mode;
    printf("Palette probe: state %u of 6: page %u (%s), palette %s, held %u frames\n", announced + 1, page,
           page ? "road" : "car", palette_names[mode], (unsigned)PROBE_HOLD_FRAMES);
  }
  enj_render_list_add(PVR_LIST_OP_POLY, render, NULL);
  enj_render_list_add(PVR_LIST_PT_POLY, labels, NULL);
}

static pvr_ptr_t upload(const unsigned char *data, size_t bytes, const char *what) {
  pvr_ptr_t texture = pvr_mem_malloc(bytes);
  if (!texture) {
    printf("Palette probe: FAILED to allocate %u bytes for %s\n", (unsigned)bytes, what);
    return NULL;
  }
  pvr_txr_load(data, texture, bytes);
  return texture;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  enj_state_init_defaults();
  if (enj_state_startup() != 0) return 1;
  const size_t before = pvr_mem_available();
  for (unsigned p = 0; p < 2; ++p) {
    for (unsigned t = 0; t < 3; ++t) index_texture[p][t] = upload(probe_index[p][t], 512, "PAL4 index 32x32");
#if PROBE_HAVE_VQ
    vq_texture[p][0] = upload(probe_vq_wide[p], 2048 + 64, "VQ+PAL4 4x4 blocks, low nibble first");
    vq_texture[p][1] = upload(probe_vq_narrow[p], 2048 + 64, "VQ+PAL4 4x4 blocks, high nibble first");
#endif
    for (unsigned c = 0; c < 9; ++c) {
      /* The blob loader copies from an aligned source. */
      void *copy = memalign(32, probe_cells[p][c].anchor_bytes);
      if (copy) {
        memcpy(copy, probe_cells[p][c].anchor, probe_cells[p][c].anchor_bytes);
        anchor_loaded[p][c] = enj_texture_load_blob(copy, &anchor[p][c]);
        free(copy);
      }
      if (!anchor_loaded[p][c]) printf("Palette probe: FAILED to load anchor texture page %u cell %u\n", p, c);
      else if (anchor[p][c].width != probe_cells[p][c].width || anchor[p][c].height != probe_cells[p][c].height)
        printf("Palette probe: anchor size mismatch page %u cell %u\n", p, c);
    }
  }
  printf("Palette probe: textures use %u bytes of VRAM; PAL4 twiddled 32x32 x6, anchors x18, VQ+PAL4 attempts %s\n",
         (unsigned)(before - pvr_mem_available()), PROBE_HAVE_VQ ? "x4 (acceptance is visual only)" : "not built");
  puts("Palette probe: submitted formats PAL4BPP twiddled (point and bilinear), PAL4BPP|VQ, and the pack's VQ16 anchors;"
       " the PVR reports no errors, so judge acceptance from the picture");
  static enj_mode_t probe_mode = {.name = "Palette probe", .mode_updater = update, .data = NULL};
  enj_mode_push(&probe_mode);
  enj_state_run();
  return 0;
}
