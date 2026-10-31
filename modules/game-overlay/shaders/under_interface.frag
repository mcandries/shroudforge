#version 450
// The game's interface put back over the world items: where the image shown
// (copied before the overlay drew) differs from the scene copied before the
// game drew its interface, the image shown is written again; elsewhere,
// nothing. A translucent part of the interface is so put back whole: what
// it covers shows the scene, not the build.

layout(set = 0, binding = 0) uniform sampler2D shown;
layout(set = 0, binding = 1) uniform sampler2D scene;
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 interface_color = texelFetch(shown, at, 0);
    vec3 under = texelFetch(scene, at, 0).rgb;
    if (all(lessThan(abs(interface_color.rgb - under), vec3(1.5 / 255.0)))) discard;
    out_color = interface_color;
}
