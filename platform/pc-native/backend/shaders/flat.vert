#version 450
/* PVR vertices: x, y in normalised device coordinates, z = 1/w as the game
 * wrote it.  w is rebuilt from z so colours and UVs interpolate perspective
 * correctly, as on the PowerVR; depth is z itself times 2^-16 (an exact
 * scale), so the game's depth comparisons see the same values. */
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec3 frag_position;

const float DEPTH_SCALE = 1.0 / 65536.0;

void main()
{
    float z = in_position.z;
    if (!(z > 1e-30)) {
        z = 1e-30;
    }
    float w = 1.0 / z;
    gl_Position = vec4(in_position.xy * w, DEPTH_SCALE, w);
    frag_color = in_color;
    frag_uv = in_uv;
    frag_position = vec3(in_position.xy, z);
}
