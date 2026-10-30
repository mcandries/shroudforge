#version 450
// mode 0: a fragment behind the game's surface (its distance more than
// `margin`, plus `extra` on an edge, past the game's) is left out; on an edge
// to dash (`dash` >= 0, counted in dash periods along it), it shows as a dash
// instead, nearly opaque.
// mode 1: the depth pass of the wire view (our faces' nearest depth only).
// mode 2: the wire view, with that depth (`own_depth`): a face behind the
// game's surface or behind our nearest face is left out; an edge hidden by
// the game's surface well in front of our nearest face (more than
// `own_tolerance`: something standing before the build) is left out; hidden
// otherwise (by the build itself, or by the game's blocks it sits in), it is
// dashed; else drawn plain.
// mode 3: the wire view showing what is hidden: as 2, but an edge hidden by
// something before the build shows dashed instead of left out.
// In the wire view, margins (the game's blocks drawn past their cells) count
// across the game's surface: seen at a grazing angle, its relief stands that
// much farther before our faces along the view (30/10: dashes vanishing on
// stairs in the distance).
layout(push_constant) uniform Constants {
    vec2 to_clip;
    vec2 depth_scale;
    float margin;
    int mode;
    float own_tolerance;  // in units of 1 / depth, as margin
    float focal;  // screen pixels per unit of tan, vertically
} c;
layout(set = 0, binding = 0) uniform sampler2D game_depth;
layout(set = 0, binding = 1) uniform sampler2D own_depth;
layout(location = 0) in vec4 v_color;
layout(location = 1) noperspective in float v_depth;
layout(location = 2) noperspective in float v_dash;
layout(location = 3) flat in float v_extra;
layout(location = 0) out vec4 out_color;
// Distance (in units of 1 / depth) of the game's surface at a texel; the sky far.
float far_at(ivec2 p) {
    p = clamp(p, ivec2(0), textureSize(game_depth, 0) - 1);
    float d = texelFetch(game_depth, p, 0).r;
    return d > 0.0 ? 1.0 / d : 1e30;
}
// How much longer a margin across the game's surface is along the view: 1
// facing it, up to 8 at a grazing angle. Its slope from the nearest
// neighbour on each axis (not across an edge of the surface).
float slope_factor(ivec2 p, float far) {
    float gx = min(abs(far_at(p + ivec2(1, 0)) - far), abs(far - far_at(p - ivec2(1, 0))));
    float gy = min(abs(far_at(p + ivec2(0, 1)) - far), abs(far - far_at(p - ivec2(0, 1))));
    float t = min(length(vec2(gx, gy) * c.depth_scale) * c.focal / far, 8.0);
    return sqrt(1.0 + t * t);
}
void main() {
    if (c.mode == 1) {
        out_color = v_color;
        return;
    }
    ivec2 texel = ivec2(gl_FragCoord.xy * c.depth_scale);
    float scene = texelFetch(game_depth, texel, 0).r;
    // Reversed depth: 1 / depth goes as the distance; nothing drawn there is 0.
    bool hidden = scene > 0.0 && 1.0 / v_depth - 1.0 / scene > c.margin + v_extra;
    vec4 color = v_color;
    if (c.mode >= 2) {
        float slope = 1.0;
        if (hidden) {
            slope = slope_factor(texel, 1.0 / scene);
            hidden = 1.0 / v_depth - 1.0 / scene > (c.margin + v_extra) * slope;
        }
        float own = texelFetch(own_depth, ivec2(gl_FragCoord.xy), 0).r;
        bool behind_own = own > 0.0 && v_depth < own * 0.9999;
        if (v_dash < 0.0) {
            if (hidden || behind_own) discard;
        } else {
            // What hides it stands before the build: nothing of it shows
            // (shown dashed in mode 3).
            float front = own > 0.0 ? own : v_depth;
            if (c.mode == 2 && hidden && 1.0 / front - 1.0 / scene > c.own_tolerance * slope) discard;
            if (hidden || behind_own) {
                if (fract(v_dash) >= 0.6) discard;
                color.a = max(color.a, 220.0 / 255.0);
            }
        }
        out_color = color;
        return;
    }
    if (hidden) {
        if (v_dash < 0.0 || fract(v_dash) >= 0.6) discard;
        color.a = max(color.a, 220.0 / 255.0);
    }
    out_color = color;
}
