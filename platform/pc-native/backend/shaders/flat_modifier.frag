#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_position;
layout(location = 0) out vec4 out_color;

#include "pvr_state.glsl"

struct ModifierTriangle {
    vec4 a;
    vec4 b;
    vec4 c;
    uvec4 state;
};

layout(std430, set = 0, binding = 3) readonly buffer ModifierEvents {
    ModifierTriangle triangles[];
} modifiers;

float cross2(vec2 a, vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

bool top_left_edge(vec2 edge)
{
    /* Half-open ownership prevents a shared mesh edge from contributing two
     * XOR crossings at the same sample. NDC Y follows the submitted PVR Y. */
    return edge.y < 0.0 || (edge.y == 0.0 && edge.x > 0.0);
}

bool triangle_crosses_receiver(ModifierTriangle triangle)
{
    vec2 a = triangle.a.xy;
    vec2 b = triangle.b.xy;
    vec2 c = triangle.c.xy;
    float az = triangle.a.z;
    float bz = triangle.b.z;
    float cz = triangle.c.z;
    vec2 p = frag_position.xy;
    float denominator = cross2(b - a, c - a);
    if (abs(denominator) < 0.0000001) {
        return false;
    }
    if (denominator < 0.0) {
        vec2 swap_position = b;
        b = c;
        c = swap_position;
        float swap_depth = bz;
        bz = cz;
        cz = swap_depth;
        denominator = -denominator;
    }
    vec2 edges[3] = vec2[3](b - a, c - b, a - c);
    float coverage[3] = float[3](cross2(edges[0], p - a),
                                 cross2(edges[1], p - b),
                                 cross2(edges[2], p - c));
    const float edge_epsilon = 0.0000001;
    for (int edge = 0; edge < 3; edge++) {
        if (coverage[edge] < -edge_epsilon ||
            (abs(coverage[edge]) <= edge_epsilon &&
             !top_left_edge(edges[edge]))) {
            return false;
        }
    }
    float wa = cross2(b - p, c - p) / denominator;
    float wb = cross2(c - p, a - p) / denominator;
    float wc = 1.0 - wa - wb;
    float modifier_depth = wa * az + wb * bz + wc * cz;
    return modifier_depth > frag_position.z;
}

bool area_one_at_receiver()
{
    bool current = false;
    bool summary = false;
    bool pending = false;
    for (uint i = 0u; i < state.modifier_count; i++) {
        ModifierTriangle triangle = modifiers.triangles[i];
        if (triangle_crosses_receiver(triangle)) {
            current = triangle.state.y != 0u ? true : !current;
        }
        pending = true;
        if (triangle.state.x != 0u) {
            summary = triangle.state.z == 2u ? summary && !current
                                             : summary || current;
            current = false;
            pending = false;
        }
    }
    /* Raw open/planar OR streams do not necessarily carry VolumeLast. */
    if (pending) {
        summary = summary || current;
    }
    return summary;
}

void main()
{
    bool area_one = area_one_at_receiver();
    if ((state.modifier_area == 1u && area_one) ||
        (state.modifier_area == 2u && !area_one)) {
        discard;
    }
    out_color = pvr_shade(frag_color, frag_uv);
}
