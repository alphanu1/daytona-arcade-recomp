#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_position;
layout(location = 0) out vec4 out_color;

#include "pvr_state.glsl"

void main()
{
    vec4 color = pvr_shade(frag_color, frag_uv);
    if ((state.flags & FLAG_PUNCH) != 0u && color.a < state.alpha_ref) {
        discard;
    }
    out_color = color;
}
