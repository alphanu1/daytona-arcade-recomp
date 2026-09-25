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

#include <cstdint>
#include <stdexcept>
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
    GeoVertex v[8];
};

// A bounds-checked cursor standing in for MAME's raw u32 pointers.
struct GeoPtr {
    uint32_t *base = nullptr;
    uint32_t size = 0, i = 0;
    bool null() const { return base == nullptr; }
    uint32_t &at() const {
        if (!base || i >= size) throw GeoFatal("geometrizer read past the end of its memory");
        return base[i];
    }
    uint32_t &operator*() const { return at(); }
    GeoPtr operator++(int) { GeoPtr t = *this; ++i; return t; }
    GeoPtr &operator+=(uint32_t n) { i += n; return *this; }
};
struct GeoPtr16 {
    const uint16_t *base = nullptr;
    uint32_t size = 0, i = 0;
    uint16_t operator*() const {
        if (i >= size) throw GeoFatal("rasterizer read past the end of texture memory");
        return base[i];
    }
    GeoPtr16 operator++(int) { GeoPtr16 t = *this; ++i; return t; }
};

class Geo {
public:
    // polygons: the model ROM (0x1000000 bytes); textures: the texture ROM
    // (0x1000000 bytes); buffer: buffer RAM (0x8000 dwords), which the
    // display list is read from.
    Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer);

    // MAME screen_vblank -> geo_parse: walk this frame's display list from
    // read_start (the geometrizer's read-start register).
    void parse(uint32_t read_start);

    // This frame's output.
    std::vector<GeoPoly> polys;       // kept polygons, in the order added
    std::vector<uint32_t> pushed;     // every word handed to the rasterizer
    bool record_pushes = false;

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
        float matrix[12] = {};
        GeoVertex focus, light;
        float lod = 0;
        float coef_table[32] = {};
        texture_parameter texture_parameters[32];
        uint32_t polygon_ram0[0x8000] = {};
        uint32_t polygon_ram1[0x8000] = {};
    };

    void zclip_w(uint32_t data) { raster_.master_z_clip = uint8_t(data); } // model2_3d_zclip_w

private:
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
