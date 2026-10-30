#version 450
// A fragment behind the game's surface (its distance more than `margin`,
// plus `extra` on an edge, past the game's) is left out; on an edge to dash (`dash` >= 0, counted in
// dash periods along it), it shows as a dash instead, nearly opaque.
layout(push_constant) uniform Constants {
    vec2 to_clip;
    vec2 depth_scale;
    float margin;
} c;
layout(set = 0, binding = 0) uniform sampler2D game_depth;
layout(location = 0) in vec4 v_color;
layout(location = 1) noperspective in float v_depth;
layout(location = 2) noperspective in float v_dash;
layout(location = 3) flat in float v_extra;
layout(location = 0) out vec4 out_color;
void main() {
    float scene = texelFetch(game_depth, ivec2(gl_FragCoord.xy * c.depth_scale), 0).r;
    // Reversed depth: 1 / depth goes as the distance; nothing drawn there is 0.
    bool hidden = scene > 0.0 && 1.0 / v_depth - 1.0 / scene > c.margin + v_extra;
    vec4 color = v_color;
    if (hidden) {
        if (v_dash < 0.0 || fract(v_dash) >= 0.6) discard;
        color.a = max(color.a, 220.0 / 255.0);
    }
    out_color = color;
}
