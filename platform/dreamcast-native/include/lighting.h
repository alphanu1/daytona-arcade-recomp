// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
// Isolated face lighting from runtime/geo.cpp's MAME reference. No board state.
#ifndef DAYTONA_NATIVE_LIGHTING_H
#define DAYTONA_NATIVE_LIGHTING_H
#include "meshpack.h"
#include "geometry.h"
#include <math.h>
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
#endif

/* daytona93 normal-race descriptor 0x235db8, loaded by 0x72c4..0x72f4.
 * Direction in WORLD coordinates; transform by the view rotation, never by
 * a car's body pose or camera translation, and do not normalize it.
 * Other scene descriptors have different lights; this is not a global default. */
static const float nm_race_world_light[3] = {-0.45f, -0.89f, -0.45f};

/* Unfocused view-space inputs. Points follow packed face order (p1,p0,p2).
 * Modes 0/1 use the transformed source normal WITHOUT renormalizing it.
 * Modes 2/3 derive a unit normal from points; source_normal is ignored.
 * Bit 0 of mode enables specular. Result: luma[7:0], rear[8], or -1 on
 * invalid arithmetic. Rear is evidence for culling, not a culling decision.
 * Caller supplies validated material parameters and non-null arrays. */
static inline int nm_face_lighting_eval(unsigned mode, const float source_normal[3],
    const float points[3][3], const float light[3], const nm_material_t *material, int accelerated) {
    if (mode > 3 || material->specular_control > 255) return -1;
    float normal[3];
    if (mode & 2) {
        float a[3], b[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            a[axis] = points[0][axis] - points[1][axis];
            b[axis] = points[2][axis] - points[1][axis];
        }
        normal[0] = a[1]*b[2] - a[2]*b[1];
        normal[1] = a[2]*b[0] - a[0]*b[2];
        normal[2] = a[0]*b[1] - a[1]*b[0];
        const float squared_length = normal[0]*normal[0] +
                                     normal[1]*normal[1] + normal[2]*normal[2];
        float reciprocal;
#ifdef __DREAMCAST__
        {
            if(!nm_finite_float(squared_length)) return -1;
            /* Normalization needs 1/sqrt directly, not sqrt followed by division. */
            reciprocal=shz_inv_sqrtf(squared_length);
            if(!nm_finite_float(reciprocal)) return -1;
        }
#else
        {
            const float length = (float)sqrt((double)squared_length);
            if (!nm_finite_float(length)) return -1;
            reciprocal = length ? 1.0f / length : 0.0f;
        }
#endif
        if (reciprocal) {
            for (unsigned axis = 0; axis < 3; ++axis) normal[axis] *= reciprocal;
        }
    } else {
        for (unsigned axis = 0; axis < 3; ++axis) normal[axis] = source_normal[axis];
    }
    float dotl, dotp;
#ifdef __DREAMCAST__
    if(accelerated) {
        dotl=shz_dot6f(normal[0],normal[1],normal[2],light[0],light[1],light[2]);
        dotp=shz_dot6f(normal[0],normal[1],normal[2],points[2][0],points[2][1],points[2][2]);
    } else
#else
    (void)accelerated;
#endif
    {
        dotl = normal[0]*light[0] + normal[1]*light[1] + normal[2]*light[2];
        dotp = normal[0]*points[2][0] + normal[1]*points[2][1] + normal[2]*points[2][2];
    }
    if (!nm_finite_float(dotl) || !nm_finite_float(dotp)) return -1;
    float luminance = nm_float_negative(dotl * dotp) ? 0 : fabsf(dotl);
    luminance = luminance * material->diffuse + material->ambient;
    if (mode & 1) {
        const unsigned control = material->specular_control;
        float specular = (2*dotl) * normal[2] - light[2];
        if (specular < 0 || control == 0) specular = 0;
        if (control >> 1) specular *= specular;
        if (control >> 2) specular *= specular;
        if ((control + 1) >> 3) specular *= specular;
        luminance += specular * material->specular_scale;
    }
    /* Saturation precedes float-to-int conversion, as in the reference. */
    if (luminance < 0) luminance = 0;
    if (luminance > 255) luminance = 255;
    if (!nm_finite_float(luminance)) return -1;
    return (int)luminance | (dotp < 0 ? 256 : 0);
}
static inline int nm_face_lighting(unsigned mode, const float source_normal[3],
    const float points[3][3], const float light[3], const nm_material_t *material) {
    return nm_face_lighting_eval(mode,source_normal,points,light,material,0);
}
#endif
