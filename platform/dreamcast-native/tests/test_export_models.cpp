// Synthetic meshes only: check raw topology/UVs against the independent full
// reference pipeline for every mode/link type and ROM/RAM addressing path.
#include <array>
#include <bit>
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <random>
#include <span>
#include <vector>
#define private public
#include "runtime/geo.h"
#undef private
#include "../../../src/runtime/geo.cpp"
#include "../tools/export_course.hpp"

static uint64_t translation_checks;
static void check_lighting() {
    std::vector<uint8_t> polygons(256 * 4), textures(256 * 2);
    std::array<uint32_t, 0x8000> buffer{};
    auto geo = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    auto &s = geo->geo_;
    s.focus.x = 13; s.focus.y = 0.125f; // Lighting MUST precede focus.
    geo->raster_.master_z_clip = 255;
    geo->record_pushes = true;
    std::array<unsigned, 512> histogram{};
    geo->object_observer_data = &histogram;
    geo->object_observer = +[](void *out, const rt::Geo::geo_state &state,
                              const rt::Geo::ObjectSource &source, std::span<const uint32_t> words) {
        const auto faces = daytona::native::decode_model(state, source, words, 4);
        assert(faces.size() == 1); // decode_model checks all nine light bits.
        // Offline removal of sub-ambient variants must hold for every mode,
        // specular control and facing branch, including non-rigid transforms.
        assert((faces[0].reference_light & 255) >= unsigned(faces[0].lighting.ambient));
        ++(*static_cast<std::array<unsigned, 512> *>(out))[faces[0].reference_light];
        if (!(state.mode & 2)) {
            const auto &face = faces[0];
            const float *m = state.matrix;
            float normal[3]; double normal_magnitude[3]{};
            for (unsigned a = 0; a < 3; ++a) {
                normal[a] = face.normal[0]*m[a] + face.normal[1]*m[3+a] + face.normal[2]*m[6+a];
                for (unsigned c = 0; c < 3; ++c)
                    normal_magnitude[a] += std::abs(double(face.normal[c])*m[c*3+a]);
            }
            for (unsigned corner = 0; corner < 8; ++corner) {
                float point[3]; double point_magnitude[3]{};
                const auto &p = face.vertices[2].position;
                for (unsigned a = 0; a < 3; ++a) {
                    const float translation = m[9+a] - (corner & (1u<<a) ? 4.f : -4.f);
                    point[a] = p[0]*m[a] + p[1]*m[3+a] + p[2]*m[6+a] + translation;
                    point_magnitude[a] = std::abs(double(translation));
                    for (unsigned c = 0; c < 3; ++c)
                        point_magnitude[a] += std::abs(double(p[c])*m[c*3+a]);
                }
                for (unsigned perturbation = 0; perturbation < 64; ++perturbation) {
                    float n[3], q[3];
                    for (unsigned a = 0; a < 3; ++a) {
                        n[a] = float(normal[a] + (perturbation & (1u<<a) ? 1 : -1) *
                            (8*FLT_EPSILON*normal_magnitude[a] + FLT_MIN));
                        q[a] = float(point[a] + (perturbation & (8u<<a) ? 1 : -1) *
                            (8*FLT_EPSILON*point_magnitude[a] + FLT_MIN));
                    }
                    const float dot = n[0]*q[0] + n[1]*q[1] + n[2]*q[2];
                    assert(face.translation_sides & (dot < 0 ? 2 : 1));
                    ++translation_checks;
                }
            }
        }
    };
    std::mt19937 random(0x4c494748);
    const auto value = [&]() { return (int(random() % 4001) - 2000) / 1000.0f; };
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned i = 0; i < 10256; ++i) {
        s.mode = mode;
        for (auto &m : s.matrix) m = value(); // Nonuniform scale, shear/reflection.
        s.light.x = value(); s.light.y = value(); s.light.p[0] = value();
        auto &l = s.texture_parameters[0];
        l.diffuse = random() % 256; l.ambient = random() % 256;
        l.specular_scale = random() % 256; l.specular_control = i % 256;
        for (unsigned n = 0; n < 19; ++n) s.polygon_rom[n] = std::bit_cast<uint32_t>(value());
        s.polygon_rom[6] = 0x20000 | 0x100 | 2; // One triangle, normal, p2, unused p3.
        if (i < 256) { // Degenerate triangles and supplied zero normals too.
            for (unsigned n = 0; n < 16; ++n) if (n != 6) s.polygon_rom[n] = 0;
        }
        std::array<uint32_t, 4> command{0, 0, 0x800000, 1};
        geo->polys.clear(); geo->pushed.clear(); geo->raster_.poly_list_index = 0;
        geo->geo_object_data(&s, 0x800000, {command.data(), 4, 0});
    }
    unsigned front = 0, rear = 0, levels = 0;
    for (unsigned i = 0; i < 256; ++i) {
        front += histogram[i]; rear += histogram[i+256];
        levels += (histogram[i] + histogram[i+256]) != 0;
    }
    assert(front && rear && levels == 256 && front + rear == 41024);
    nm_material_t material{}; material.diffuse = 80; material.ambient = 3.75f;
    float points[3][3] = {{0,1,2}, {0,0,2}, {1,0,2}};
    float normal[3] = {0,0,2}, light[3] = {0,0,0.5f};
    assert(nm_face_lighting(0, normal, points, light, &material) == 83); // No normal renormalization.
    assert(nm_face_lighting(2, normal, points, light, &material) == (43 | 256));
    points[2][2] = -2;
    assert(nm_face_lighting(0, normal, points, light, &material) == (3 | 256));
    light[0] = std::numeric_limits<float>::quiet_NaN();
    assert(nm_face_lighting(0, normal, points, light, &material) == -1);
    light[0] = 0; points[2][2] = std::numeric_limits<float>::infinity();
    assert(nm_face_lighting(2, normal, points, light, &material) == -1);
    assert(nm_face_lighting(4, normal, points, light, &material) == -1);
    std::printf("PASS: 41024 native/reference face lights, all modes/specular controls/luma levels; front=%u rear=%u\n", front, rear);
    std::printf("PASS: %llu translated facing cases across normal/point FTRV error envelopes\n",
                static_cast<unsigned long long>(translation_checks));
}

