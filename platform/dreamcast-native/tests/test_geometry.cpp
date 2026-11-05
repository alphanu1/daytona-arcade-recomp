#include "pvr_geometry.hpp"
extern "C" {
#include "../include/geometry.h"
#include "../include/meshpack.h"
#include "../include/camera.h"
}
#undef NDEBUG
#include <cassert>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using Point = std::array<double, 5>;

// Independent double-precision homogeneous clip oracle: never divide until
// all five planes are clipped. Input is already transformed/focused, not ROM.
static std::vector<Point> oracle(const nm_view_t &view, const nm_view_vertex_t *input) {
    std::vector<Point> points;
    for (unsigned i = 0; i < 3; ++i) {
        auto p = input[i]; points.push_back({p.x, p.y, p.z, p.u, p.v});
    }
    for (unsigned plane = 0; plane < 5; ++plane) {
        const auto distance = [&](Point p) {
            switch (plane) {
            case 0: return p[2] - view.near_z;
            case 1: return p[0] + (view.center[0] - double(view.bounds[0])) * p[2];
            case 2: return (view.bounds[1] - double(view.center[0])) * p[2] - p[0];
            case 3: return (view.center[1] - double(view.bounds[2])) * p[2] - p[1];
            default: return p[1] + (view.bounds[3] - double(view.center[1])) * p[2];
            }
        };
        std::vector<Point> clipped;
        for (size_t i = 0; i < points.size(); ++i) {
            auto a = points[i], b = points[(i + 1) % points.size()];
            const double da = distance(a), db = distance(b);
            if (da >= 0) clipped.push_back(a);
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                Point p;
                for (unsigned j = 0; j < 5; ++j) p[j] = a[j] + (b[j] - a[j]) * da / (da - db);
                clipped.push_back(p);
            }
        }
        points = clipped;
    }
    if (points.size() < 3) points.clear();
    for (auto &p : points) {
        const double q = 1 / p[2];
        p = {view.center[0] + p[0] * q, view.center[1] - p[1] * q, q, p[3] * q, p[4] * q};
    }
    return points;
}

static unsigned tested = 0, visible = 0, failures = 0, fast_in = 0, fast_out = 0, clipped = 0;
static double max_xy = 0, max_attribute = 0;

