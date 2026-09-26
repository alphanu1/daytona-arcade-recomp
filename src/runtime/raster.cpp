// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
//
// Model 2 3D layer, CPU reference: MAME's model2_3d_project,
// render_polygons, model2_3d_render, model2rd.ipp and the poly.h triangle
// and polygon setup, transplanted (see raster.h). Changes: polygons are drawn
// immediately (MAME's work queue keeps the same per-pixel order); float to
// int conversions that can overflow or see a NaN are pinned to the x86 result
// MAME's reference build gives, so every host agrees.

#include "runtime/raster.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace rt {

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s32 = int32_t;

inline u32 f2u(float f) { return std::bit_cast<u32>(f); }
inline u32 rgb(u32 r, u32 g, u32 b) { return 0xff000000u | (r << 16) | (g << 8) | b; } // rgb_t(r, g, b)

// x86 cvttss2si: NaN and out of range give INT32_MIN.
inline s32 to_s32(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return std::numeric_limits<s32>::min();
    return s32(f);
}

// poly.h round_coordinate (float), with NaN pinned as above.
inline s32 round_coordinate(float value) {
    if (std::isnan(value)) return std::numeric_limits<s32>::min();
    if (value >= float(std::numeric_limits<s32>::max())) return std::numeric_limits<s32>::max();
    const float ipart = std::floor(value);
    if (ipart < float(std::numeric_limits<s32>::min())) return std::numeric_limits<s32>::min();
    const float fpart = value - ipart;
    return s32(ipart) + ((fpart > 0.5f) ? 1 : 0);
}

constexpr u32 LERP(u32 x, u32 y, unsigned a) { return (x + (((y - x) * a) >> 8)) & 0x00ff00ff; }

inline s32 fast_log2(float value) {
    if (value < 0.0F) return 0;
    u32 ival = f2u(value) >> 16;
    s32 exp = s32(ival >> 7) - 127;
    static u8 const s_log2_table[128] = {
        0,   2,   5,   8,   11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,  43,  46,  48,  51,  53,  56,
        58,  61,  63,  65,  68,  70,  73,  75,  77,  80,  82,  84,  87,  89,  91,  93,  96,  98,  100, 102, 104, 106,
        109, 111, 113, 115, 117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147, 149, 151,
        153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 189, 191,
        193, 194, 196, 198, 200, 201, 203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227,
        229, 230, 232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254};
    return (exp << 8) | s_log2_table[ival & 127];
}

inline u16 get_texel(u32 base_x, u32 base_y, int x, int y, const u32 *sheet) {
    int x2 = int(base_x) + x;
    int y2 = int(base_y) + y;
    if (x2 >= 1024) {
        // texture sheets are mapped as 2048x1024 but stored in RAM as 1024x2048
        x2 -= 1024;
        y2 ^= 1024;
    }
    u32 offset = u32(((y2 / 2) * 512) + (x2 / 2));
    u32 texel = sheet[(offset >> 1) & 0x7ffff];
    if (offset & 1) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;
    return texel & 0x0f;
}

inline u16 le16(const uint8_t *base, u32 index) { return u16(base[index * 2] | base[index * 2 + 1] << 8); }

} // namespace

struct Raster::Extra {
    u8 checker = 0;
    u32 lumabase = 0, colorbase = 0;
    const u32 *texsheet[2] = {nullptr, nullptr};
    u32 texwidth = 0, texheight = 0, texx = 0, texy = 0;
    u8 texwrapx = 0, texwrapy = 0, texmirrorx = 0, texmirrory = 0, utex = 0, utexminlod = 0;
    u32 utexx = 0, utexy = 0;
    s32 texlod = 0;
    u8 luma = 0;
};

Raster::Raster() : dest_(512 * 512), fill_(512 * 512) {
    // MAME video_start
    for (int i = 0; i < 256; i++) {
        double raw_value = std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0);
        gamma_[i] = u8(raw_value);
    }
}

