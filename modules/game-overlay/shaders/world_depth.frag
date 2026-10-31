#version 450
// mode 0: a fragment behind the game's surface (its distance more than
// `extra`, its face offset or an edge's 5 cm, past the game's) is left out; on an edge
// to dash (`dash` >= 0, counted in dash periods along it), it shows as a dash
// instead, nearly opaque.
// mode 1: the depth pass of the wire view (our faces' nearest depth only).
// mode 2: the wire view, with that depth (`own_depth`): a face behind the
// game's surface or behind our nearest face is left out; an edge hidden by
// the game's surface well in front of our nearest face (more than
// `own_tolerance`: something standing before the build) is left out; hidden
// otherwise (by the build itself, or by the game's blocks it sits in), it is
// dashed; else drawn plain.
// In the wire view, a dashed edge behind the build's own faces by more than
// its margin (inside the build, or at its back) is `back_alpha` opaque, one
// hidden by the world alone `dash_alpha`: told by what hides it, not by
// which way its faces turn (31/10: an edge changed as the eye crossed the
// plane of a face, and edges inside the build showed as in front).
// mode 4: the wire view in Remove mode: an edge hidden by the game's surface
// is dashed when that surface is the one of a block: walking from it toward
// the eye through the cells of `blocks` (removed or not, or the terrain),
// the game's surface is where a run of filled cells ends; air may be
// crossed between cells of the build (removed, air, removed: still seen);
// a kept block after air (a block apart before the build), or the surface
// lying in the air (a character, a tree), and it is left out (30/10, the
// user's rules).
// In the wire view, margins (the game's blocks drawn past their cells) count
// across the game's surface: seen at a grazing angle, its relief stands that
// much farther before our faces along the view (30/10: dashes vanishing on
// stairs in the distance).
layout(push_constant) uniform Constants {
    vec2 to_clip;
    vec2 depth_scale;
    float pad;
    int mode;
    float own_tolerance;  // in units of 1 / depth
    float focal;  // screen pixels per unit of tan, vertically
    vec4 eye;     // mode 4: the eye in cells of the blocks' grid; w: the relief's tolerance, in cells
    ivec4 size;   // mode 4: the grid's size in cells
    float dash_alpha;  // a hidden edge's dashes, this opaque (cells.dash_opacity)
    float back_alpha;  // the wire view: those behind the build's own faces (cells.back_edges)
    float relief;      // how much the normal maps tilt the faces (cells.texture_relief)
    float normal_faces;  // which faces have a normal map: 1 sides, 2 top, 4 bottom
    vec4 tiling;       // the solid style's textures: repeats a meter of the sides, top and bottom; w: the scene's brightness readable
    float glow;        // the solid style's glow, 1 as the game's (cells.texture_glow)
} c;
layout(set = 0, binding = 0) uniform sampler2D game_depth;
layout(set = 0, binding = 1) uniform sampler2D own_depth;
// The world's blocks around the build (mode 4): a bit per cell, x fastest,
// then y, then z, filled (kept or the build's); then as many words, the
// build's cells alone.
layout(set = 0, binding = 2, std430) readonly buffer Blocks { uint bits[]; } blocks;
layout(location = 0) in vec4 v_color;
layout(location = 1) noperspective in float v_depth;
layout(location = 2) noperspective in float v_dash;
layout(location = 3) flat in float v_extra;
layout(location = 4) noperspective in vec3 v_cell;
layout(location = 5) flat in float v_fade;
layout(location = 6) flat in float v_tex;
// The solid style's textures: sides, top, bottom (the game's BC7 levels).
layout(set = 0, binding = 3) uniform sampler2D textures[9];
// The image shown before the overlay draws, a quarter of the screen with
// three smaller levels (c.tiling.w on): the scene's brightness.
layout(set = 0, binding = 5) uniform sampler2D screen_small;
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
bool bit_at(uint index) {
    return (blocks.bits[index >> 5u] & (1u << (index & 31u))) != 0u;
}
// How far a point (in cells) lies from a cell's box.
float from_cell(vec3 p, ivec3 cell) {
    vec3 out_of = max(max(vec3(cell) - p, p - vec3(cell + 1)), vec3(0.0));
    return length(out_of);
}
bool filled_at(ivec3 cell, uint offset) {
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, c.size.xyz))) return false;
    return bit_at(offset + uint((cell.z * c.size.y + cell.y) * c.size.x + cell.x));
}
// Whether the game's surface, `surface_at` of the way from `from` (in cells)
// to the eye, is the one of a block seen through: walking the grid, it lies
// in a filled cell, or no farther than the relief (eye.w, in cells) from
// the box of the filled cell the walk just left (a corner's bumps count
// diagonally), or from a cell of the build around it. Air may be crossed
// only toward cells of the build. Out of the grid: air.
bool through_mass(vec3 from, float surface_at) {
    vec3 to = c.eye.xyz - from;
    float length_to = length(to);
    if (length_to < 1e-4) return false;
    vec3 dir = to / length_to;
    float surface = surface_at * length_to;
    vec3 seen = from + dir * surface;
    uint total = uint(c.size.x * c.size.y * c.size.z);
    uint build_from = ((total + 31u) / 32u) * 32u;
    // A hair toward the eye: off the cell boundary the edge lies on.
    vec3 p = from + dir * 0.01;
    ivec3 cell = ivec3(floor(p));
    ivec3 step_by = ivec3(sign(dir));
    vec3 inverse = 1.0 / max(abs(dir), vec3(1e-6));
    vec3 next = (vec3(cell) + max(vec3(step_by), vec3(0.0)) - p) * sign(dir) * inverse;
    next = mix(vec3(1e30), next, notEqual(step_by, ivec3(0)));
    float walked = 0.0;
    // The cell behind the edge, the build's: the run the walk starts in.
    ivec3 left = ivec3(floor(from - dir * 0.01));
    bool was_filled = true;
    bool crossed_air = false;
    for (int i = 0; i < 768; ++i) {
        bool inside = all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, c.size.xyz));
        bool filled = inside && filled_at(cell, 0u);
        if (filled) {
            // A block apart, after the air: it stands before the build.
            if (crossed_air && !filled_at(cell, build_from)) return false;
            if (walked >= surface) return true;
            left = cell;
        } else {
            if (was_filled) {
                // A run of blocks ends here: the surface is its own when as
                // near its last cell as the relief goes.
                if (filled_at(left, 0u) && from_cell(seen, left) <= c.eye.w) return true;
                crossed_air = true;
            }
            if (walked >= surface || !inside) {
                // The surface in the air: a removed block's relief around
                // it, or else a character, a tree, an object.
                ivec3 around = ivec3(floor(seen));
                for (int z = -1; z <= 1; ++z)
                    for (int y = -1; y <= 1; ++y)
                        for (int x = -1; x <= 1; ++x) {
                            ivec3 near_cell = around + ivec3(x, y, z);
                            if (filled_at(near_cell, build_from) && from_cell(seen, near_cell) <= c.eye.w) return true;
                        }
                return false;
            }
        }
        was_filled = filled;
        if (next.x < next.y && next.x < next.z) {
            walked = next.x;
            next.x += inverse.x;
            cell.x += step_by.x;
        } else if (next.y < next.z) {
            walked = next.y;
            next.y += inverse.y;
            cell.y += step_by.y;
        } else {
            walked = next.z;
            next.z += inverse.z;
            cell.z += step_by.z;
        }
    }
    return false;
}
// The scene's brightness against a day's (0.2 to 1.3, 1 when it cannot be
// read): the mean of 9 taps of the image shown at its smallest level. The
// game's build to place follows it, not the scene's hue (31/10, captures of
// a wall to build by day, at dusk, at night and by a fire: its stones as
// bright as the scene's mean, 0.37 by day, 0.10 at night; no orange by the
// fire).
float scene_brightness() {
    if (c.tiling.w < 0.5) return 1.0;
    float sum = 0.0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            sum += dot(textureLod(screen_small, vec2(0.2 + 0.3 * float(x), 0.2 + 0.3 * float(y)), 3.0).rgb,
                vec3(0.2126, 0.7152, 0.0722));
    return clamp(sum / 9.0 / 0.37, 0.2, 1.3);
}

