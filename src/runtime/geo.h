// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer and the rasterizer's front end (culling, clipping,
// z-sort keys): the display list, as native C++. The original Model 2's
// geometrizer runs code from ROM inside its DSP that nobody has dumped, so
// this is MAME's high-level implementation (src/mame/sega/model2_v.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81, BSD-3-Clause; notice above kept
// as the licence requires), transplanted: geo_parse and its commands,
// model2_3d_push, model2_3d_process_polygon. Memory is read through
// bounds-checked cursors, so a display list that runs off its buffer is a
// hard error instead of MAME's undefined behaviour. See THIRD_PARTY.md.
#pragma once

#include "runtime/fast_paths.h"
#include "runtime/rom_source.h"

#include <cstdint>
#include <stdexcept>
#ifdef M2_DC_NATIVE_HOST
#include <span>
#endif
#include <vector>

namespace rt {

struct GeoFatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GeoVertex {
    float x = 0, y = 0;
    float p[3] = {0, 0, 0}; // pz, pu, pv
};

// One polygon kept for rendering (after culling and clipping), as MAME's
// model2_3d_process_polygon adds it to its list.
struct GeoPoly {
    uint16_t z = 0;
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
    int16_t viewport[4] = {0, 0, 0, 0};
    int16_t center[2] = {0, 0};
    uint8_t window = 0;
    uint32_t reverse = 0;
    uint8_t num_vertices = 0;
#ifdef M2_FAST_GEO
    // Not zero-filled for every polygon kept (160 bytes): only
    // v[0..num_vertices) is ever read, and model2_3d_process_polygon writes
    // those.
    GeoPoly() {}
    union { GeoVertex v[8]; };
#else
    GeoVertex v[8];
#endif
};

// Cursors standing in for MAME's raw u32/u16 pointers. Reads run on from a
// masked start, so a start near the end runs off it (time attack does, in
// polygon RAM and in texture memory). MAME reads past its array there; we
// wrap, as the hardware's address counter would. Sizes are powers of 2.
struct GeoPtr {
    uint32_t *base = nullptr;
    uint32_t size = 0, i = 0;
#ifdef M2_DC_MEMORY
    RomSource *rom = nullptr; // the polygon ROM, read through pages (base is null)
    mutable uint32_t rom_word = 0;
#endif
    bool null() const { return base == nullptr; }
    uint32_t &at() const {
#ifdef M2_DC_MEMORY
        if (rom) return rom_word = rom->dword(RomRegion::Polygons, (i & (size - 1)) * 4);
#endif
        if (!base) throw GeoFatal("geometrizer read from missing memory");
        return base[i & (size - 1)];
    }
    uint32_t &operator*() const { return at(); }
    GeoPtr operator++(int) { GeoPtr t = *this; ++i; return t; }
    GeoPtr &operator+=(uint32_t n) { i += n; return *this; }
#ifdef M2_FAST_GEO
    // n words from here into out, moving past them: the values n times
    // *(*this)++ gives, a page (or up to the wrap) at a time instead of a
    // page lookup each. (The ROM's pages are little-endian words, as the
    // Dreamcast's M2_DC_MEMORY supplies.)
    void read(uint32_t *out, uint32_t n) {
        while (n) {
            const uint32_t w = i & (size - 1);
            uint32_t run = n < size - w ? n : size - w;
            const uint32_t *src;
#ifdef M2_DC_MEMORY
            if (rom) {
                constexpr uint32_t kWords = RomSource::kPageSize / 4;
                if (run > kWords - (w & (kWords - 1))) run = kWords - (w & (kWords - 1));
                src = reinterpret_cast<const uint32_t *>(rom->page_fast(RomRegion::Polygons, w / kWords)) +
                      (w & (kWords - 1));
            } else
#endif
            {
                if (!base) throw GeoFatal("geometrizer read from missing memory");
                src = base + w;
            }
            for (uint32_t k = 0; k < run; ++k) out[k] = src[k];
            out += run;
            n -= run;
            i += run;
        }
    }
#endif
};
struct GeoPtr16 {
    const uint16_t *base = nullptr;
    uint32_t size = 0, i = 0;
#ifdef M2_DC_MEMORY
    RomSource *rom = nullptr; // the texture ROM, read through pages (base is null)
#endif
    uint16_t operator*() const {
#ifdef M2_DC_MEMORY
        if (rom) return rom->word(RomRegion::Textures, (i & (size - 1)) * 2);
#endif
        if (!base) throw GeoFatal("rasterizer read from missing texture memory");
        return base[i & (size - 1)];
    }
    GeoPtr16 operator++(int) { GeoPtr16 t = *this; ++i; return t; }
#ifdef M2_FAST_GEO
    // As GeoPtr::read, 16-bit words.
    void read(uint16_t *out, uint32_t n) {
        while (n) {
            const uint32_t w = i & (size - 1);
            uint32_t run = n < size - w ? n : size - w;
            const uint16_t *src;
#ifdef M2_DC_MEMORY
            if (rom) {
                constexpr uint32_t kWords = RomSource::kPageSize / 2;
                if (run > kWords - (w & (kWords - 1))) run = kWords - (w & (kWords - 1));
                src = reinterpret_cast<const uint16_t *>(rom->page_fast(RomRegion::Textures, w / kWords)) +
                      (w & (kWords - 1));
            } else
#endif
            {
                if (!base) throw GeoFatal("rasterizer read from missing texture memory");
                src = base + w;
            }
            for (uint32_t k = 0; k < run; ++k) out[k] = src[k];
            out += run;
            n -= run;
            i += run;
        }
    }
#endif
};

class Geo {
public:
    // polygons: the model ROM (0x1000000 bytes); textures: the texture ROM
    // (0x1000000 bytes); buffer: buffer RAM (0x8000 dwords), which the
    // display list is read from.
    Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer);
#ifdef M2_DC_MEMORY
    // Both ROMs read through pages, not copied.
    Geo(RomSource &rom, uint32_t *buffer);
#endif

