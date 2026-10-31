#version 450
// World items drawn against the game's depth (game_depth.cpp): vertices in
// screen pixels with their reversed depth (near / distance, as the game's),
// interpolated along the screen as the game's is.
layout(push_constant) uniform Constants {
    vec2 to_clip;      // 2 / screen size
    vec2 depth_scale;  // the game's depth size / screen size
    float margin;      // how far behind the game's surface still shows, in units of 1 / depth
    int mode;          // see world_depth.frag
    float own_tolerance;
    float focal;  // screen pixels per unit of tan, vertically
    vec4 eye;     // mode 4: the eye in cells of the blocks' grid; w: the relief's tolerance, in cells
    ivec4 size;   // mode 4: the grid's size in cells
    float dash_alpha;  // a hidden edge's dashes, this opaque (cells.dash_opacity)
    float back_alpha;  // the wire view: those behind the build's own faces (cells.back_edges)
    float normal_y;    // the normal maps' green: 1 as stored, -1 the other way
    float normal_faces;  // which faces have a normal map: 1 sides, 2 top, 4 bottom
    vec4 tiling;       // the solid style's textures: repeats a meter of the sides, top and bottom
} c;
layout(location = 0) in vec2 position;
layout(location = 1) in float depth;
layout(location = 2) in float dash;
layout(location = 3) in vec4 color;
layout(location = 4) in float extra;  // more margin (edges), in units of 1 / depth
layout(location = 5) in vec3 cell;    // in cells of the blocks' grid, times depth (mode 4)
layout(location = 6) in float fade;   // its opacity, times (the wire view's creases turned away)
layout(location = 7) in float tex;    // textured faces: 1, 2 sides (and a half facing the negative way), 3 top, 4 bottom; 0 none
layout(location = 0) out vec4 v_color;
layout(location = 1) noperspective out float v_depth;
layout(location = 2) noperspective out float v_dash;
layout(location = 3) flat out float v_extra;
// Times depth, as the depth goes along the screen: divided by it, the point.
layout(location = 4) noperspective out vec3 v_cell;
layout(location = 5) flat out float v_fade;
layout(location = 6) flat out float v_tex;
void main() {
    v_color = color;
    v_depth = depth;
    v_dash = dash;
    v_extra = extra;
    v_cell = cell;
    v_fade = fade;
    v_tex = tex;
    // The depth too, reversed as the game's: our own depth image, when the
    // build is drawn solid, keeps the nearest of its faces.
    gl_Position = vec4(position * c.to_clip - 1.0, depth, 1.0);
}
