/* The attract's 2D layer: include/render/ui/attract_2d_render.h. */
#include "render/ui/attract_2d_render.h"
#include <enDjinn/enj_enDjinn.h>
#include <stdio.h>
#include <string.h>
#if __has_include("attract_2d_data.h")
#include "attract_2d.h" /* with attract_2d_data.h (NATIVE_ATTRACT_2D_DIR, local.cfg.mk): tools/prepare_attract_2d.py */
#include "nm_mem.h"

/* 53e59a & 1 of the reference board (the factory settings): the demo's Japanese text. */
#define ATTRACT_2D_COUNTRY 1u
/* The screen's tile rows and columns (496 x 384 pixels). */
enum { SCREEN_COLUMNS = 62, SCREEN_ROWS = 48 };

/* Retained: the layer's TA records are built when its visible part changes (the blinking line, a demo box, a
 * ranking line revealed: a few ticks in eight) and the list callback only copies them through the store queues.
 * A run of cells whose atlas slots follow in one atlas row (a string, a block's line) is one rectangle; a
 * rectangle is one PVR sprite (two 32-byte records instead of four vertices). Sprite u/v are the upper halves of
 * the floats: exact for every u (multiples of 1/32) and every v of a whole tile row; a rectangle cut by the
 * hidden lines whose v would not fit is a strip of four vertices after a polygon header. The same edges and
 * texel coordinates as one quad a cell and scanline span: the same picture. */
typedef union { pvr_vertex_t vertex; pvr_poly_hdr_t header; uint32_t word[8]; float f[8]; } a2d_record_t;
_Static_assert(sizeof(a2d_record_t) == 32, "one store-queue line per TA record");
/* Sprites (two records each) and strips (four): beyond them a frame is drawn cell by cell as before. The
 * attract's largest layer (the ranking) is 424 rectangles; a cut row needs at most a few strips. */
enum { MAX_SPRITES = 512, MAX_STRIPS = SCREEN_COLUMNS, CELLS = SCREEN_ROWS * SCREEN_COLUMNS };
enum { MAX_RECORDS = 1 + 2 * MAX_SPRITES + 1 + 4 * MAX_STRIPS };

static enj_texture_info_t atlas;
alignas(32) static pvr_poly_hdr_t header;
alignas(32) static pvr_sprite_hdr_t sprite_header;
alignas(32) static a2d_record_t records[MAX_RECORDS];
static unsigned record_count, overflow, stale;
static unsigned loaded, shown;
static unsigned builds, frames, record_peak;
static uint16_t built_words[CELLS], built_first, built_end; /* the visible part as the records show it */
static int16_t built_slot[CELLS];
static nm_a2d_t layer;

static void stats_print(void) {
    if (!frames) return;
    printf("Native attract 2D: frames=%u builds=%u records peak=%u%s; retained sprites, direct SQ\n", frames, builds,
           record_peak, overflow ? " (over capacity: cell by cell)" : "");
    frames = builds = record_peak = 0;
}

void nm_attract_2d_render_unload(void) {
    stats_print();
    shown = 0;
    if (!loaded) return;
    if (pvr_wait_ready() || pvr_wait_render_done()) { enj_state_flag_shutdown(NULL); return; }
    enj_texture_unload(&atlas);
    loaded = 0;
}

int nm_attract_2d_render_load(const char *root) {
    if (!root) return 0;
    if (loaded) return 1;
    char path[512];
    if (snprintf(path, sizeof(path), "%sattract-2d/attract-2d.dt", root) >= (int)sizeof(path)) return 0;
    FILE *probe = fopen(path, "rb");
    if (!probe) { printf("Native attract 2D: no %s: not drawn\n", path); return 1; }
    fclose(probe);
    const unsigned before = pvr_mem_available();
    if (!enj_texture_load_file(path, &atlas)) { printf("FAIL attract 2D: %s\n", path); return 0; }
    loaded = 1;
    if (atlas.width != NM_A2D_ATLAS_WIDTH || atlas.height != NM_A2D_ATLAS_HEIGHT ||
        atlas.pvrformat != PVR_TXRFMT_ARGB1555) {
        printf("FAIL attract 2D: atlas %ux%u format %08lx\n", atlas.width, atlas.height, (unsigned long)atlas.pvrformat);
        nm_attract_2d_render_unload();
        return 0;
    }
    pvr_poly_cxt_t context;
    pvr_poly_cxt_txr(&context, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED, atlas.width, atlas.height,
                     atlas.ptr, PVR_FILTER_NONE);
    context.gen.culling = PVR_CULLING_NONE;
    context.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    context.depth.write = PVR_DEPTHWRITE_ENABLE; /* a scene polygon queued after it fails its depth test */
    pvr_poly_compile(&header, &context);
    /* The sprites': base colour white, no offset colour (words 4..7), as the vertices of the quads. */
    pvr_sprite_cxt_t sprite;
    pvr_sprite_cxt_txr(&sprite, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED, atlas.width, atlas.height,
                       atlas.ptr, PVR_FILTER_NONE);
    sprite.gen.culling = PVR_CULLING_NONE;
    sprite.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    sprite.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_sprite_compile(&sprite_header, &sprite);
    uint32_t *word = (uint32_t *)&sprite_header;
    word[4] = 0xffffffffu; word[5] = word[6] = word[7] = 0;
    printf("Native attract 2D: %u tiles resident, VRAM %u bytes (%u free); largest colour error %u\n",
           (unsigned)NM_A2D_TILES, before - pvr_mem_available(), pvr_mem_available(), (unsigned)NM_A2D_COLOUR_ERROR);
    return 1;
}