uint64_t Raster::hash(int minx, int maxx, int miny, int maxy) const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            const u32 px = dest_[size_t(y) * 512 + size_t(x)];
            for (int b = 0; b < 4; b++) {
                h ^= (px >> (8 * b)) & 0xff;
                h *= 0x100000001b3ULL;
            }
        }
    return h;
}

void Raster::render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                    int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
    mem_ = &mem;
    std::fill(dest_.begin(), dest_.end(), 0u);
    std::fill(fill_.begin(), fill_.end(), u8(0));
    // MAME: for window = cur_window..0, for z = min_z..max_z, each bucket
    // newest first.
    std::vector<size_t> order(polys.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    for (size_t i : order)
        if (polys[i].window <= windows) render_one(polys[i], crtc_x, crtc_y, clip_minx, clip_maxx, clip_miny, clip_maxy);
}

void Raster::render_one(GeoPoly poly, int crtc_x, int crtc_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
    // model2_3d_project
    for (int i = 0; i < poly.num_vertices; i++) {
        GeoVertex &v = poly.v[i];
        v.x = float(crtc_x + poly.center[0]) + (v.x / (v.p[0] + std::numeric_limits<float>::min()));
        v.y = float((384 - poly.center[1]) + crtc_y) - (v.y / (v.p[0] + std::numeric_limits<float>::min()));
    }

    // model2_3d_render
    Extra extra;
    const int renderer = (poly.texheader[0] >> 13) & 3;
    // rectangle(minx, maxx, miny, maxy) &= cliprect; the renderer's offsets are the CRTC's
    int clip[4] = {std::max(poly.viewport[0] + crtc_x, clip_minx), std::min(poly.viewport[2] + crtc_x, clip_maxx),
                   std::max((384 - poly.viewport[3]) + crtc_y, clip_miny), std::min((384 - poly.viewport[1]) + crtc_y, clip_maxy)};

    extra.checker = (poly.texheader[0] >> 15) & 1;
    extra.lumabase = u32(poly.texheader[1] & 0xff) << 7;
    extra.colorbase = (poly.texheader[3] >> 6) & 0x3ff;
    extra.luma = poly.luma;
    extra.texlod = poly.texlod;

    if (renderer & 2) {
        extra.texmirrorx = (poly.texheader[0] >> 8) & 1;
        extra.texmirrory = (poly.texheader[0] >> 9) & 1;
        extra.texwrapx = (poly.texheader[0] >> 6) & 1 & ~extra.texmirrorx;
        extra.texwrapy = (poly.texheader[0] >> 7) & 1 & ~extra.texmirrory;
        extra.texsheet[0] = (poly.texheader[2] & 0x1000) ? mem_->tex1 : mem_->tex0;
        extra.texsheet[1] = (poly.texheader[2] & 0x1000) ? mem_->tex0 : mem_->tex1;
        extra.texwidth = 32u << ((poly.texheader[0] >> 0) & 0x7);
        extra.texheight = 32u << ((poly.texheader[0] >> 3) & 0x7);
        extra.texx = 32u * ((poly.texheader[2] >> 0) & 0x3f);
        extra.texy = 32u * ((poly.texheader[2] >> 6) & 0x1f);
        extra.utex = (poly.texheader[0] >> 12) & 1;
        extra.utexminlod = (poly.texheader[0] >> 10) & 3;
        extra.utexx = ((poly.texheader[2] >> 13) & 1) * 128;
        extra.utexy = ((poly.texheader[2] >> 14) & 3) * 128;
        for (int i = 0; i < poly.num_vertices; i++) {
            GeoVertex &v = poly.v[i];
            v.p[0] = 1.0f / (v.p[0] + std::numeric_limits<float>::min());
            v.p[1] = v.p[1] * v.p[0] * (1.0f / 8.0f);
            v.p[2] = v.p[2] * v.p[0] * (1.0f / 8.0f);
        }
    }

    switch (poly.num_vertices) {
    case 3: render_triangle(clip, renderer, extra, poly.v[0], poly.v[1], poly.v[2]); break;
    case 4: render_polygon<4>(clip, renderer, extra, poly.v); break;
    case 5: render_polygon<5>(clip, renderer, extra, poly.v); break;
    case 6: render_polygon<6>(clip, renderer, extra, poly.v); break;
    case 7: render_polygon<7>(clip, renderer, extra, poly.v); break;
    case 8: render_polygon<8>(clip, renderer, extra, poly.v); break;
    default: break;
    }
}