static void check_bounds() {
    const nm_view_t view = {{0,0}, {-1,1,-1,1}, 1};
    const float identity[12] = {1,0,0,0,1,0,0,0,1,0,0,0}, focus[2] = {1,1};
    float box[6] = {-1,-1,1,1,1,2};
    const float inner[6]={-0.25f,-0.25f,2,0.25f,0.25f,3};
    assert(nm_bounds_classify(&view,inner,identity,focus)==2);
    nm_bounds_planes_t prepared;
    nm_bounds_planes_prepare(&prepared,&view,identity,focus);
    assert(prepared.valid && nm_bounds_planes_classify(&prepared,inner)==2);
    assert(nm_bounds_classify(&view,box,identity,focus)==1);
    assert(nm_bounds_visible(&view, box, identity, focus));
    for (unsigned axis = 0; axis < 2; ++axis) for (float sign : {-1.f,1.f}) {
        float edge[6] = {0,0,1,0,0,1};
        edge[axis] = edge[axis+3] = sign;
        assert(nm_bounds_visible(&view, edge, identity, focus));
        edge[axis] = edge[axis+3] = sign * (1 + 8*FLT_EPSILON);
        assert(nm_bounds_visible(&view, edge, identity, focus));
        edge[axis] = edge[axis+3] = 3 * sign;
        assert(!nm_bounds_visible(&view, edge, identity, focus));
    }
    box[2] = box[5] = 0.5f;
    assert(!nm_bounds_visible(&view, box, identity, focus));
    box[0] = NAN;
    assert(nm_bounds_visible(&view, box, identity, focus));
    box[0] = -1;
    float bad_matrix[12]; std::memcpy(bad_matrix, identity, sizeof(identity));
    bad_matrix[0] = INFINITY;
    nm_bounds_planes_prepare(&prepared,&view,bad_matrix,focus);
    assert(!prepared.valid && nm_bounds_planes_classify(&prepared,box)==1);
    assert(nm_bounds_visible(&view, box, bad_matrix, focus));
    bad_matrix[0] = FLT_MAX; box[3] = FLT_MAX;
    assert(nm_bounds_visible(&view, box, bad_matrix, focus));

    std::mt19937 random(0x424f554e);
    std::uniform_real_distribution<float> value(-2,2), exponent(-8,8);
    unsigned rejected = 0, fully_inside=0, plane_rejected=0, plane_inside=0;
    for (unsigned trial = 0; trial < 100000; ++trial) {
        float matrix[12], lens[2] = {value(random)*100, value(random)*100};
        const float scale = std::pow(10.f, exponent(random));
        for (float &v : matrix) v = value(random)*scale;
        // Keep the original 50k rejection cases; add a narrow projected set
        // in front of the eye to exercise positive whole-block classification.
        if(trial>=50000) {
            lens[0]*=0.00001f; lens[1]*=0.00001f;
            matrix[11]+=16*scale+2;
        }
        float source[3][3];
        for (auto &p : source) for (float &v : p) v = value(random);
        for (unsigned a = 0; a < 3; ++a) {
            box[a] = std::min({source[0][a], source[1][a], source[2][a]});
            box[a+3] = std::max({source[0][a], source[1][a], source[2][a]});
        }
        const bool maybe = nm_bounds_visible(&view, box, matrix, lens);
        const int classification=nm_bounds_classify(&view,box,matrix,lens);
        nm_bounds_planes_prepare(&prepared,&view,matrix,lens);
        const int plane_class=nm_bounds_planes_classify(&prepared,box);
        plane_rejected+=plane_class==0; plane_inside+=plane_class==2;
        const int quick=nm_bounds_quick_classify(&view,box,matrix,lens);
        if(!(quick==3 || quick==plane_class)) {
            std::fprintf(stderr,"quick %d planes %d trial %u view near %g lens %g %g\n",quick,plane_class,trial,(double)view.near_z,(double)lens[0],(double)lens[1]);
            for(unsigned k=0;k<12;++k) std::fprintf(stderr,"%g ",(double)matrix[k]);
            std::fprintf(stderr,"| box %g %g %g %g %g %g\n",(double)box[0],(double)box[1],(double)box[2],(double)box[3],(double)box[4],(double)box[5]);
            std::abort();
        }
        assert((classification!=0)==maybe);
        fully_inside+=classification==2;
        rejected += !maybe;
        // Perturb affine results across the validated FTRV magnitude envelope,
        // then focus/clip. Rejected boxes must hide no triangle in any trial.
        for (unsigned variation = 0; variation < 8; ++variation) {
            nm_view_vertex_t points[3]{};
            for (unsigned v = 0; v < 3; ++v) {
                float transformed[3];
                for (unsigned a = 0; a < 3; ++a) {
                    double exact = matrix[9+a], magnitude = std::abs(exact);
                    for (unsigned c = 0; c < 3; ++c) {
                        const double term = double(source[v][c])*matrix[c*3+a];
                        exact += term; magnitude += std::abs(term);
                    }
                    transformed[a] = float(exact + (variation & (1u<<a) ? 1 : -1) *
                        (8*FLT_EPSILON*magnitude + FLT_MIN));
                }
                points[v] = {transformed[0]*lens[0], transformed[1]*lens[1], transformed[2],0,0};
                if(classification==2 || plane_class==2) {
                    assert(nm_geometry_classify(&view,points+v)==0);
                    nm_cached_vertex_t regular{},inside{};
                    nm_cache_vertex(&view,points+v,&regular);
                    nm_cache_vertex_classified(&view,points+v,&inside,1);
                    assert(std::memcmp(&regular,&inside,sizeof(regular))==0);
                }
            }
            if (!maybe || plane_class==0) {
                nm_screen_vertex_t output[NM_CLIP_VERTICES];
                assert(nm_project_triangle(&view, points, output) == 0);
                assert(oracle(view, points).empty());
            }
        }
    }
    assert(rejected > 1000 && rejected < 49000);
    assert(fully_inside>0);
    {
        /* nm_bounds_quick_classify against the planes: rotations and small boxes (car parts, the pit crew) placed
         * anywhere around the frustum and right at its planes, a screen view and a guard view, scaled matrices;
         * whenever it decides (0 or 2) the planes must say the same. */
        std::uniform_real_distribution<float> unit(-1,1), angle(0,6.2831853f), place(-1.5f,1.5f), depth(-2,300);
        const nm_view_t views[2]={view,{{248,192},{-2048,496+2048,-2048,384+2048},0.01f}};
        unsigned decided=0, agreed[3]={0,0,0}, near_plane=0, by_quick[4]={0,0,0,0};
        for(unsigned trial=0;trial<2000000;++trial) {
            const nm_view_t &v=views[trial&1];
            const float a=angle(random), b=angle(random), scale=trial%5==0 ? std::pow(2.f,unit(random)*20) : 1.f;
            float matrix[12]={std::cos(a)*std::cos(b),std::sin(a),-std::cos(a)*std::sin(b),
                -std::sin(a)*std::cos(b),std::cos(a),std::sin(a)*std::sin(b), std::sin(b),0,std::cos(b), 0,0,0};
            for(unsigned k=0;k<9;++k) matrix[k]*=scale;
            const float z=depth(random), lens[2]={280,280};
            matrix[9]=place(random)*(v.bounds[1]-v.center[0])/280*(z>0 ? z : 1); matrix[10]=place(random)*z; matrix[11]=z;
            if(trial%3==0) { /* right at a side plane: x/z = plane slope, plus a tiny offset */
                const float slope=(trial&2 ? v.bounds[1]-v.center[0] : v.center[0]-v.bounds[0])/280;
                matrix[9]=(trial&2 ? 1 : -1)*slope*z+unit(random)*std::pow(10.f,unit(random)*4-3); ++near_plane;
            }
            float box[6];
            for(unsigned k=0;k<3;++k) { const float c=unit(random)*0.5f, h=std::abs(unit(random))*0.5f; box[k]=c-h; box[k+3]=c+h; }
            nm_bounds_planes_t planes;
            nm_bounds_planes_prepare(&planes,&v,matrix,lens);
            const int exact=nm_bounds_planes_classify(&planes,box), quick=nm_bounds_quick_classify(&v,box,matrix,lens);
            if(quick!=3) {
                ++decided;
                if(quick!=exact) {
                    std::fprintf(stderr,"quick classify %d, planes %d (trial %u)\n",quick,exact,trial);
                    std::abort();
                }
            }
            ++agreed[exact]; ++by_quick[quick];
        }
        assert(decided>1000000);
        std::printf("Quick bounds classification: %u of 2000000 decided (%u outside, %u across, %u inside; %u boxes at a side "
            "plane), each as the planes (planes: %u outside, %u crossing, %u inside)\n",decided,by_quick[0],by_quick[1],by_quick[2],
            near_plane,agreed[0],agreed[1],agreed[2]);
    }
    assert(plane_rejected>1000 && plane_inside>0);
    std::printf("Prepared planes: %u rejected, %u inside; no false classifications\n",plane_rejected,plane_inside);
    std::printf("Bounds strictly inside: %u; all perturbed vertices inside\n",fully_inside);
    std::printf("Bounds: 100000 transformed boxes, %u rejected; 800000 perturbed triangles, no false rejects\n", rejected);
}
static void check(const nm_view_t &view, const nm_view_vertex_t input[3]) {
    nm_screen_vertex_t output[NM_CLIP_VERTICES];
    const unsigned count = nm_project_triangle(&view, input, output);
    assert(nm_view_valid(&view));
    nm_cached_vertex_t cache[3];
    const nm_cached_vertex_t *points[3] = {cache, cache+1, cache+2};
    for (unsigned i = 0; i < 3; ++i) nm_cache_vertex(&view, &input[i], &cache[i]);
    nm_screen_vertex_t cached[NM_CLIP_VERTICES];
    assert(nm_project_cached_triangle(&view, points, cached) == count);
    assert(std::memcmp(cached, output, count * sizeof(*output)) == 0);
    if (!(nm_cached_outside(cache+0) | nm_cached_outside(cache+1) | nm_cached_outside(cache+2))) ++fast_in;
    else if (nm_cached_outside(cache+0) & nm_cached_outside(cache+1) & nm_cached_outside(cache+2)) ++fast_out;
    else ++clipped;
    const auto expected = oracle(view, input);
    if (count != expected.size()) {
        std::fprintf(stderr, "case %u: vertex count %u != %zu\n", tested, count, expected.size());
        std::abort();
    }
    unsigned rotation = 0;
    double best = INFINITY;
    for (unsigned r = 0; r < count; ++r) {
        double score = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto p = output[i];
            const Point actual{p.x, p.y, p.q, p.uq, p.vq};
            for (unsigned a = 0; a < 5; ++a)
                score += std::abs(actual[a] - expected[(i+r)%count][a]) / std::max(1.0, std::abs(expected[(i+r)%count][a]));
        }
        if (score < best) { best = score; rotation = r; }
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto p = output[i];
        assert(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.q) && p.q > 0);
        assert(p.x >= view.bounds[0] - 0.001f && p.x <= view.bounds[1] + 0.001f);
        assert(p.y >= view.bounds[2] - 0.001f && p.y <= view.bounds[3] + 0.001f);
        const Point actual{p.x, p.y, p.q, p.uq, p.vq};
        const auto closest = expected.begin() + (i + rotation) % count;
        for (unsigned a = 0; a < 5; ++a) {
            double error = std::abs(actual[a] - (*closest)[a]);
            if (a < 2) max_xy = std::max(max_xy, error);
            else { error /= std::max(1.0, std::abs((*closest)[a])); max_attribute = std::max(max_attribute, error); }
            if (error > (a < 2 ? 0.02 : 0.0002)) {
                if (failures++ < 32) {
                    std::fprintf(stderr, "case %u vertex %u axis %u: %g != %g (error %g)\n",
                             tested, i, a, actual[a], (*closest)[a], error);
                    // Hex floats preserve exact inputs for a minimal regression case.
                    std::fprintf(stderr, "center %a %a near %a bounds %a %a %a %a\n",
                        view.center[0], view.center[1], view.near_z,
                        view.bounds[0],view.bounds[1],view.bounds[2],view.bounds[3]);
                    for (unsigned k = 0; k < 3; ++k) std::fprintf(stderr, "%a %a %a %a %a\n",
                        input[k].x, input[k].y, input[k].z, input[k].u, input[k].v);
                }
            }
        }
    }
    ++tested; visible += count != 0;
}