void nm_attract_2d_phase2(unsigned course, const nm_race_record_t *records_of_course, unsigned view) {
    stats_print();
    nm_a2d_phase2(&layer, course, records_of_course, ATTRACT_2D_COUNTRY, view);
    shown = 1; stale = 1;
}
void nm_attract_2d_cinematic(unsigned phase, uint32_t counter, int last) {
    if (shown) nm_a2d_cinematic(&layer, phase, counter, last);
}
void nm_attract_2d_demo(unsigned phase, uint32_t counter) {
    if (shown) nm_a2d_demo(&layer, phase, counter);
}
void nm_attract_2d_off(void) { shown = 0; }

static inline float screen_x(float x) { return x * (640.0f / 496.0f) * ENJ_XSCALE; }
static inline float screen_y(float y) { return y * (480.0f / 384.0f); }

/* The tile row's visible scanlines: [top, top + 8) less [first, end); returns their count (0..2). */
static inline unsigned row_spans(unsigned top, unsigned first, unsigned end, unsigned spans[2][2]) {
    unsigned count = 0;
    if (end <= top || first >= top + 8) { spans[0][0] = 0; spans[0][1] = 8; return 1; }
    if (first > top) { spans[count][0] = 0; spans[count][1] = first - top; ++count; }
    if (end < top + 8) { spans[count][0] = end - top; spans[count][1] = 8; ++count; }
    return count;
}

static inline void vertex(float x, float y, float u, float v, int last) {
    pvr_vertex_t *out = pvr_dr_target();
    out->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    out->x = screen_x(x); out->y = screen_y(y); out->z = 1000.0f;
    out->u = u; out->v = v; out->argb = 0xffffffff; out->oargb = 0;
    pvr_dr_commit(out);
}

/* Over capacity: cell by cell, every frame (as before the records were retained). */
static void render_cells(void) {
    pvr_poly_hdr_t *h = pvr_dr_target(); *h = header; pvr_dr_commit(h);
    const float iu = 1.0f / NM_A2D_ATLAS_WIDTH, iv = 1.0f / NM_A2D_ATLAS_HEIGHT;
    for (unsigned row = 0; row < SCREEN_ROWS; ++row) {
        const unsigned top = 8 * row;
        unsigned spans[2][2];
        const unsigned count = row_spans(top, layer.hidden_first, layer.hidden_end, spans);
        if (!count) continue;
        for (unsigned column = 0; column < SCREEN_COLUMNS; ++column) {
            const int slot = nm_a2d_slot(layer.tiles[row * NM_A2D_COLUMNS + column]);
            if (slot < 0) continue;
            const float u = (float)(8 * (slot % 32)), v = (float)(8 * (slot / 32)), x = (float)(8 * column);
            for (unsigned k = 0; k < count; ++k) {
                const float y0 = (float)spans[k][0], y1 = (float)spans[k][1];
                vertex(x, (float)top + y0, u * iu, (v + y0) * iv, 0);
                vertex(x, (float)top + y1, u * iu, (v + y1) * iv, 0);
                vertex(x + 8.0f, (float)top + y0, (u + 8.0f) * iu, (v + y0) * iv, 0);
                vertex(x + 8.0f, (float)top + y1, (u + 8.0f) * iu, (v + y1) * iv, 1);
            }
        }
    }
}

static inline int uv16_exact(float value) {
    union { float f; uint32_t u; } bits = { value };
    return !(bits.u & 0xffffu);
}