static void check_race_scenery_lights() {
    std::mt19937 random(0x574c4954);
    const auto value=[&]() { return (int(random()%20001)-10000)/10000.f; };
    const auto check=[&](const daytona::native::ModelFace &face,unsigned mode) {
        const auto &l=face.lighting;
        const auto allowed=daytona::native::race_scenery_lumas(face,mode);
        // Independent quaternion-derived rigid rotations, not exported views.
        double q[4]={value(),value(),value(),value()}, length=0;
        for (double n:q) length+=n*n;
        for (double &n:q) n/=std::sqrt(length);
        const double x=q[0],y=q[1],z=q[2],w=q[3];
        const float m[9]={float(1-2*(y*y+z*z)),float(2*(x*y+w*z)),float(2*(x*z-w*y)),
            float(2*(x*y-w*z)),float(1-2*(x*x+z*z)),float(2*(y*z+w*x)),
            float(2*(x*z+w*y)),float(2*(y*z-w*x)),float(1-2*(x*x+y*y))};
        float normal[3],light[3],points[3][3]{};
        for (unsigned a=0;a<3;++a) {
            normal[a]=light[a]=0;
            float nm=0,lm=0;
            for (unsigned c=0;c<3;++c) {
                normal[a]+=m[c*3+a]*face.normal[c];
                light[a]+=m[c*3+a]*nm_race_world_light[c];
                nm+=std::abs(m[c*3+a]*face.normal[c]);
                lm+=std::abs(m[c*3+a]*nm_race_world_light[c]);
            }
            normal[a]+=value()*(8*FLT_EPSILON*nm+FLT_MIN);
            light[a]+=value()*(8*FLT_EPSILON*lm+FLT_MIN);
            points[2][a]=value()*2000;
        }
        nm_material_t material{};
        material.diffuse=l.diffuse; material.ambient=l.ambient;
        material.specular_control=l.specular_control; material.specular_scale=l.specular_scale;
        const int actual=nm_face_lighting(mode,normal,points,light,&material);
        assert(actual>=0 && allowed.contains(uint16_t(actual&255)));
    };
    for (unsigned i=0;i<65536;++i) {
        daytona::native::ModelFace face{};
        for (float &n : face.normal) n=4*value();
        auto &l=face.lighting;
        l.diffuse=random()%256; l.ambient=i%4 ? random()%256 : 255;
        l.specular_control=random()%256; l.specular_scale=l.ambient==255 ? random()%256 : 0;
        check(face,i&1);
    }
    // Variable specular (mode 1, ambient below 255, control and scale non-zero;
    // Expert's glass and metal): every luma the term reaches under a rigid view.
    for (unsigned i=0;i<65536;++i) {
        daytona::native::ModelFace face{};
        for (float &n : face.normal) n=4*value();
        auto &l=face.lighting;
        l.diffuse=random()%256; l.ambient=random()%255;
        l.specular_control=1+random()%255; l.specular_scale=1+random()%255;
        assert(daytona::native::race_scenery_specular(l,1));
        check(face,1);
    }
    daytona::native::ModelFace bad{};
    const auto rejects=[&](unsigned mode) {
        try { daytona::native::race_scenery_lumas(bad,mode); }
        catch (const std::runtime_error &) { return true; }
        return false;
    };
    assert(rejects(2) && rejects(3));
    bad.lighting.specular_control=bad.lighting.specular_scale=1;
    assert(!rejects(1));
    bad.lighting.specular_scale=-1; assert(rejects(1));
    bad.lighting.specular_scale=1; bad.lighting.specular_control=256; assert(rejects(1));
    bad.lighting.specular_control=0;
    bad.lighting.specular_scale=0; bad.normal[0]=4.01f; assert(rejects(0));
    bad.normal[0]=NAN; assert(rejects(0));
    puts("PASS: 131072 rigid-view scenery light envelopes, variable specular, FTRV perturbations and unsupported material rejection");
}