void Raster::scanline(int renderer, int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o) {
    switch (renderer) {
    case 0: draw_scanline_solid<false>(y, x0, x1, start, dpdx, o); break;
    case 1: draw_scanline_solid<true>(y, x0, x1, start, dpdx, o); break;
    case 2: draw_scanline_tex<false>(y, x0, x1, start, dpdx, o); break;
    default: draw_scanline_tex<true>(y, x0, x1, start, dpdx, o); break;
    }
}

// poly.h render_triangle<3>, clip = {left, right, top, bottom} inclusive.
void Raster::render_triangle(const int *clip, int renderer, const Extra &o, const GeoVertex &_v1, const GeoVertex &_v2,
                             const GeoVertex &_v3) {
    const GeoVertex *v1 = &_v1, *v2 = &_v2, *v3 = &_v3;
    if (v2->y < v1->y) std::swap(v1, v2);
    if (v3->y < v2->y) {
        std::swap(v2, v3);
        if (v2->y < v1->y) std::swap(v1, v2);
    }
    const int32_t v1y = round_coordinate(v1->y);
    const int32_t v3y = round_coordinate(v3->y);
    const int32_t v1yclip = std::max(v1y, clip[2]);
    const int32_t v3yclip = std::min(v3y, clip[3] + 1);
    if (v3yclip - v1yclip <= 0) return;

    const float dxdy_v1v2 = (v2->y == v1->y) ? 0.0f : (v2->x - v1->x) / (v2->y - v1->y);
    const float dxdy_v1v3 = (v3->y == v1->y) ? 0.0f : (v3->x - v1->x) / (v3->y - v1->y);
    const float dxdy_v2v3 = (v3->y == v2->y) ? 0.0f : (v3->x - v2->x) / (v3->y - v2->y);

    std::array<float, 3> param_start, param_dpdx, param_dpdy;
    const float a00 = v2->y - v3->y, a01 = v3->x - v2->x, a02 = v2->x * v3->y - v3->x * v2->y;
    const float a10 = v3->y - v1->y, a11 = v1->x - v3->x, a12 = v3->x * v1->y - v1->x * v3->y;
    const float a20 = v1->y - v2->y, a21 = v2->x - v1->x, a22 = v1->x * v2->y - v2->x * v1->y;
    const float det = a02 + a12 + a22;
    if (std::abs(det) < 0.00001f) {
        for (int p = 0; p < 3; p++) param_dpdx[p] = 0.0f, param_dpdy[p] = 0.0f, param_start[p] = v1->p[p];
    } else {
        const float idet = 1.0f / det;
        for (int p = 0; p < 3; p++) {
            param_dpdx[p] = idet * (v1->p[p] * a00 + v2->p[p] * a10 + v3->p[p] * a20);
            param_dpdy[p] = idet * (v1->p[p] * a01 + v2->p[p] * a11 + v3->p[p] * a21);
            param_start[p] = idet * (v1->p[p] * a02 + v2->p[p] * a12 + v3->p[p] * a22);
        }
    }

    for (int32_t curscan = v1yclip; curscan < v3yclip; curscan++) {
        const float fully = float(curscan) + 0.5f;
        const float startx = v1->x + (fully - v1->y) * dxdy_v1v3;
        const float stopx = fully < v2->y ? v1->x + (fully - v1->y) * dxdy_v1v2 : v2->x + (fully - v2->y) * dxdy_v2v3;
        int32_t istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, clip[0]);
        istopx = std::min(istopx, clip[1] + 1);
        if (istartx >= istopx) istartx = istopx = 0;
        const float fullstartx = float(istartx) + 0.5f;
        float start[3], dpdx[3];
        for (int p = 0; p < 3; p++) {
            start[p] = param_start[p] + fullstartx * param_dpdx[p] + fully * param_dpdy[p];
            dpdx[p] = param_dpdx[p];
        }
        scanline(renderer, curscan, istartx, istopx, start, dpdx, o);
    }
}

