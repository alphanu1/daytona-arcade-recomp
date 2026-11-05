// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
// Host-only source mesh extraction. Point ordering, links and texture addressing
// follow runtime/geo.cpp's MAME reference; see THIRD_PARTY.md. No clipped or
// screen-space vertices are used as source geometry.
#pragma once
#include "runtime/geo.h"
#include "runtime/video.h"
#include "dreamcast_assets.h"
#include "../include/lighting.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>

namespace daytona::native {
struct ModelVertex { std::array<float, 3> position; uint16_t u, v; };
struct ModelFace {
    uint32_t attribute;
    std::array<uint16_t, 4> header;
    std::array<float, 3> normal;
    rt::Geo::texture_parameter lighting;
    uint16_t reference_light; // Luma + rear bit: evidence, never mesh identity.
    std::array<uint16_t, 2> translation_light{}; // Modes 0/1, both facing signs.
    unsigned translation_sides = 0; // Bit 0 front, bit 1 rear within the requested box.
    std::vector<ModelVertex> vertices;
};

/* Static WORLD-space scenery under the normal-race directional light.
 * Supplied normals only. Include both facing branches and adjacent integer
 * levels. For component magnitudes <=4, rigid view rotations and the checked
 * 8*epsilon FTRV envelope, rotation/dot/lighting roundoff is <1 luma. No
 * nearest-light substitution. This does not cover moving models, non-rigid
 * views or other scene lights.
 * Variable specular (mode 1, control and scale non-zero, ambient below 255;
 * Expert's glass and metal): the specular term (2 n.l) n_z - l_z reads the
 * view-space z of the normal, so it follows the camera's turn; the renderer
 * evaluates it every frame (prepare_face_order.py --live-specular). Every
 * luma it can reach is exported: from each branch's specular-free level less
 * one up to that level plus the largest term (n.l, n and l rotated alike:
 * the view z of R = 2 (n.l) n - l is at most |R|, raised to the control's
 * power and times the scale) plus one, saturated at 255. */
inline bool race_scenery_specular(const rt::Geo::texture_parameter &l, unsigned mode) {
    return (mode&1) && l.specular_control && l.specular_scale && l.ambient<255;
}
inline std::set<uint16_t> race_scenery_lumas(const ModelFace &face, unsigned mode) {
    const auto &l=face.lighting;
    if (mode>1) throw std::runtime_error("race scenery requires supplied normals");
    for (float n : face.normal) if (!std::isfinite(n) || std::abs(n)>4)
        throw std::runtime_error("race scenery normal exceeds rigid-view lighting envelope");
    if (l.ambient==255) return {255};
    const bool specular=race_scenery_specular(l,mode);
    if (specular && (l.specular_scale<0 || l.specular_control>255))
        throw std::runtime_error("race scenery specular outside the renderer's bounds");
    nm_material_t material{};
    material.diffuse=l.diffuse; material.ambient=l.ambient;
    material.specular_control=specular ? 0 : l.specular_control; material.specular_scale=l.specular_scale;
    unsigned reach=0;
    if (specular) {
        const float *L=nm_race_world_light, *n=face.normal.data();
        const double dot=double(n[0])*L[0]+double(n[1])*L[1]+double(n[2])*L[2];
        double r=0;
        for (unsigned a=0;a<3;++a) r+=(2*dot*n[a]-L[a])*(2*dot*n[a]-L[a]);
        double term=std::sqrt(r);
        const unsigned c=l.specular_control;
        if (c>>1) term*=term;
        if (c>>2) term*=term;
        if ((c+1)>>3) term*=term;
        term*=l.specular_scale;
        reach=term>=256 ? 256u : unsigned(std::ceil(term));
    }
    unsigned axis=0;
    for (unsigned a=1;a<3;++a) if (std::abs(face.normal[a])>std::abs(face.normal[axis])) axis=a;
    std::set<uint16_t> result;
    for (float sign : {-1.f,1.f}) {
        float points[3][3]{}; points[2][axis]=sign;
        const int light=nm_face_lighting(mode,face.normal.data(),points,nm_race_world_light,&material);
        if (light<0) throw std::runtime_error("invalid race scenery light");
        const int top=std::min(255,(light&255)+1+int(reach));
        for (int v=std::max(0,(light&255)-1);v<=top;++v) result.insert(uint16_t(v));
    }
    return result;
}

inline std::vector<ModelFace> decode_model(const rt::Geo::geo_state &state,
    const rt::Geo::ObjectSource &source, std::span<const uint32_t> words, float translation_limit = 0) {
    auto input = source.vertices;
    const auto &r = *state.raster;
    size_t word = 0, read_count = 0;
    const auto expect = [&](uint32_t value) {
        if (word >= words.size() || words[word++] != value)
            throw std::runtime_error("source model differs from reference rasterizer words");
    };
    const auto read = [&]() {
        // Refuse multiple memory wraps on malformed/unbounded objects.
        if (++read_count > input.size) throw std::runtime_error("source model exceeds its memory region");
        return *input++;
    };
    const auto point = [&]() {
        std::array<float, 3> p;
        for (auto &v : p) {
            v = std::bit_cast<float>(read());
            if (!std::isfinite(v)) throw std::runtime_error("non-finite model coordinate");
        }
        return p;
    };
    const auto check_point = [&](const std::array<float, 3> &p) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto *m = state.matrix;
            float v = p[0] * m[axis] + p[1] * m[3 + axis] + p[2] * m[6 + axis] + m[9 + axis];
            if (axis == 0) v *= state.focus.x;
            if (axis == 1) v *= state.focus.y;
            expect(std::bit_cast<uint32_t>(v) >> 8);
        }
    };
    const auto texture = [&](uint32_t address) {
        return address & 0x800000
            ? rt::GeoPtr16{r.texture_ram, 0x10000, address & 0xffff}
            : rt::GeoPtr16{r.texture_rom, r.texture_rom_mask + 1,
                          address & r.texture_rom_mask};
    };
    expect(source.opcode >> 23); expect(source.texture_points); expect(source.texture_headers);
    auto p0 = point(), p1 = point();
    check_point(p0); check_point(p1);
    uint32_t tpa = source.texture_points, tha = source.texture_headers;
    std::vector<ModelFace> faces;
    for (uint32_t link = 0; link < source.count; ++link) {
        const uint32_t attribute = read();
        expect(attribute & 0x3ffff);
        if (!(attribute & 3)) break;
        ModelFace face{};
        face.attribute = attribute;
        for (auto &n : face.normal) {
            const uint32_t bits = read();
            n = state.mode & 2 ? 0.0f : std::bit_cast<float>(bits);
            if (!std::isfinite(n)) throw std::runtime_error("non-finite model normal");
        }
        // Lighting/distance are instance-dependent evidence, not baked vertices.
        if (word + 2 > words.size()) throw std::runtime_error("truncated reference lighting");
        face.reference_light = uint16_t((words[word] >> 15) & 511);
        word += 2;
        const auto p2 = point(); check_point(p2);
        auto p3 = p2;
        if (attribute & 1) { p3 = point(); check_point(p3); }
        else { read(); read(); read(); } // Unused fourth-point storage.
        const std::array<std::array<float, 3>, 4> positions{p1, p0, p2, p3};
        auto uv = texture(tpa), header = texture(tha);
        for (unsigned i = 0; i < (attribute & 1 ? 4u : 3u); ++i) {
            const uint16_t v = *uv++, u = *uv++;
            face.vertices.push_back({positions[i], u, v});
        }
        for (auto &h : face.header) h = *header++;
        tpa += unsigned(face.vertices.size()) * 2;
        const int offset = int((attribute >> 12) & 31);
        tha += (offset & 16 ? offset - 32 : offset) * 4;
        face.lighting = state.texture_parameters[(attribute >> 18) & 31];
        // Validate the SAME portable native light function against the full
        // reference parser's emitted luma/rear bits, before geometric culling.
        float points[3][3], normal[3];
        const auto *m = state.matrix;
        for (unsigned axis = 0; axis < 3; ++axis) {
            normal[axis] = face.normal[0]*m[axis] + face.normal[1]*m[3+axis] + face.normal[2]*m[6+axis];
            for (unsigned v = 0; v < 3; ++v) {
                const auto &p = face.vertices[v].position;
                points[v][axis] = p[0]*m[axis] + p[1]*m[3+axis] + p[2]*m[6+axis] + m[9+axis];
            }
        }
        nm_material_t material{};
        material.diffuse = face.lighting.diffuse; material.ambient = face.lighting.ambient;
        material.specular_control = face.lighting.specular_control;
        material.specular_scale = face.lighting.specular_scale;
        const float light[3] = {state.light.x, state.light.y, state.light.p[0]};
        if (nm_face_lighting(state.mode & 3, normal, points, light, &material) != face.reference_light)
            throw std::runtime_error("native face lighting differs from reference luma/rear bits");
        if (!(state.mode & 2)) {
            double facing = 0, extent = 0, magnitude = 0, floor_scale = 1;
            for (unsigned a = 0; a < 3; ++a) {
                double normal_magnitude = 0, point_magnitude = std::abs(double(m[9+a])) + translation_limit;
                for (unsigned c = 0; c < 3; ++c) {
                    normal_magnitude += std::abs(double(face.normal[c])*m[c*3+a]);
                    point_magnitude += std::abs(double(face.vertices[2].position[c])*m[c*3+a]);
                }
                facing += double(normal[a])*points[2][a];
                extent += double(translation_limit)*std::abs(normal[a]);
                magnitude += normal_magnitude*point_magnitude;
                floor_scale += normal_magnitude + point_magnitude;
            }
            // Includes the validated 8*epsilon FTRV magnitude envelopes for
            // BOTH normal and position, translation and dot-product rounding.
            const double slack = 32*FLT_EPSILON*magnitude + 32*FLT_MIN*floor_scale;
            face.translation_sides = (facing + extent + slack >= 0 ? 1 : 0) |
                                     (facing - extent - slack < 0 ? 2 : 0);
            // Translation changes only dot(normal, position), not supplied
            // normal/light/specular. Bake both diffuse branches without
            // enumerating camera poses. A unit-axis probe avoids normal^2
            // overflow; zero normal has the same shade in both cases.
            unsigned axis = 0;
            for (unsigned a = 1; a < 3; ++a)
                if (std::abs(normal[a]) > std::abs(normal[axis])) axis = a;
            for (unsigned sign = 0; sign < 2; ++sign) {
                for (float &v : points[2]) v = 0;
                points[2][axis] = sign ? -1.0f : 1.0f;
                const int shade = nm_face_lighting(state.mode & 3, normal, points, light, &material);
                if (shade < 0) throw std::runtime_error("invalid translation light branch");
                face.translation_light[sign] = shade;
            }
            if ((face.translation_light[0] & 255) != (face.reference_light & 255) &&
                (face.translation_light[1] & 255) != (face.reference_light & 255))
                throw std::runtime_error("translation light branches do not cover reference shade");
        }
        // Link zero advances the strip but is not a drawable surface.
        if ((attribute >> 8) & 3) faces.push_back(std::move(face));
        switch ((attribute >> 8) & 3) {
            case 0: case 2: p0 = p2; p1 = p3; break;
            case 1: p1 = p2; break;
            case 3: p0 = p3; break;
        }
    }
    expect(0);
    if (word != words.size()) throw std::runtime_error("unconsumed model reference words");
    return faces;
}

