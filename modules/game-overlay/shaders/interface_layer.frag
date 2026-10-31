#version 450
// The game's interface layer laid back over the world items: the game drew
// its interface into it, cleared to transparent black, so its colors are
// premultiplied and its alpha its coverage; the pipeline blends it as
// color + (1 - alpha) x what is under it.

layout(set = 0, binding = 0) uniform sampler2D interface_layer;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texelFetch(interface_layer, ivec2(gl_FragCoord.xy), 0);
}