int main(int argc, char **argv) {
    static_assert(sizeof(nm_cached_vertex_t) == 32);
    nm_cached_vertex_t tagged{};
    for (unsigned status=1;status<=66;++status) {
        nm_cached_set_outside(&tagged,status);
        assert(nm_cached_outside(&tagged)==status);
    }
    for (float q : {FLT_MIN, 1.0f/FLT_MAX, 1.0f, FLT_MAX}) {
        tagged.q=q;
        assert(nm_cached_outside(&tagged)==0 && tagged.q==q);
    }
    // Native camera inspection uses wall time, endpoint-correct signed axes,
    // bounded movement and explicit reset. Invalid data must not mutate state.
    assert(nm_camera_axis(-128) == -1 && nm_camera_axis(127) == 1);
    assert(nm_camera_axis(-1000) == -1 && nm_camera_axis(1000) == 1);
    for (int input = -16; input <= 16; ++input) assert(nm_camera_axis(input) == 0);
    const float motion[3] = {1, -1, 0.5f};
    float at30[3] = {}, at60[3] = {};
    for (unsigned i = 0; i < 30; ++i) assert(nm_camera_update(at30, motion, 1.f/30, 0));
    for (unsigned i = 0; i < 60; ++i) assert(nm_camera_update(at60, motion, 1.f/60, 0));
    for (unsigned a = 0; a < 3; ++a) {
        assert(std::abs(at30[a]-at60[a]) < 1e-5f);
        assert(std::abs(at60[a]-3*motion[a]) < 1e-5f);
    }
    float position[3] = {};
    assert(nm_camera_update(position, motion, 10, 0));
    assert(std::abs(position[0]-0.15f) < 1e-7f); // Stall clamp.
    for (unsigned i = 0; i < 1000; ++i) assert(nm_camera_update(position, motion, 0.05f, 0));
    assert(position[0] == NM_CAMERA_LIMIT && position[1] == -NM_CAMERA_LIMIT && position[2] == NM_CAMERA_LIMIT);
    const auto unchanged = std::array{position[0], position[1], position[2]};
    for (float bad : {NAN, INFINITY, -1.f}) {
        assert(!nm_camera_update(position, motion, bad, 0));
        assert(!std::memcmp(position, unchanged.data(), sizeof(position)));
    }
    for (float bad : {NAN, INFINITY, 1.01f, -1.01f}) {
        const float invalid[3] = {0, bad, 0};
        assert(!nm_camera_update(position, invalid, 0.02f, 0));
        assert(!std::memcmp(position, unchanged.data(), sizeof(position)));
    }
    const float idle[3] = {};
    assert(nm_camera_update(position, idle, 0.02f, 0));
    assert(!std::memcmp(position, unchanged.data(), sizeof(position)));
    assert(nm_camera_update(position, idle, 0, 1));
    assert(position[0] == 0 && position[1] == 0 && position[2] == 0);
    // Every IEEE exponent, both signs, zero/nonzero/quiet/signaling mantissas.
    for (uint32_t sign : {0u, 0x80000000u}) for (uint32_t e = 0; e < 256; ++e)
        for (uint32_t mantissa : {0u, 1u, 0x3fffffu, 0x7fffffu}) {
            const uint32_t bits = sign | (e << 23) | mantissa;
            float value; std::memcpy(&value, &bits, sizeof(value));
            assert(bool(nm_finite_float(value)) == std::isfinite(value));
        }
    check_bounds();
    nm_view_t view = {{248, 192}, {0, 496, 0, 384}, 0.01f};
    nm_view_vertex_t input[3] = {{-1, -1, 1, 0, 0}, {1, -1, 1, 1, 0}, {0, 1, 1, 0.5f, 1}};
    check(view, input);
    // Boundaries, points behind the eye, and a positive near plane are distinct.
    for (float z : {-2.f, -0.01f, 0.f, 0.01f, 0.010001f, 1.f}) {
        input[0].z = z; check(view, input);
        input[1].z = z; check(view, input);
    }
    input[0].z = input[1].z = 1;
    nm_screen_vertex_t out[NM_CLIP_VERTICES];
    for (float bad : {NAN, INFINITY, -INFINITY}) {
        input[0].u = bad; assert(!nm_project_triangle(&view, input, out));
        nm_cached_vertex_t cache[3];
        const nm_cached_vertex_t *points[3] = {cache, cache+1, cache+2};
        for (unsigned i = 0; i < 3; ++i) nm_cache_vertex(&view, &input[i], &cache[i]);
        assert(nm_cached_outside(cache+0) & 32);
        assert(!nm_project_cached_triangle(&view, points, out));
    }
    input[0].u = 0;
    // Compact cache re-derives uq/vq at emission, but must still reject a
    // finite UV whose perspective product overflows during cache creation.
    for (unsigned axis = 0; axis < 2; ++axis) for (float sign : {-1.f, 1.f}) {
        auto point = input[0];
        point.z = 0.5f;
        (axis ? point.v : point.u) = sign * FLT_MAX;
        nm_cached_vertex_t cache[3];
        const nm_cached_vertex_t *points[3] = {cache, cache+1, cache+2};
        nm_view_vertex_t triangle[3] = {point, input[1], input[2]};
        for (unsigned i = 0; i < 3; ++i) nm_cache_vertex(&view, &triangle[i], &cache[i]);
        assert(nm_cached_outside(cache+0) & 32);
        assert(!nm_project_cached_triangle(&view, points, out));
        assert(!nm_project_triangle(&view, triangle, out));
        // Reusing that slot for an ordinary vertex must clear invalid state.
        nm_cache_vertex(&view, &input[0], &cache[0]);
        assert(nm_project_cached_triangle(&view, points, out) == 3);
    }
    for (float bad : {0.f, -1.f, NAN, INFINITY}) {
        view.near_z = bad; assert(!nm_project_triangle(&view, input, out));
        assert(!nm_view_valid(&view));
    }
    view.near_z = 0.01f;

    std::mt19937 rng(2701);
    std::uniform_real_distribution<float> coordinate(-800, 800), uv(-3, 3), exponent(-2, 4);
    for (unsigned trial = 0; trial < 100000; ++trial) {
        view.center[0] = 248 + float(int(rng() % 100) - 50);
        view.center[1] = 192 + float(int(rng() % 100) - 50);
        for (auto &p : input) {
            const float z = std::pow(10.f, exponent(rng));
            p = {coordinate(rng) * z, coordinate(rng) * z,
                 trial & 1 ? z : -z, uv(rng), uv(rng)};
            // Mixed eye-plane crossings, not just all-front/all-back triangles.
            if (rng() & 1) p.z = -p.z;
        }
        check(view, input);
    }

    // Actual existing PVR adapter: verify the original center/CRTC/viewport
    // conventions independently of the new triangle kernel and double oracle.
    for (int offset = -200; offset <= 200; ++offset) {
        rt::GeoPoly poly{};
        poly.num_vertices = 3; poly.texheader[0] = 0x4000;
        poly.center[0] = 248; poly.center[1] = 192;
        poly.viewport[0] = 20; poly.viewport[2] = 475;
        poly.viewport[1] = 21; poly.viewport[3] = 364;
        poly.v[0] = {-400, 300, {1, 0, 0}};
        poly.v[1] = {1200, 400, {2, 256, 0}};
        poly.v[2] = {0, -1400, {4, 128, 256}};
        view = {{float(248 + offset), float(192 - offset)}, {30, 486, 30, 374}, 0.01f};
        for (unsigned v = 0; v < 3; ++v) input[v] = {poly.v[v].x, poly.v[v].y,
            poly.v[v].p[0], poly.v[v].p[1]/256, poly.v[v].p[2]/256};
        const auto ref = daytona::project_and_clip(poly, offset, -offset, 10, 10);
        const unsigned count = nm_project_triangle(&view, input, out);
        assert(count == ref.count);
        for (unsigned i = 0; i < count; ++i) {
            bool found = false;
            for (unsigned j = 0; j < count; ++j) {
                const auto &a = out[i]; const auto &b = ref.points[j];
                found |= std::abs(a.x-b.x) < 0.001f && std::abs(a.y-b.y) < 0.001f &&
                         std::abs(a.q-b.q) < 1e-6f && std::abs(a.uq-b.uq) < 1e-6f && std::abs(a.vq-b.vq) < 1e-6f;
            }
            assert(found);
        }
    }

    if (argc == 2) {
        const unsigned before_tested = tested, before_visible = visible;
        const unsigned before_in = fast_in, before_out = fast_out, before_clip = clipped;
        nm_pack_t pack{}; assert(nm_pack_load(&pack, argv[1]));
        for (unsigned i = 0; i < pack.header->instances; ++i) {
            const auto &inst = pack.instances[i];
            const auto &mesh = pack.meshes[inst.mesh];
            const float *m = inst.matrix;
            view = {{float(pack.header->crtc[0] + inst.center[0]), float(384 - inst.center[1] + pack.header->crtc[1])},
                {float(std::max(inst.viewport[0] + pack.header->render[0], 0)),
                 float(std::min(inst.viewport[2] + pack.header->render[0] + 1, 496)),
                 float(std::max(384 - inst.viewport[3] + pack.header->render[1], 0)),
                 float(std::min(384 - inst.viewport[1] + pack.header->render[1] + 1, 384))}, 0.01f};
            const bool bounds_visible = nm_bounds_visible(&view, mesh.bounds, m, inst.focus);
            for (unsigned j = 0; j < mesh.face_count; ++j) {
                const auto &face = pack.faces[mesh.face_first + j];
                const unsigned th = pack.materials[face.material].header[0];
                for (unsigned t = 1; t + 1 < face.count; ++t) {
                    const unsigned corners[3] = {0, t, t + 1};
                    for (unsigned v = 0; v < 3; ++v) {
                        const auto &vertex = pack.vertices[mesh.vertex_first + face.indices[corners[v]]];
                        const float *p = vertex.position;
                        input[v] = {(p[0]*m[0]+p[1]*m[3]+p[2]*m[6]+m[9])*inst.focus[0],
                            (p[0]*m[1]+p[1]*m[4]+p[2]*m[7]+m[10])*inst.focus[1],
                             p[0]*m[2]+p[1]*m[5]+p[2]*m[8]+m[11],
                             float(vertex.u) / float(256u << (th & 7)),
                             float(vertex.v) / float(256u << ((th >> 3) & 7))};
                    }
                    check(view, input);
                    assert(bounds_visible || nm_project_triangle(&view, input, out) == 0);
                    // Runtime caches original-unit UVs shared by different
                    // materials, then scales perspective attributes per face.
                    nm_cached_vertex_t cache[3];
                    const nm_cached_vertex_t *pointers[3] = {cache, cache+1, cache+2};
                    const float uscale = float(256u << (th & 7)), vscale = float(256u << ((th >> 3) & 7));
                    for (unsigned v = 0; v < 3; ++v) {
                        auto raw = input[v]; raw.u *= uscale; raw.v *= vscale;
                        nm_cache_vertex(&view, &raw, &cache[v]);
                    }
                    nm_screen_vertex_t normalized[NM_CLIP_VERTICES], raw[NM_CLIP_VERTICES];
                    const unsigned count = nm_project_triangle(&view, input, normalized);
                    assert(nm_project_cached_triangle(&view, pointers, raw) == count);
                    for (unsigned v = 0; v < count; ++v) {
                        raw[v].uq /= uscale; raw[v].vq /= vscale;
                    }
                    assert(std::memcmp(normalized, raw, count * sizeof(*raw)) == 0);
                }
            }
        }
        nm_pack_unload(&pack);
        std::printf("Model pack: %u triangles, %u visible; trivial accept/reject=%u/%u, clip path=%u\n",
            tested-before_tested, visible-before_visible, fast_in-before_in, fast_out-before_out, clipped-before_clip);
    }
    std::printf("%u triangles, %u visible; max xy=%g logical pixels, attribute scaled error=%g; failures=%u\n",
                tested, visible, max_xy, max_attribute, failures);
    return failures != 0;
}