// ponytail: inspectable per-model text is an offline intermediate, not the
// runtime pack. Freeze its binary layout only alongside the native mesh reader.
struct ModelExport {
    std::map<std::string, size_t> meshes;
    std::vector<std::string> instances;
    std::set<std::array<uint16_t, 5>> material_variants;
    std::set<std::array<uint16_t, 5>> translation_variants;
    std::set<std::array<uint16_t, 5>> all_light_variants;
    std::set<std::array<uint16_t, 5>> race_variants;
    bool race_scenery_lights=false;
    std::map<std::array<uint16_t, 5>, std::array<uint16_t, 4>> material_uv_bounds;
    void record_uv(const ModelFace &face, uint16_t light) {
        if (!(face.header[0] & 0x4000)) return;
        const std::array<uint16_t, 5> key{face.header[0], face.header[1], face.header[2], face.header[3], uint16_t(light & 255)};
        auto [at, inserted] = material_uv_bounds.try_emplace(key, std::array<uint16_t, 4>{65535,65535,0,0});
        for (const auto &v : face.vertices) {
            at->second[0] = std::min(at->second[0], v.u);
            at->second[1] = std::min(at->second[1], v.v);
            at->second[2] = std::max(at->second[2], v.u);
            at->second[3] = std::max(at->second[3], v.v);
        }
    }
    float translation_limit = 0;
    std::set<uint32_t> all_lights_models, observed_light_models; // Selected ROM parts, all256 lumas.
    size_t face_instances = 0, unique_faces = 0, vertices = 0;
    size_t rom_objects = 0, ram_objects = 0;
    uint32_t lighting_hash = 2166136261u;
    static void observe(void *data, const rt::Geo::geo_state &state,
                        const rt::Geo::ObjectSource &source, std::span<const uint32_t> words) {
        static_cast<ModelExport *>(data)->add(state, source, words);
    }
    void add(const rt::Geo::geo_state &state, const rt::Geo::ObjectSource &source,
             std::span<const uint32_t> words) {
        if (!all_lights_models.empty() && !all_lights_models.contains(source.address)) return;
        const auto faces = decode_model(state, source, words, translation_limit);
        if (faces.empty()) return;
        if (!all_lights_models.empty()) observed_light_models.insert(source.address);
        std::ostringstream mesh, instance;
        mesh << std::setprecision(std::numeric_limits<float>::max_digits10);
        instance << std::setprecision(std::numeric_limits<float>::max_digits10);
        mesh << "# native source mesh v1; local coordinates; UVs in original texture units\n"
             << "mode " << (state.mode & 3) << "\n";
        std::array<float, 3> low = faces[0].vertices[0].position, high = low;
        for (const auto &face : faces) for (const auto &v : face.vertices)
            for (unsigned axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], v.position[axis]);
                high[axis] = std::max(high[axis], v.position[axis]);
            }
        mesh << "bounds";
        for (float value : low) mesh << ' ' << value;
        for (float value : high) mesh << ' ' << value;
        mesh << '\n';
        for (const auto &face : faces) {
            material_variants.insert({face.header[0], face.header[1], face.header[2], face.header[3], uint16_t(face.reference_light & 255)});
            if (!race_scenery_lights) record_uv(face, face.reference_light);
            else for (uint16_t light : race_scenery_lumas(face,state.mode&3)) {
                race_variants.insert({face.header[0],face.header[1],face.header[2],face.header[3],light});
                record_uv(face,light);
            }
            if (!all_lights_models.empty() && (face.header[0] & 0x4000)) {
                for (unsigned light=0; light<256; ++light) {
                    all_light_variants.insert({face.header[0],face.header[1],face.header[2],face.header[3],uint16_t(light)});
                    record_uv(face,uint16_t(light));
                }
            }
            if (translation_limit > 0 && (face.header[0] & 0x4000)) {
                if (state.mode & 2) throw std::runtime_error("translation light export requires supplied-normal modes 0/1");
                for (uint16_t light : face.translation_light) {
                    const bool rear = light & 256;
                    if (!(face.translation_sides & (rear ? 2 : 1)) ||
                        (rear && !(face.attribute & (1u << 17)))) continue;
                    translation_variants.insert({face.header[0], face.header[1], face.header[2], face.header[3], uint16_t(light & 255)});
                    record_uv(face, light);
                }
            }
            lighting_hash = (lighting_hash ^ face.reference_light) * 16777619u;
            mesh << "face " << face.vertices.size() << ' ' << face.attribute;
            for (auto h : face.header) mesh << ' ' << h;
            for (auto n : face.normal) mesh << ' ' << n;
            const auto &l = face.lighting;
            mesh << ' ' << l.diffuse << ' ' << l.ambient << ' ' << l.specular_control << ' ' << l.specular_scale << '\n';
            for (const auto &v : face.vertices)
                mesh << "v " << v.position[0] << ' ' << v.position[1] << ' ' << v.position[2]
                     << ' ' << v.u << ' ' << v.v << '\n';
        }
        const auto [at, inserted] = meshes.emplace(mesh.str(), meshes.size());
        if (inserted) {
            unique_faces += faces.size();
            for (const auto &f : faces) vertices += f.vertices.size();
        }
        face_instances += faces.size();
        const auto &r = *state.raster;
        instance << at->second << ' ' << source.address << ' ' << unsigned(r.cur_window)
                 << ' ' << r.reverse;
        for (float value : state.matrix) {
            if (!std::isfinite(value)) throw std::runtime_error("non-finite instance matrix");
            instance << ' ' << value;
        }
        instance << ' ' << state.focus.x << ' ' << state.focus.y
                 << ' ' << state.light.x << ' ' << state.light.y << ' ' << state.light.p[0];
        for (auto value : r.viewport) instance << ' ' << value;
        for (auto value : r.center[r.center_sel]) instance << ' ' << value;
        instance << '\n';
        instances.push_back(instance.str());
        if ((source.address & 0x1800000) == 0x800000) ++rom_objects;
        else ++ram_objects;
    }
    std::set<std::array<uint16_t, 5>> solid_keys(bool all_lights, bool observed=false) const {
        std::set<std::array<uint16_t, 5>> keys;
        for (auto key : (race_scenery_lights && !observed ? race_variants : material_variants)) if (!(key[0] & 0x4000)) {
            if (all_lights) for (unsigned luma = 0; luma < 256; ++luma) {
                key[4] = luma; keys.insert(key);
            }
            else keys.insert(key);
        }
        return keys;
    }
    void write(const std::filesystem::path &directory, const rt::Video &video, bool all_solid_lights = false,
               const char *coverage = "selected-frame object commands; no direct geometry, full-course or dynamic classification") const {
        all_solid_lights = all_solid_lights || !all_lights_models.empty();
        if (observed_light_models != all_lights_models)
            throw std::runtime_error("selected ROM parts are missing from the export frame");
        if (instances.empty()) throw std::runtime_error("no source model instances in selected frame");
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("model directory exists; choose a new output directory");
        const auto save = [&](const auto &path, const auto &text) {
            std::ofstream out;
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out.open(path); out << text; out.close();
        };
        for (const auto &[mesh, index] : meshes)
            save(directory / ("mesh_" + std::to_string(index) + ".txt"), mesh);
        std::ostringstream list;
        list << "# mesh source-address window reverse matrix[12] focus[2] light[3] viewport[4] center[2]\n"
             << "# Matrices are object-to-reference-view, NOT a calibrated world placement.\n";
        for (const auto &instance : instances) list << instance;
        save(directory / "instances.txt", list.str());
        std::ostringstream uv;
        uv << "# h0 h1 h2 h3 luma min_u min_v max_u max_v; pre-cull source UVs, 8 units per texel\n";
        for (const auto &[key, bounds] : material_uv_bounds) {
            for (auto value : key) uv << value << ' ';
            uv << bounds[0] << ' ' << bounds[1] << ' ' << bounds[2] << ' ' << bounds[3] << '\n';
        }
        save(directory / "material_uv.txt", uv.str());
        // Pre-cull source faces, not Video::gpu_polys(). The observed luma
        // variants are preparation/lighting evidence, not a live light model.
        auto export_variants = race_scenery_lights ? race_variants : material_variants;
        const size_t base_variants=export_variants.size();
        export_variants.insert(translation_variants.begin(), translation_variants.end());
        const size_t added_translation_variants = export_variants.size()-base_variants;
        export_variants.insert(all_light_variants.begin(),all_light_variants.end());
        tools::dreamcast::export_texture_materials(directory / "materials",
            {export_variants.begin(), export_variants.end()}, video.gpu_mem());
        std::ostringstream solids;
        solids << "# h0 h1 h2 h3 luma argb; " << (all_solid_lights ? "all light levels for discovered solid headers" :
            race_scenery_lights ? "normal-race world-light envelope" : "observed pre-cull solid variants") << '\n';
        size_t solid_count = 0;
        for (const auto &key : solid_keys(all_solid_lights)) {
            rt::GeoPoly poly{};
            std::copy_n(key.begin(), 4, poly.texheader); poly.luma = uint8_t(key[4]);
            for (auto value : key) solids << value << ' ';
            solids << rt::material_solid_color(poly, video.gpu_mem()) << '\n';
            ++solid_count;
        }
        save(directory / "solid_materials.txt", solids.str());
        std::ostringstream report;
        report << "{\n  \"coverage\": " << std::quoted(coverage) << ",\n"
               << "  \"verification\": \"source point transforms, attributes and face lighting match reference raster words\",\n"
               << "  \"meshes\": " << meshes.size() << ",\n  \"instances\": " << instances.size()
               << ",\n  \"unique_faces\": " << unique_faces << ",\n  \"face_instances\": " << face_instances
               << ",\n  \"unindexed_vertices\": " << vertices << ",\n  \"rom_instances\": " << rom_objects
               << ",\n  \"ram_instances\": " << ram_objects
               << ",\n  \"observed_material_variants\": " << material_variants.size()
               << ",\n  \"observed_solid_variants\": " << solid_keys(false,true).size()
               << ",\n  \"race_scenery_lights\": " << (race_scenery_lights ? "true" : "false")
               << ",\n  \"race_material_variants\": " << race_variants.size()
               << ",\n  \"exported_solid_variants\": " << solid_count
               << ",\n  \"all_solid_lights\": " << (all_solid_lights ? "true" : "false")
               << ",\n  \"all_lights_model\": " << (all_lights_models.size()==1 ? *all_lights_models.begin() : 0)
               << ",\n  \"all_lights_models\": [";
        bool first=true;
        for(auto address:all_lights_models) { report << (first ? "" : ",") << address;first=false; }
        report << ']'
               << ",\n  \"translation_light_radius\": " << translation_limit
               << ",\n  \"added_translation_variants\": " << added_translation_variants
               << ",\n  \"added_all_light_variants\": " << export_variants.size()-base_variants-added_translation_variants
               << ",\n  \"reference_lighting_hash\": " << lighting_hash
               << ",\n  \"crtc_offset\": [" << video.crtc_x() << ',' << video.crtc_y() << ']'
               << ",\n  \"render_offset\": [" << video.render_x() << ',' << video.render_y() << ']'
               << ",\n  \"reference_screen_hash\": \"" << std::hex << video.screen_hash() << "\"\n}\n";
        save(directory / "manifest.json", report.str());
        std::printf("native_models=PASS meshes=%zu instances=%zu unique_faces=%zu face_instances=%zu vertices=%zu rom/ram=%zu/%zu light_hash=%08x\n",
                    meshes.size(), instances.size(), unique_faces, face_instances, vertices, rom_objects, ram_objects, lighting_hash);
    }
};