// poly.h render_polygon<NumVerts, 3>
template <int NumVerts>
void Raster::render_polygon(const int *clip, int renderer, const Extra &o, const GeoVertex *v) {
    int minv = 0, maxv = 0;
    for (int vertnum = 1; vertnum < NumVerts; vertnum++) {
        if (v[vertnum].y < v[minv].y) minv = vertnum;
        else if (v[vertnum].y > v[maxv].y) maxv = vertnum;
    }
    const int32_t miny = round_coordinate(v[minv].y);
    const int32_t maxy = round_coordinate(v[maxv].y);
    const int32_t minyclip = std::max(miny, clip[2]);
    const int32_t maxyclip = std::min(maxy, clip[3] + 1);
    if (maxyclip - minyclip <= 0) return;

    struct poly_edge {
        const GeoVertex *v1 = nullptr, *v2 = nullptr;
        float dxdy = 0;
        std::array<float, 3> dpdy{};
    };
    poly_edge fedgelist[NumVerts - 1], bedgelist[NumVerts - 1];
    poly_edge *edgeptr = &fedgelist[0];
    for (int curv = minv; curv != maxv; curv = (curv == NumVerts - 1) ? 0 : (curv + 1)) {
        edgeptr->v1 = &v[curv];
        edgeptr->v2 = &v[(curv == NumVerts - 1) ? 0 : (curv + 1)];
        if (edgeptr->v1->y == edgeptr->v2->y) continue;
        const float ooy = 1.0f / (edgeptr->v2->y - edgeptr->v1->y);
        edgeptr->dxdy = (edgeptr->v2->x - edgeptr->v1->x) * ooy;
        for (int p = 0; p < 3; p++) edgeptr->dpdy[p] = (edgeptr->v2->p[p] - edgeptr->v1->p[p]) * ooy;
        ++edgeptr;
    }
    edgeptr = &bedgelist[0];
    for (int curv = minv; curv != maxv; curv = (curv == 0) ? (NumVerts - 1) : (curv - 1)) {
        edgeptr->v1 = &v[curv];
        edgeptr->v2 = &v[(curv == 0) ? (NumVerts - 1) : (curv - 1)];
        if (edgeptr->v1->y == edgeptr->v2->y) continue;
        const float ooy = 1.0f / (edgeptr->v2->y - edgeptr->v1->y);
        edgeptr->dxdy = (edgeptr->v2->x - edgeptr->v1->x) * ooy;
        for (int p = 0; p < 3; p++) edgeptr->dpdy[p] = (edgeptr->v2->p[p] - edgeptr->v1->p[p]) * ooy;
        ++edgeptr;
    }

    const poly_edge *ledge, *redge;
    if ((fedgelist[0].v1 == bedgelist[0].v1 && fedgelist[0].dxdy < bedgelist[0].dxdy) ||
        (fedgelist[0].v1 != bedgelist[0].v1 && fedgelist[0].v1->x < bedgelist[0].v1->x)) {
        ledge = fedgelist;
        redge = bedgelist;
    } else {
        ledge = bedgelist;
        redge = fedgelist;
    }

    for (int32_t curscan = minyclip; curscan < maxyclip; curscan++) {
        const float fully = float(curscan) + 0.5f;
        while (fully > ledge->v2->y && fully < v[maxv].y) ++ledge;
        while (fully > redge->v2->y && fully < v[maxv].y) ++redge;
        const float startx = ledge->v1->x + (fully - ledge->v1->y) * ledge->dxdy;
        const float stopx = redge->v1->x + (fully - redge->v1->y) * redge->dxdy;
        int32_t istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, clip[0]);
        istopx = std::min(istopx, clip[1] + 1);
        float start[3], dpdx[3];
        const float ldy = fully - ledge->v1->y;
        const float rdy = fully - redge->v1->y;
        const float oox = 1.0f / (stopx - startx);
        for (int p = 0; p < 3; p++) {
            const float lparam = ledge->v1->p[p] + ldy * ledge->dpdy[p];
            const float rparam = redge->v1->p[p] + rdy * redge->dpdy[p];
            const float d = (rparam - lparam) * oox;
            start[p] = lparam + (float(istartx) + 0.5f - startx) * d;
            dpdx[p] = d;
        }
        if (istartx >= istopx) istartx = istopx = 0;
        scanline(renderer, curscan, istartx, istopx, start, dpdx, o);
    }
}