int main() {
    check_race_scenery_lights();
    check_lighting();
    using namespace daytona::native;
    std::map<uint32_t, uint32_t> vehicle_ram{{0x501220,0x510000}, {0x501224,0x510300}, {0x5012f0,0x510000}};
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t at = 0x510000 + i*0x300;
        vehicle_ram[at+8] = 0x300;
        vehicle_ram[at+0x88] = 0x510000 + ((i+1)%16)*0x300;
        vehicle_ram[at+0x8c] = 0x510000 + ((i+15)%16)*0x300;
    }
    const auto vehicle_read = [&](uint32_t at) { return vehicle_ram[at]; };
    const auto neighbors = vehicle_neighbors(vehicle_read, 16);
    for (unsigned i = 0; i < 14; ++i) assert(neighbors[i] == 0x510000 + ((i+11)%16)*0x300);
    for (const auto [address, value] : {std::pair{0x501220u,0x510001u},
            {0x501224u,0xfffffffcu}, {0x501220u,0xfff00000u},
            {0x510308u,0xfffffffcu}, {0x510088u,0x1000000u},
            {0x51038cu,0x510300u}, {0x5012f0u,0u}}) {
        bool rejected = false;
        try { vehicle_neighbors([&](uint32_t at) { return at == address ? value : vehicle_read(at); }, 16); }
        catch (const std::runtime_error &) { rejected = true; }
        assert(rejected);
    }
    for (unsigned count : {0u,65u}) {
        bool rejected = false;
        try { vehicle_neighbors(vehicle_read, count); }
        catch (const std::runtime_error &) { rejected = true; }
        assert(rejected);
    }
    std::puts("PASS: vehicle pool/link validation and wrapped 14-entry neighborhood");
    std::array<uint32_t, 261> course_words{};
    std::fill_n(course_words.begin(), 256, 0x02000400);
    course_words[256] = 0x800004;
    course_words[257] = 12; course_words[258] = 16; course_words[259] = 7;
    const auto course_read = [&](uint32_t address) -> uint32_t {
        if (address == 0x501460) return 2;
        if (address == 0x501420 || address == 0x39b8) return 0x02000000;
        return course_words.at((address - 0x02000000)/4);
    };
    CourseExport course;
    course.load(course_read);
    assert(course.slot == 2 && course.keys.size() == 1);
    for (const auto &cell : course.cells)
        assert(cell.size() == 1 && cell[0].model == 0x800004 && cell[0].cost == 7);
    for (const auto [address, value] : {std::pair{0x501460u,4u}, {0x501420u,0u},
            {0x2000000u,0x2000001u}, {0x2000400u,0x1000000u}, {0x2000404u,0x800000u}}) {
        CourseExport bad;
        bool rejected = false;
        try { bad.load([&](uint32_t at) { return at == address ? value : course_read(at); }); }
        catch (const std::runtime_error &) { rejected = true; }
        assert(rejected);
    }
    std::puts("PASS: scenery table traversal, aliased cell records and malformed pointers/slots");
    ModelExport solid_test;
    ModelFace uv_face{};
    uv_face.header = {0x4000,0,0,64};
    uv_face.vertices = {{{0,0,0},8,32}, {{0,0,0},24,16}};
    solid_test.record_uv(uv_face, 300); // Rear flag is not material identity.
    uv_face.vertices = {{{0,0,0},0,64}};
    solid_test.record_uv(uv_face, 44);
    assert((solid_test.material_uv_bounds.at({0x4000,0,0,64,44}) == std::array<uint16_t,4>{0,16,24,64}));
    uv_face.header[0] = 0;
    solid_test.record_uv(uv_face, 44);
    assert(solid_test.material_uv_bounds.size() == 1);
    solid_test.material_variants = {{0,0,0,64,17}, {0,0,0,64,231},
                                   {0,0,0,128,42}, {0x4000,0,0,64,17}};
    assert(solid_test.solid_keys(false).size() == 3);
    const auto all_solids = solid_test.solid_keys(true);
    assert(all_solids.size() == 512);
    for (unsigned luma = 0; luma < 256; ++luma) {
        assert(all_solids.contains({0,0,0,64,uint16_t(luma)}));
        assert(all_solids.contains({0,0,0,128,uint16_t(luma)}));
        assert(!all_solids.contains({0x4000,0,0,64,uint16_t(luma)}));
    }
    unsigned cases = 0, checked_faces = 0;
    for (unsigned mode = 0; mode < 4; ++mode)
    for (uint32_t bank : {0u, 0x800000u, 0x1000000u})
    for (bool ram_texture : {false, true})
    for (bool exact_count : {false, true}) {
        std::vector<uint8_t> polygons(256 * 4), textures(256 * 2);
        for (unsigned i = 0; i < 256; ++i) {
            const uint16_t texel = uint16_t(i * 7);
            std::memcpy(&textures[i * 2], &texel, 2);
        }
        std::array<uint32_t, 0x8000> buffer{};
        auto geo = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
        auto &s = geo->geo_;
        auto &r = geo->raster_;
        for (unsigned i = 0; i < 0x10000; ++i) r.texture_ram[i] = uint16_t(i * 11);
        s.mode = mode;
        s.matrix[0] = 2; s.matrix[4] = 3; s.matrix[8] = 4;
        s.matrix[9] = 1; s.matrix[10] = -2; s.matrix[11] = 40;
        s.focus.x = 2; s.focus.y = 3;
        s.texture_parameters[0].ambient = 64;
        r.master_z_clip = 0xff;
        auto *memory = bank == 0x800000 ? s.polygon_rom :
                       bank == 0x1000000 ? s.polygon_ram1 : s.polygon_ram0;
        const unsigned size = bank == 0x800000 ? 256 : 0x8000;
        unsigned at = size - 4; // Source cursor crosses the memory boundary.
        const auto put = [&](uint32_t w) { memory[at++ & (size - 1)] = w; };
        const auto point = [&](float x, float y, float z) {
            put(std::bit_cast<uint32_t>(x)); put(std::bit_cast<uint32_t>(y)); put(std::bit_cast<uint32_t>(z));
        };
        point(0, 0, 1); point(0, 1, 1);
        for (unsigned link = 0; link < 12; ++link) {
            // Alternating positive/negative texture header offsets and all links.
            put(0x20000 | ((link & 1 ? 31u : 1u) << 12) | ((link % 4) << 8) | ((link / 4) & 1 ? 2 : 1));
            point(0, 0, 1); // normal or ignored storage
            point(float(link + 1), float(link % 3), 1);
            point(float(link + 2), float(link % 5), 1);
        }
        put(0); point(0, 0, 0); // explicit stream terminator and unused normal
        std::vector<ModelFace> faces;
        geo->record_pushes = true;
        geo->object_observer_data = &faces;
        geo->object_observer = +[](void *out, const rt::Geo::geo_state &state,
                                   const rt::Geo::ObjectSource &source, std::span<const uint32_t> words) {
            *static_cast<std::vector<ModelFace> *>(out) = decode_model(state, source, words);
            // Independent mutable state, same ROM owner and command. All 48
            // topology/mode/bank cases must emit identical reference words.
            std::vector<uint8_t> raster_before(sizeof(*state.raster));
            std::memcpy(raster_before.data(), state.raster, raster_before.size());
            auto inspection = std::make_unique<rt::Geo>(state);
            inspection->record_pushes = true;
            inspection->inspect_object(source);
            assert(std::equal(words.begin(), words.end(), inspection->pushed.begin(), inspection->pushed.end()));
            assert(std::memcmp(raster_before.data(), state.raster, raster_before.size()) == 0);
            bool no_buffer = false;
            try { inspection->parse(0); }
            catch (const rt::GeoFatal &) { no_buffer = true; }
            assert(no_buffer);
            auto invalid_command = source;
            invalid_command.opcode |= 0x80000000;
            bool invalid_rejected = false;
            try { inspection->inspect_object(invalid_command); }
            catch (const rt::GeoFatal &) { invalid_rejected = true; }
            assert(invalid_rejected);
            auto bad = std::vector<uint32_t>(words.begin(), words.end());
            bad[3] ^= 1;
            bool rejected = false;
            try { decode_model(state, source, bad); }
            catch (const std::runtime_error &) { rejected = true; }
            assert(rejected);
            rejected = false;
            try { decode_model(state, source, words.first(words.size() - 1)); }
            catch (const std::runtime_error &) { rejected = true; }
            assert(rejected);
        };
        const uint32_t texture_bank = ram_texture ? 0x800000 : 0;
        std::array<uint32_t, 4> command{texture_bank | 250, texture_bank | 252,
                                      bank | (size - 4), exact_count ? 12u : 20u};
        geo->geo_object_data(&s, 0x800000, {command.data(), 4, 0});
        assert(faces.size() == 9 && geo->polys.size() == faces.size());
        for (size_t i = 0; i < faces.size(); ++i) {
            const auto &face = faces[i]; const auto &poly = geo->polys[i];
            assert(face.vertices.size() == poly.num_vertices);
            assert((face.reference_light & 255) == poly.luma);
            for (unsigned h = 0; h < 4; ++h) assert(face.header[h] == poly.texheader[h]);
            for (size_t v = 0; v < face.vertices.size(); ++v) {
                const auto &src = face.vertices[v]; const auto &got = poly.v[v];
                assert((src.position[0] * 2 + 1) * 2 == got.x);
                assert((src.position[1] * 3 - 2) * 3 == got.y);
                assert(src.position[2] * 4 + 40 == got.p[0]);
                assert(src.u == got.p[1] && src.v == got.p[2]);
            }
            ++checked_faces;
        }
        // Same parse with observation disabled must preserve every raster word.
        const auto recorded = geo->pushed;
        geo->object_observer = nullptr;
        geo->pushed.clear(); geo->polys.clear();
        geo->geo_object_data(&s, 0x800000, {command.data(), 4, 0});
        assert(recorded == geo->pushed);
        // Reject every face geometrically: source materials must still exist.
        for (auto &center : r.clip_plane) for (auto &plane : center) {
            plane.normal = {}; plane.distance = 1;
        }
        ModelExport exported;
        geo->object_observer = ModelExport::observe;
        geo->object_observer_data = &exported;
        geo->pushed.clear(); geo->polys.clear();
        geo->geo_object_data(&s, 0x800000, {command.data(), 4, 0});
        std::set<std::array<uint16_t, 5>> expected_materials;
        for (const auto &face : faces)
            expected_materials.insert({face.header[0], face.header[1], face.header[2], face.header[3], uint16_t(face.reference_light & 255)});
        assert(geo->polys.empty() && exported.face_instances == faces.size());
        assert(exported.material_variants == expected_materials);
        for (const auto &face : faces) if (face.header[0] & 0x4000) {
            const auto &bounds = exported.material_uv_bounds.at({face.header[0], face.header[1],
                face.header[2], face.header[3], uint16_t(face.reference_light & 255)});
            for (const auto &v : face.vertices)
                assert(bounds[0] <= v.u && bounds[1] <= v.v && bounds[2] >= v.u && bounds[3] >= v.v);
        }
        ModelExport isolated;
        isolated.all_lights_models={command[2]+1};
        geo->object_observer_data=&isolated;
        geo->pushed.clear();
        geo->geo_object_data(&s,0x800000,{command.data(),4,0});
        assert(isolated.instances.empty());
        isolated.all_lights_models.insert(command[2]);
        geo->pushed.clear();
        geo->geo_object_data(&s,0x800000,{command.data(),4,0});
        assert(isolated.instances.size()==1 && isolated.meshes==exported.meshes);
        assert(isolated.observed_light_models==std::set<uint32_t>{command[2]});
        assert(geo->pushed==recorded); // Filtering/expansion do not alter source execution.
        for (const auto &face : faces) if (face.header[0]&0x4000) for (unsigned l=0;l<256;++l) {
            const std::array<uint16_t,5> key{face.header[0],face.header[1],face.header[2],face.header[3],uint16_t(l)};
            assert(isolated.all_light_variants.contains(key));
            const auto &bounds=isolated.material_uv_bounds.at(key);
            for (const auto &v : face.vertices)
                assert(bounds[0]<=v.u && bounds[1]<=v.v && bounds[2]>=v.u && bounds[3]>=v.v);
        }
        if(mode==1 && bank==0 && ram_texture && !exact_count) {
            PlayerWindowExport windows;
            std::array<uint32_t,137> authored{};
            for(unsigned i=0;i<authored.size();++i) authored[i]=memory[(size-4+i)&(size-1)];
            std::copy(authored.begin(),authored.end(),s.polygon_ram0+0x13b);
            const auto texture=[](unsigned i)->uint16_t {
                if(i>=0x78 && i<0x9c) return (i&3)==0 ? 0x4009 : 0;
                return uint16_t(i*7);
            };
            const auto read=[&](uint32_t address)->uint32_t {
                const uint32_t list[5]={0x13b,0x8050f8,0x800078,13,0};
                if(address>=0x2890550 && address<0x2890564) return list[(address-0x2890550)/4];
                if(address>=0x29c04ec && address<0x29c04ec+4*authored.size()) return authored[(address-0x29c04ec)/4];
                assert(address>=0x29e0000 && address<0x2a00000 && address%4==0);
                const unsigned i=(address-0x29e0000)/2;
                return texture(i)|(uint32_t(texture(i+1))<<16);
            };
            for(uint32_t bad_address:{0x2890550u,0x29c070cu}) {
                PlayerWindowExport invalid;bool rejected=false;
                try {invalid.load([&](uint32_t a){return a==bad_address ? 1u : read(a);});}
                catch(const std::runtime_error &) {rejected=true;}
                assert(rejected);
            }
            windows.load(read);
            std::array<uint32_t,4> window_command{0x8050f8,0x800078,0x13b,5000};
            const auto draw_windows=[&]() {
                geo->pushed.clear();geo->object_observer=PlayerWindowExport::observe;
                geo->object_observer_data=&windows;
                geo->geo_object_data(&s,0x800000,{window_command.data(),4,0});
            };
            const std::vector<uint16_t> original_ram(std::begin(r.texture_ram),std::end(r.texture_ram));
            draw_windows();
            assert(std::equal(original_ram.begin(),original_ram.end(),r.texture_ram));
            assert(windows.models.meshes.size()==1 && windows.models.instances.size()==1);
            const auto base_mesh=windows.models.meshes;
            for(unsigned i=0x50f8;i<0x5168;++i) r.texture_ram[i]^=65535;
            for(unsigned i=0x78;i<0x9c;++i) r.texture_ram[i]=0;
            draw_windows();
            assert(windows.models.meshes==base_mesh && windows.models.instances.size()==2);
            for(const auto &[key,bounds]:windows.models.material_uv_bounds)
                assert((bounds==std::array<uint16_t,4>{0,0,65535,65535}));
            assert(windows.models.all_light_variants.size()==256);
            for(unsigned bad:{0u,1u,2u}) {
                if(bad==0) s.polygon_ram0[0x13b]^=1;
                else if(bad==1) window_command[0]^=1;
                else window_command[3]=12;
                bool rejected=false;
                try {draw_windows();} catch(const std::runtime_error &) {rejected=true;}
                assert(rejected && windows.models.instances.size()==2);
                if(bad==0) s.polygon_ram0[0x13b]^=1;
                else if(bad==1) window_command[0]^=1;
                else window_command[3]=5000;
            }
            std::puts("PASS: ROM-initialized windows, animated UV/header independence, full-axis bounds and changed geometry/layout rejection");
        }
        ++cases;
    }
    std::printf("PASS: %u model modes/banks, %u source faces, topology/UVs/transforms, pre-cull materials and observer isolation\n", cases, checked_faces);
}