// The relief of a face from its normal map (x and y in its first two
// channels, the green toward the image's top, as the game stores it: 31/10,
// tested in game; z made from them; tilted by c.relief), laid as its color
// (u along `along`, v along `down`): lit from above (a side, from above and
// in front), against the flat face so lit; 1 where it is flat.
float relief(vec4 stored, vec3 face, vec3 along, vec3 down) {
    vec2 xy = (stored.rg * 2.0 - 1.0) * c.relief;
    vec3 n = normalize(along * xy.x - down * xy.y + face * sqrt(max(1.0 - dot(xy, xy), 0.0)));
    vec3 light = abs(face.y) > 0.5 ? face : normalize(face + vec3(0.0, 0.8, 0.0));
    return clamp(max(dot(n, light), 0.0) / max(dot(face, light), 0.2), 0.35, 1.6);
}

// A face of the solid style as the game draws a build to place (31/10,
// captures): a hologram in blues. The brightness of the material's texture
// (its sides, top or bottom), darkened by its ambient occlusion and shaded
// by its normal map, in a ramp from navy to blue, as bright as the scene;
// a cyan glow, nearly the same day and night, where the normal map bends
// hard (the joints and the edges of stones). Laid along the world's axes at
// the game's tiling (v_cell: meters from the texture origin, times depth).
// The three are sampled alike and one kept (indexing them would want the
// same index for all the draw).
const vec3 HOLO_DARK = vec3(0.20, 0.22, 0.30);
const vec3 HOLO_LIGHT = vec3(0.52, 0.58, 0.78);
const vec3 HOLO_FLOOR = vec3(0.03, 0.05, 0.10);
const vec3 GLOW_NIGHT = vec3(0.44, 0.61, 0.94);
const vec3 GLOW_DAY = vec3(0.71, 0.81, 1.00);
vec4 textured(vec4 color) {
    if (v_tex < 0.5) return color;
    vec3 p = v_cell / v_depth;
    int kind = int(v_tex);
    float sign = fract(v_tex) > 0.25 ? -1.0 : 1.0;
    vec2 plane = kind == 1 ? vec2(p.z, -p.y) : kind == 2 ? vec2(p.x, -p.y) : vec2(p.x, p.z);
    vec3 side = texture(textures[0], plane * c.tiling.x).rgb;
    vec3 top = texture(textures[1], plane * c.tiling.y).rgb;
    vec3 bottom = texture(textures[2], plane * c.tiling.z).rgb;
    vec3 rgb = kind <= 2 ? side : kind == 3 ? top : bottom;
    // Their ambient occlusion (white when the material has none), against
    // its mean (its smallest level): the light lent by the game's surfaces
    // already holds theirs (31/10: counted twice, the wall too dark).
    float side_ao = texture(textures[3], plane * c.tiling.x).r / max(textureLod(textures[3], vec2(0.5), 16.0).r, 0.25);
    float top_ao = texture(textures[4], plane * c.tiling.y).r / max(textureLod(textures[4], vec2(0.5), 16.0).r, 0.25);
    float bottom_ao = texture(textures[5], plane * c.tiling.z).r / max(textureLod(textures[5], vec2(0.5), 16.0).r, 0.25);
    float shade = min(kind <= 2 ? side_ao : kind == 3 ? top_ao : bottom_ao, 1.5);
    // Their normal maps: the face's outward normal, and the world's axes
    // along which its texture's u and v run.
    vec4 side_n = texture(textures[6], plane * c.tiling.x);
    vec4 top_n = texture(textures[7], plane * c.tiling.y);
    vec4 bottom_n = texture(textures[8], plane * c.tiling.z);
    int faces = int(c.normal_faces + 0.5);
    int bit = kind <= 2 ? 1 : kind == 3 ? 2 : 4;
    float glow = 0.0;
    if ((faces & bit) != 0) {
        vec4 stored = kind <= 2 ? side_n : kind == 3 ? top_n : bottom_n;
        vec3 face = kind == 1 ? vec3(sign, 0.0, 0.0) : kind == 2 ? vec3(0.0, 0.0, sign)
            : kind == 3 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, -1.0, 0.0);
        vec3 along = kind == 1 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        vec3 down = kind <= 2 ? vec3(0.0, -1.0, 0.0) : vec3(0.0, 0.0, 1.0);
        shade *= relief(stored, face, along, down);
        // How far the normal map leans from the face: steep at the joints.
        glow = smoothstep(0.45, 0.9, length(stored.rg * 2.0 - 1.0)) * c.glow;
    }
    float bright = scene_brightness();
    float level = clamp(dot(rgb, vec3(0.2126, 0.7152, 0.0722)) * 1.8, 0.0, 1.0);
    vec3 body = max(mix(HOLO_DARK, HOLO_LIGHT, level) * shade * bright, HOLO_FLOOR);
    vec3 light = mix(GLOW_NIGHT, GLOW_DAY, clamp(bright, 0.0, 1.0));
    return vec4(mix(body, light, clamp(glow, 0.0, 1.0)), color.a);
}
void main() {
    if (c.mode == 1) {
        out_color = v_color;
        return;
    }
    ivec2 texel = ivec2(gl_FragCoord.xy * c.depth_scale);
    float scene = texelFetch(game_depth, texel, 0).r;
    // Reversed depth: 1 / depth goes as the distance; nothing drawn there is 0.
    bool hidden = scene > 0.0 && 1.0 / v_depth - 1.0 / scene > v_extra;
    vec4 color = v_color;
    if (c.mode >= 2) {
        float slope = 1.0;
        if (hidden) {
            slope = slope_factor(texel, 1.0 / scene);
            hidden = 1.0 / v_depth - 1.0 / scene > v_extra * slope;
        }
        float own = texelFetch(own_depth, ivec2(gl_FragCoord.xy), 0).r;
        bool behind_own = own > 0.0 && v_depth < own * 0.9999;
        float dashes_alpha = own > 0.0 && 1.0 / v_depth - 1.0 / own > v_extra ? c.back_alpha : c.dash_alpha;
        if (v_dash < 0.0) {
            if (hidden || behind_own) discard;
        } else if (c.mode == 4) {
            if (hidden && !through_mass(v_cell / v_depth, 1.0 - v_depth / scene)) discard;
            if (hidden || behind_own) {
                if (fract(v_dash) >= 0.6) discard;
                color.a = dashes_alpha;
            }
        } else {
            // What hides it stands before the build: nothing of it shows.
            float front = own > 0.0 ? own : v_depth;
            if (hidden && 1.0 / front - 1.0 / scene > c.own_tolerance * slope) discard;
            if (hidden || behind_own) {
                if (fract(v_dash) >= 0.6) discard;
                color.a = dashes_alpha;
            }
        }
        out_color = vec4(color.rgb, color.a * v_fade);
        return;
    }
    if (hidden) {
        if (v_dash < 0.0 || fract(v_dash) >= 0.6) discard;
        color.a = c.dash_alpha;
    }
    if (v_dash < 0.0) color = textured(color);
    out_color = vec4(color.rgb, color.a * v_fade);
}