template <bool Translucent>
void Raster::draw_scanline_solid(int32_t y, int32_t x0, int32_t x1, const float *, const float *, const Extra &o) {
    if (Translucent) return; // nothing to render
    u32 *const p = &dest_[size_t(y) * 512];
    u8 *const fill = &fill_[size_t(y) * 512];
    const u8 luma = o.luma >> 2;
    const u32 color = le16(mem_->palram, o.colorbase + 0x1000) & 0xffff;
    const u32 tr = gamma_[le16(mem_->colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const u32 tg = gamma_[le16(mem_->colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const u32 tb = gamma_[le16(mem_->colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    const u32 c = rgb(tr, tg, tb);
    int x = x0;
    const int dx = o.checker ? 2 : 1;
    if (o.checker && !((x ^ y) & 1)) x++;
    for (; x < x1; x += dx)
        if (fill[x] == 0) p[x] = c, fill[x] = 0xff;
}

template <bool Translucent>
uint32_t Raster::fetch_bilinear_texel(const Extra &o, int32_t miplevel, int32_t u, int32_t v) const {
    u32 tex_width, tex_height, tex_x, tex_y;
    const u32 *sheet;
    if (miplevel == -1) { // microtexture
        tex_width = 128;
        tex_height = 128;
        tex_x = o.utexx;
        tex_y = o.utexy;
        sheet = o.texsheet[1];
        u <<= 1 << o.utexminlod;
        v <<= 1 << o.utexminlod;
    } else {
        tex_width = o.texwidth >> miplevel;
        tex_height = o.texheight >> miplevel;
        tex_x = ((o.texx - 2048) >> miplevel) & 2047;
        tex_y = ((o.texy - 1024) >> miplevel) & 1023;
        sheet = o.texsheet[miplevel & 1];
        u >>= miplevel;
        v >>= miplevel;
    }
    if (o.texmirrorx && (u & s32(tex_width << 8))) u = ~u;
    if (o.texmirrory && (v & s32(tex_height << 8))) v = ~v;
    u -= 0x80;
    v -= 0x80;
    u32 ufrac = u32(u) & 0xff;
    u32 vfrac = u32(v) & 0xff;
    u32 u0 = u32(u >> 8) & (tex_width - 1);
    u32 u1 = (u0 + 1) & (tex_width - 1);
    u32 v0 = u32(v >> 8) & (tex_height - 1);
    u32 v1 = (v0 + 1) & (tex_height - 1);
    if (!o.texwrapx && u1 == 0) {
        if (ufrac >= 0x80) u0 = u1, u1++, ufrac = 0;
        else u1 = u0, u0--, ufrac = 0x100;
    }
    if (!o.texwrapy && v1 == 0) {
        if (vfrac >= 0x80) v0 = 0, v1++, vfrac = 0;
        else v1 = v0, v0--, vfrac = 0x100;
    }
    u32 tex00 = u32(get_texel(tex_x, tex_y, int(u0), int(v0), sheet)) << 4;
    u32 tex01 = u32(get_texel(tex_x, tex_y, int(u1), int(v0), sheet)) << 4;
    u32 tex10 = u32(get_texel(tex_x, tex_y, int(u0), int(v1), sheet)) << 4;
    u32 tex11 = u32(get_texel(tex_x, tex_y, int(u1), int(v1), sheet)) << 4;
    if (Translucent) {
        if (tex00 != 0xf0) tex00 |= 0x00800000;
        if (tex01 != 0xf0) tex01 |= 0x00800000;
        if (tex10 != 0xf0) tex10 |= 0x00800000;
        if (tex11 != 0xf0) tex11 |= 0x00800000;
        if (tex00 == 0x000000f0) tex00 = tex01 & 0xff;
        if (tex01 == 0x000000f0) tex01 = tex00 & 0xff;
        if (tex10 == 0x000000f0) tex10 = tex11 & 0xff;
        if (tex11 == 0x000000f0) tex11 = tex10 & 0xff;
    }
    u32 tex0x = LERP(tex00, tex01, ufrac);
    u32 tex1x = LERP(tex10, tex11, ufrac);
    if (Translucent) {
        if (tex0x == 0x000000f0) tex0x = tex1x & 0xff;
        if (tex1x == 0x000000f0) tex1x = tex0x & 0xff;
    }
    return LERP(tex0x, tex1x, vfrac);
}

template <bool Translucent>
void Raster::draw_scanline_tex(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o) {
    u32 *const p = &dest_[size_t(y) * 512];
    u8 *const fill = &fill_[size_t(y) * 512];
    float ooz = start[0], uoz = start[1], voz = start[2];
    float dooz = dpdx[0], duoz = dpdx[1], dvoz = dpdx[2];
    const s32 max_level = 30 - std::countl_zero(std::min(o.texwidth, o.texheight));
    const u32 colorbase = le16(mem_->palram, o.colorbase + 0x1000) & 0x7fff;
    const u32 cr = 0x0000 / 2 + (((colorbase >> 0) & 0x1f) << 8);
    const u32 cg = 0x4000 / 2 + (((colorbase >> 5) & 0x1f) << 8);
    const u32 cb = 0x8000 / 2 + (((colorbase >> 10) & 0x1f) << 8);

    int x = x0;
    int dx = 1;
    if (o.checker) {
        if (!((x ^ y) & 1)) {
            x++;
            ooz += dooz;
            uoz += duoz;
            voz += dvoz;
        }
        dx = 2;
        dooz *= 2.0F;
        duoz *= 2.0F;
        dvoz *= 2.0F;
    }
    for (; x < x1; x += dx, ooz += dooz, uoz += duoz, voz += dvoz) {
        if (fill[x] > 0) continue;
        float const z = 1.0F / ooz;
        s32 const mml = -o.texlod + fast_log2(z);
        s32 const level = std::clamp(mml >> 7, 0, max_level);
        s32 const u = to_s32(uoz * z * 256.0F);
        s32 const v = to_s32(voz * z * 256.0F);
        u32 t = fetch_bilinear_texel<Translucent>(o, level, u, v);
        if (mml > 0 && level < max_level) {
            u32 const t2 = fetch_bilinear_texel<Translucent>(o, level + 1, u, v);
            s32 const frac = (mml & 127) << 1;
            t = LERP(t, t2, unsigned(frac));
        } else if (o.utex && mml < 0) {
            u32 const t2 = fetch_bilinear_texel<Translucent>(o, -1, u, v);
            s32 const frac = std::min(-mml >> o.utexminlod, 127);
            t = LERP(t, t2, unsigned(frac));
        }
        if (Translucent) {
            if (t < 0x00400000) continue;
            t &= 0xff;
        }
        u8 luma = u8(u32(mem_->lumaram[(o.lumabase + (t >> 1)) * 4]) * o.luma / 256);
        luma = std::min(luma, u8(0x3f));
        const u32 tr = gamma_[le16(mem_->colorxlat, cr + luma) & 0xff];
        const u32 tg = gamma_[le16(mem_->colorxlat, cg + luma) & 0xff];
        const u32 tb = gamma_[le16(mem_->colorxlat, cb + luma) & 0xff];
        p[x] = rgb(tr, tg, tb);
        fill[x] = 0xff;
    }
}

} // namespace rt