// Source7300's player windows live in RAM, initialized from authored main ROM.
// Export their BASE UV/header data, never one frame's animated RAM contents.
// Deliberately separate from unrestricted RAM export and ROM-part selection.
struct PlayerWindowExport {
    ModelExport models;
    bool car_parts=false;
    uint32_t address=0x13b,points=0x8050f8,header=0x800078,strips=13,faces=9;
    std::vector<uint32_t> geometry;
    std::vector<uint16_t> textures;
    template<class Read> void load(Read read,uint32_t model=0x13b) {
        if(!textures.empty()) throw std::runtime_error("player windows already initialized");
        unsigned choice;
        if(model==0x13b) choice=0;
        else if(model==0x1c8) choice=1;
        else if(model==0x241) choice=2;
        else throw std::runtime_error("unsupported canonical window model");
        const uint32_t models_by_detail[3]={0x13b,0x1c8,0x241};
        const uint32_t uv_by_detail[3]={0x8050f8,0x805168,0x8051c8};
        const uint32_t header_by_detail[3]={0x800078,0x80009c,0x8000b8};
        address=models_by_detail[choice];points=uv_by_detail[choice];header=header_by_detail[choice];
        strips=13-2*choice;faces=9-2*choice;
        const uint32_t list[5]={address,points,header,strips,0};
        for(unsigned i=0;i<5;++i) if(read(0x2890550+20*choice+4*i)!=list[i])
            throw std::runtime_error("authored player window list changed");
        geometry.resize(7+10*strips);
        for(unsigned i=0;i<geometry.size();++i) geometry[i]=read(0x29c0000+4*address+4*i);
        if(geometry.back()!=0) throw std::runtime_error("player window strip exceeds authored region");
        textures.resize(65536);
        for(unsigned i=0;i<textures.size();i+=2) {
            const uint32_t pair=read(0x29e0000+2*i);
            textures[i]=uint16_t(pair);textures[i+1]=uint16_t(pair>>16);
        }
        models.all_lights_models={address};
        const unsigned first_header=header&0x7fffff;
        unsigned current_header=first_header;
        for(unsigned i=6;i<geometry.size() && (geometry[i]&3);i+=10) {
            if(current_header>textures.size()-4 || !std::equal(textures.begin()+first_header,textures.begin()+first_header+4,textures.begin()+current_header))
                throw std::runtime_error("player windows require one shared reflection material");
            const int offset=int((geometry[i]>>12)&31);
            current_header+=(offset&16 ? offset-32 : offset)*4;
        }
        if(car_parts) models.all_lights_models.insert({0x95ff9d,0x961bbf,0x8d7b13,0x8d7c9a});
    }
    static void observe(void *data,const rt::Geo::geo_state &state,
                        const rt::Geo::ObjectSource &source,std::span<const uint32_t> words) {
        auto &self=*static_cast<PlayerWindowExport *>(data);
        if(source.address!=self.address) {self.models.add(state,source,words);return;}
        self.add(self.models,state,source,words);
    }
    void add(ModelExport &destination,const rt::Geo::geo_state &state,
             const rt::Geo::ObjectSource &source,std::span<const uint32_t> words) const {
        if(textures.size()!=65536 || source.address!=address || (source.count && source.count<strips) || source.texture_points!=points || source.texture_headers!=header ||
            !std::equal(geometry.begin(),geometry.end(),state.polygon_ram0+address))
            throw std::runtime_error("player windows differ from authored ROM geometry/layout");
        auto canonical=std::make_unique<rt::Geo::geo_state>(state);
        auto raster=std::make_unique<rt::Geo::raster_state>(*state.raster);
        std::copy(textures.begin(),textures.end(),raster->texture_ram);
        canonical->raster=raster.get();
        destination.add(*canonical,source,words);
        // Runtime offsets can cover either complete texture axis. Inhibit all
        // snapshot-only crops; the existing packer retains exact full VQ tiles.
        for(auto &[key,bounds]:destination.material_uv_bounds)
            if(std::equal(key.begin(),key.begin()+4,textures.begin()+(header&0x7fffff))) bounds={0,0,65535,65535};
    }
    void write(const std::filesystem::path &directory,const rt::Video &video) const {
        if(models.meshes.size()!=(car_parts ? 5u : 1u) || models.unique_faces!=(car_parts ? 424u : faces))
            throw std::runtime_error("expected complete player window/car models");
        models.write(directory,video,false,
            car_parts ? "normal body/shell/wheels plus canonical player windows; ROM base UV/headers; all lumas, full reflection axes; snapshot palette/view, strip captured poses before runtime" :
            "player windows: ROM29c04ec geometry checked against observed RAM13b; ROM29e0000 base UV/headers; all lumas/full UV axes; snapshot palette/view, animation belongs to native ticks");
    }
};
}
