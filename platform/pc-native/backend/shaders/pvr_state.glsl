/* Shared by the fragment shaders: TexturePush in pc_endjinn_vulkan.cpp. */
layout(set = 0, binding = 0) uniform sampler2D color_texture;
layout(set = 0, binding = 1) uniform usampler2D index_texture;
layout(set = 0, binding = 2) uniform sampler2D palette_texture;

layout(push_constant) uniform TextureState {
    uint indexed;
    uint palette_base;
    uint filter_mode;
    uint flags;
    uint modifier_count;
    uint modifier_area;
    uint tex_env;
    float color_scale;
    uint mip_bias;
    float alpha_ref;
    uint reserved0;
    uint reserved1;
} state;

const uint FLAG_TEXTURED = 1u;
const uint FLAG_USE_ALPHA = 2u;
const uint FLAG_IGNORE_TEX_ALPHA = 4u;
const uint FLAG_PUNCH = 8u;
const uint FLAG_CLAMP_U = 16u;
const uint FLAG_CLAMP_V = 32u;
const uint FLAG_FLIP_U = 64u;
const uint FLAG_FLIP_V = 128u;

float address_axis(float t, bool clamp_axis, bool flip_axis)
{
    if (clamp_axis) {
        return clamp(t, 0.0, 1.0);
    }
    if (flip_axis) {
        return 1.0 - abs(mod(t, 2.0) - 1.0);
    }
    return t;
}

vec2 address(vec2 uv)
{
    return vec2(address_axis(uv.x, (state.flags & FLAG_CLAMP_U) != 0u, (state.flags & FLAG_FLIP_U) != 0u),
                address_axis(uv.y, (state.flags & FLAG_CLAMP_V) != 0u, (state.flags & FLAG_FLIP_V) != 0u));
}

ivec2 texel_address(ivec2 pixel, ivec2 size)
{
    if ((state.flags & FLAG_CLAMP_U) != 0u) {
        pixel.x = clamp(pixel.x, 0, size.x - 1);
    } else {
        pixel.x = (pixel.x % size.x + size.x) % size.x;
    }
    if ((state.flags & FLAG_CLAMP_V) != 0u) {
        pixel.y = clamp(pixel.y, 0, size.y - 1);
    } else {
        pixel.y = (pixel.y % size.y + size.y) % size.y;
    }
    return pixel;
}

vec4 palette_color(ivec2 pixel, int level)
{
    ivec2 size = textureSize(index_texture, level);
    uint index = texelFetch(index_texture, texel_address(pixel, size), level).r;
    return texelFetch(palette_texture, ivec2(int(state.palette_base + index), 0), 0);
}

/* The PVR's D (texels per pixel) times the header's mip bias (4 = 1.0). */
float mip_scale()
{
    return float(max(state.mip_bias, 1u)) / 4.0;
}

vec4 texture_color(vec2 raw_uv)
{
    vec2 uv = address(raw_uv);
    if (state.indexed == 0u) {
        return textureGrad(color_texture, uv, dFdx(raw_uv) * mip_scale(), dFdy(raw_uv) * mip_scale());
    }
    int levels = textureQueryLevels(index_texture);
    int level = 0;
    if (levels > 1) {
        vec2 size0 = vec2(textureSize(index_texture, 0));
        vec2 dx = dFdx(raw_uv) * size0;
        vec2 dy = dFdy(raw_uv) * size0;
        float d = max(length(dx), length(dy)) * mip_scale();
        level = clamp(int(floor(log2(max(d, 1e-8)))), 0, levels - 1);
    }
    vec2 position = uv * vec2(textureSize(index_texture, level)) - 0.5;
    if (state.filter_mode == 0u) {
        return palette_color(ivec2(floor(position + 0.5)), level);
    }
    ivec2 pixel = ivec2(floor(position));
    vec2 weight = fract(position);
    return mix(mix(palette_color(pixel, level), palette_color(pixel + ivec2(1, 0), level), weight.x),
               mix(palette_color(pixel + ivec2(0, 1), level), palette_color(pixel + ivec2(1, 1), level), weight.x),
               weight.y);
}

/* TSP shading: texture environment, vertex/texture alpha switches, then the
 * cheap-shadow scale and the punch-through alpha test. */
vec4 pvr_shade(vec4 vertex_color, vec2 raw_uv)
{
    vec4 col = vertex_color;
    if ((state.flags & FLAG_USE_ALPHA) == 0u) {
        col.a = 1.0;
    }
    vec4 c = col;
    if ((state.flags & FLAG_TEXTURED) != 0u) {
        vec4 tex = texture_color(raw_uv);
        if ((state.flags & FLAG_IGNORE_TEX_ALPHA) != 0u) {
            tex.a = 1.0;
        }
        if (state.tex_env == 0u) {          /* REPLACE */
            c = tex;
        } else if (state.tex_env == 1u) {   /* MODULATE */
            c = vec4(tex.rgb * col.rgb, tex.a);
        } else if (state.tex_env == 2u) {   /* DECAL */
            c = vec4(tex.rgb * tex.a + col.rgb * (1.0 - tex.a), col.a);
        } else {                            /* MODULATEALPHA */
            c = tex * col;
        }
    }
    c.rgb *= state.color_scale;
    return c;
}
