#ifndef DAYTONA_NATIVE_RENDER_CLOCK_LIGHT_H
#define DAYTONA_NATIVE_RENDER_CLOCK_LIGHT_H
/* The lumas of a PLACED actor group (include/clock_actors.h; the Jeffry statue, the horses, the birds of Expert) for
 * one placement: what model_renderer.c nm_render_world_clock_light evaluates and tests/test_clock_light.c checks
 * against the per-face reference on the host.
 *
 * lighting.h: dotl = (M n).l = n.(M^T l) with M the model's 3x3 (no renormalisation: a scale scales it); the
 * specular term of a mode-1 face with a specular control reads the view-space z of M n, n.(p2, p5, p8) of the
 * placed matrix (view times model), and of the light. So the lumas of a group depend on the frame's normals and on
 * the key below alone: lm = M^T l, and for a group with a specular material az = (p2, p5, p8) and the light's
 * view-space z. A placement whose key equals the last one lit leaves the lumas as they are. */
#include "meshpack.h"
#include <stdint.h>
#include <string.h>
#ifdef __DREAMCAST__
#include <sh4zam/shz_sh4zam.h>
#endif

typedef struct { float lm[3], az[3], light_z; } nm_clock_light_key_t;

/* A sum as computed: the render units compile with -ffast-math, under which the compiler may add the per-face
 * specular to the material's ambient first and hoist that out of the sides; the lumas are lighting.h's order,
 * ((shown diffuse) + ambient) + specular, bit for bit (the SH4 pin is free: an empty asm on the register). */
static inline float nm_clock_light_pin(float v) {
#if defined(__GNUC__) && defined(__sh__)
    __asm__("" : "+f"(v));
#else
    volatile float pinned=v; v=pinned;
#endif
    return v;
}
static inline float nm_clock_light_dot(const float a[3],const float b[3]) {
#ifdef __DREAMCAST__
    return shz_dot6f(a[0],a[1],a[2],b[0],b[1],b[2]);
#else
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
#endif
}
/* The key of a placement: model and placed as column-major 3x4, view the scene's, l the race light. A group without
 * a specular material (specular 0) reads lm alone; its az and light_z are 0 so equal placements compare equal. */
static inline void nm_clock_light_key(nm_clock_light_key_t *key,const float model[12],const float placed[12],
        const float view[12],const float l[3],int specular) {
    for(unsigned c=0;c<3;++c) key->lm[c]=nm_clock_light_dot(model+3*c,l);
    if(specular) {
        key->az[0]=placed[2]; key->az[1]=placed[5]; key->az[2]=placed[8];
        key->light_z=view[2]*l[0]+view[5]*l[1]+view[8]*l[2];
    } else {
        key->az[0]=key->az[1]=key->az[2]=0; key->light_z=0;
    }
}
/* The front and back luma index of every face of the group's mesh (faces, count; the pack's materials), for the key:
 * out[f][0] the front side's, out[f][1] the back side's or UINT16_MAX for a one-sided face, each read from the
 * face's row of the material/luma table (rows + 256 face_row[f]). specular_mode: the mesh's mode bit 0. The
 * arithmetic is lighting.h's in the reference's order; a one-sided face evaluates its front alone (the back was
 * never shown). */
static inline void nm_clock_light_faces(const nm_face_t *faces,unsigned count,const nm_material_t *materials,
        int specular_mode,const nm_clock_light_key_t *key,const uint16_t *rows,const uint16_t *face_row,
        uint16_t (*out)[2]) {
    const float *const lm=key->lm,*const az=key->az;
    const float light_z=key->light_z;
    for(unsigned f=0;f<count;++f) {
        const nm_face_t *face=faces+f;
        const nm_material_t *material=materials+face->material;
        const float *n=face->normal;
        const float dotl=nm_clock_light_dot(n,lm);
        float specular=0;
        if(specular_mode && material->specular_control) {
            const float z=nm_clock_light_dot(n,az);
            const unsigned control=material->specular_control;
            float term=(2*dotl)*z-light_z;
            if(term<0) term=0;
            if(control>>1) term*=term;
            if(control>>2) term*=term;
            if((control+1)>>3) term*=term;
            specular=term*material->specular_scale;
        }
        const uint16_t *row=rows+face_row[f]*256u;
        const unsigned sides=(face->attribute&(1u<<17)) ? 2u : 1u;
        uint16_t side[2]={0,UINT16_MAX};
        for(unsigned rear=0;rear<sides;++rear) {
            const float shown=rear ? -dotl : dotl;
            float luminance=nm_clock_light_pin((shown>0 ? shown : 0.0f)*material->diffuse+material->ambient)+specular;
            if(luminance<0) luminance=0;
            if(luminance>255) luminance=255;
            side[rear]=row[(unsigned)(int)luminance];
        }
        out[f][0]=side[0]; out[f][1]=side[1];
    }
}
#endif
