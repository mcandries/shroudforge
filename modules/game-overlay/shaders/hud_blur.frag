#version 450
// The HUD's blurred scene (renderer.cpp, fill_hud_blur): a Gaussian blur
// along one direction, run twice (across, then down) at the size of the
// game's HUD blur. Across, the image shown made a quarter of its size is read
// between its texels (a 2 x 2 mean, down to an eighth); down, the first pass.
// 31/10 capture: sigma 1.25 texels of the eighth matches the game's blur
// (mean squared error 0.00005, against 0.005 without a blur).

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Blur {
    vec2 texel;      // one texel of the target, in uv
    vec2 direction;  // (1, 0) across, (0, 1) down
    float lod;       // the source's level read
    float sigma;     // in the target's texels
} c;
layout(location = 0) out vec4 out_color;

const int kRadius = 4;

void main() {
    vec2 uv = gl_FragCoord.xy * c.texel;
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int i = -kRadius; i <= kRadius; ++i) {
        float weight = exp(-float(i * i) / (2.0 * c.sigma * c.sigma));
        sum += weight * textureLod(source, uv + c.direction * c.texel * float(i), c.lod);
        total += weight;
    }
    out_color = sum / total;
}
