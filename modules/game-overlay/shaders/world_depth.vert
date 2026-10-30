#version 450
// World items drawn against the game's depth (game_depth.cpp): vertices in
// screen pixels with their reversed depth (near / distance, as the game's),
// interpolated along the screen as the game's is.
layout(push_constant) uniform Constants {
    vec2 to_clip;      // 2 / screen size
    vec2 depth_scale;  // the game's depth size / screen size
    float margin;      // how far behind the game's surface still shows, in units of 1 / depth
} c;
layout(location = 0) in vec2 position;
layout(location = 1) in float depth;
layout(location = 2) in float dash;
layout(location = 3) in vec4 color;
layout(location = 4) in float extra;  // more margin (edges), in units of 1 / depth
layout(location = 0) out vec4 v_color;
layout(location = 1) noperspective out float v_depth;
layout(location = 2) noperspective out float v_dash;
layout(location = 3) flat out float v_extra;
void main() {
    v_color = color;
    v_depth = depth;
    v_dash = dash;
    v_extra = extra;
    // The depth too, reversed as the game's: our own depth image, when the
    // build is drawn solid, keeps the nearest of its faces.
    gl_Position = vec4(position * c.to_clip - 1.0, depth, 1.0);
}