/* The records of the layer's visible part: sprites, then the strips of cut rectangles that a sprite cannot hold. */
static void records_build(void) {
    const float iu = 1.0f / NM_A2D_ATLAS_WIDTH, iv = 1.0f / NM_A2D_ATLAS_HEIGHT;
    static a2d_record_t strips[4 * MAX_STRIPS];
    unsigned sprites = 0, strip_count = 0;
    a2d_record_t *r = records;
    r->header = *(const pvr_poly_hdr_t *)&sprite_header; ++r;
    overflow = 0;
    for (unsigned row = 0; row < SCREEN_ROWS && !overflow; ++row) {
        const unsigned top = 8 * row;
        unsigned spans[2][2];
        const unsigned count = row_spans(top, built_first, built_end, spans);
        if (!count) continue;
        const int16_t *slots = built_slot + row * SCREEN_COLUMNS;
        for (unsigned column = 0; column < SCREEN_COLUMNS && !overflow;) {
            const int slot = slots[column];
            if (slot < 0) { ++column; continue; }
            unsigned length = 1;
            while (column + length < SCREEN_COLUMNS && slots[column + length] == slot + (int)length &&
                   (slot + length) % 32u) ++length;
            const float u0 = (float)(8 * (slot % 32)) * iu, u1 = (float)(8 * (slot % 32 + length)) * iu;
            const float v = (float)(8 * (slot / 32));
            const float x0 = screen_x((float)(8 * column)), x1 = screen_x((float)(8 * (column + length)));
            for (unsigned k = 0; k < count; ++k) {
                const float v0 = (v + (float)spans[k][0]) * iv, v1 = (v + (float)spans[k][1]) * iv;
                const float y0 = screen_y((float)(top + spans[k][0])), y1 = screen_y((float)(top + spans[k][1]));
                if (uv16_exact(v0) && uv16_exact(v1)) {
                    if (sprites == MAX_SPRITES) { overflow = 1; break; }
                    ++sprites;
                    /* A left bottom, B left top, C right top, D right bottom. */
                    r[0].word[0] = PVR_CMD_VERTEX_EOL;
                    r[0].f[1] = x0; r[0].f[2] = y1; r[0].f[3] = 1000.0f;
                    r[0].f[4] = x0; r[0].f[5] = y0; r[0].f[6] = 1000.0f;
                    r[0].f[7] = x1; r[1].f[0] = y0; r[1].f[1] = 1000.0f;
                    r[1].f[2] = x1; r[1].f[3] = y1; r[1].word[4] = 0;
                    r[1].word[5] = PVR_PACK_16BIT_UV(u0, v1);
                    r[1].word[6] = PVR_PACK_16BIT_UV(u0, v0);
                    r[1].word[7] = PVR_PACK_16BIT_UV(u1, v0);
                    r += 2;
                } else {
                    if (strip_count == MAX_STRIPS) { overflow = 1; break; }
                    pvr_vertex_t *q = &strips[4 * strip_count++].vertex;
                    const float corner[4][4] = {{x0, y0, u0, v0}, {x0, y1, u0, v1}, {x1, y0, u1, v0}, {x1, y1, u1, v1}};
                    for (unsigned c = 0; c < 4; ++c, ++q) {
                        q->flags = c == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                        q->x = corner[c][0]; q->y = corner[c][1]; q->z = 1000.0f;
                        q->u = corner[c][2]; q->v = corner[c][3]; q->argb = 0xffffffff; q->oargb = 0;
                    }
                }
            }
            column += length;
        }
    }
    if (strip_count && !overflow) {
        r->header = header; ++r;
        nm_memcpy(r, strips, 4 * strip_count * sizeof(*r));
        r += 4 * strip_count;
    }
    record_count = (unsigned)(r - records);
    if (record_count > record_peak) record_peak = record_count;
    ++builds;
}

/* The visible part against what the records show: a cell's slot looked up only where its word changed. */
static void records_update(void) {
    int changed = stale || built_first != layer.hidden_first || built_end != layer.hidden_end;
    for (unsigned row = 0; row < SCREEN_ROWS; ++row) {
        const uint16_t *word = layer.tiles + row * NM_A2D_COLUMNS;
        uint16_t *built = built_words + row * SCREEN_COLUMNS;
        int16_t *slot = built_slot + row * SCREEN_COLUMNS;
        for (unsigned column = 0; column < SCREEN_COLUMNS; ++column)
            if (stale || built[column] != word[column]) {
                built[column] = word[column];
                slot[column] = (int16_t)nm_a2d_slot(word[column]);
                changed = 1;
            }
    }
    stale = 0;
    built_first = layer.hidden_first; built_end = layer.hidden_end;
    if (changed) records_build();
}

static void render_layer(void *unused) {
    (void)unused;
    if (overflow) { render_cells(); return; }
    const a2d_record_t *r = records;
    for (unsigned n = record_count; n; --n, ++r) {
        a2d_record_t *out = pvr_dr_target(); *out = *r; pvr_dr_commit(out);
    }
}

int nm_attract_2d_render_enqueue(void) {
    if (loaded && shown) {
        records_update();
        ++frames;
        enj_render_list_add(PVR_LIST_PT_POLY, render_layer, NULL);
    }
    return 1;
}
#else
int nm_attract_2d_render_load(const char *root) {
    (void)root;
    puts("Native attract 2D: no attract_2d_data.h (NATIVE_ATTRACT_2D_DIR): not drawn");
    return 1;
}
void nm_attract_2d_render_unload(void) {}
void nm_attract_2d_phase2(unsigned course, const nm_race_record_t *records, unsigned view) {
    (void)course; (void)records; (void)view;
}
void nm_attract_2d_cinematic(unsigned phase, uint32_t counter, int last) { (void)phase; (void)counter; (void)last; }
void nm_attract_2d_demo(unsigned phase, uint32_t counter) { (void)phase; (void)counter; }
void nm_attract_2d_off(void) {}
int nm_attract_2d_render_enqueue(void) { return 1; }
#endif