    // MAME screen_vblank -> geo_parse: walk this frame's display list from
    // read_start (the geometrizer's read-start register).
    void parse(uint32_t read_start);

    // This frame's output.
    std::vector<GeoPoly> polys;       // kept polygons, in the order added
    std::vector<uint32_t> pushed;     // every word handed to the rasterizer
    bool record_pushes = false;
#ifdef M2_DC_SPEED
    // Draw mode, a frame whose polygons will not be shown: object data is
    // not parsed (the rasterizer gets its opening and closing words only).
    // Every other command still runs, so the geometrizer's state is the same.
    bool skip_objects = false;
#endif

    struct plane {
        GeoVertex normal;
        float distance = 0;
    };
    struct texture_parameter {
        float diffuse = 0, ambient = 0;
        uint32_t specular_control = 0;
        float specular_scale = 0;
    };
    struct raster_state {
        const uint16_t *texture_rom = nullptr;
        uint32_t texture_rom_mask = 0;
#ifdef M2_DC_MEMORY
        RomSource *rom = nullptr;
#endif
        int16_t viewport[4] = {0, 0, 0, 0};
        int16_t center[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        uint16_t center_sel = 0;
        uint32_t reverse = 0;
        int32_t z_adjust = 0;
        float polygon_z = 0;
        uint8_t master_z_clip = 0;
        uint32_t cur_command = 0;
        uint32_t command_buffer[32] = {};
        uint32_t command_index = 0;
        uint32_t poly_list_index = 0;
        uint16_t min_z = 0, max_z = 0;
        uint16_t texture_ram[0x10000] = {};
        uint8_t log_ram[0x8000] = {};
        uint8_t cur_window = 0;
        plane clip_plane[4][4];
    };
    struct geo_state {
        raster_state *raster = nullptr;
        uint32_t mode = 0;
        uint32_t *polygon_rom = nullptr;
        uint32_t polygon_rom_mask = 0;
#ifdef M2_DC_MEMORY
        RomSource *rom = nullptr;
#endif
        float matrix[12] = {};
        GeoVertex focus, light;
        float lod = 0;
        float coef_table[32] = {};
        texture_parameter texture_parameters[32];
        uint32_t polygon_ram0[0x8000] = {};
        uint32_t polygon_ram1[0x8000] = {};
    };

#ifdef M2_DC_NATIVE_HOST
    // Host asset inspection for platform/dreamcast-native. Source coordinates
    // are still local; raster_words independently expose the transformed
    // reference output. The observer borrows everything for this call and
    // requires record_pushes.
    struct ObjectSource {
        GeoPtr vertices;
        uint32_t opcode, texture_points, texture_headers, address, count;
    };
    // Own mutable geometry/raster RAM, borrow only ROM pointers from a live
    // observer snapshot, whose owner must outlive this instance. No
    // display-list buffer: use inspect_object, not parse.
    explicit Geo(const geo_state &snapshot);
    void inspect_object(const ObjectSource &source);
    void *object_observer_data = nullptr;
    void (*object_observer)(void *, const geo_state &, const ObjectSource &,
                            std::span<const uint32_t> raster_words) = nullptr;
#endif

    int windows() const { return raster_.cur_window; } // after parse: the last window used
    void zclip_w(uint32_t data) { raster_.master_z_clip = uint8_t(data); } // model2_3d_zclip_w
    // Widescreen (enhancement, 0 = off): a full-width viewport's left and right
    // clip planes move out by this many pixels, so polygons beyond the 4:3
    // edges survive for the wider screen. Applies from the next window command.
    void set_wide_margin(int pixels) { wide_margin_ = pixels; }

private:
    int wide_margin_ = 0;
    std::vector<uint32_t> polygon_rom_;
    std::vector<uint16_t> texture_rom_;
    uint32_t *buffer_;
    raster_state raster_;
    geo_state geo_;

    GeoPtr buf(uint32_t i) { return GeoPtr{buffer_, 0x8000, i}; }
    uint16_t float_to_zval(float floatval, int32_t z_adjust);
    bool check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z);
    template <unsigned NumVerts> void model2_3d_process_polygon(raster_state *raster, uint32_t attr);
    void model2_3d_push(raster_state *raster, uint32_t input);
    void model2_3d_push_point(raster_state *raster, const GeoVertex &point);
    void render_frame_start();
    void geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count);
    GeoPtr geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_test(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_end(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code);
};

} // namespace rt
