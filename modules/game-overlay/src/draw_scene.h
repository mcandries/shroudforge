#pragma once

#include "scene.h"

#include <functional>

namespace overlay {

// An image as large as the screen, cleared at every frame and drawn before
// the zones: what is drawn on `list` (screen coordinates) can then be laid
// over the game as `texture`, once, with one opacity. `list` is null when the
// renderer has none.
struct Offscreen {
    ImDrawList* list = nullptr;
    ImTextureRef texture;
    ImVec2 size;  // Of the screen, in pixels.
};

struct DrawResources {
    std::function<ImFont*(const std::string& path)> font;          // Loaded font, or the default one.
    std::function<ImTextureRef(Image& image)> texture;             // GPU texture of a decoded image.
    std::function<Offscreen()> offscreen;                          // Off-screen target, or none.
};

// Lays out and draws every zone of the snapshot on the draw list.
void draw_scene(ImDrawList& list, const SceneSnapshot& scene, ImVec2 screen, const DrawResources& resources);

}  // namespace overlay
