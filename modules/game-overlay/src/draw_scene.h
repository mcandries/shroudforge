#pragma once

#include "scene.h"

#include <cstdint>
#include <functional>
#include <vector>

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

// World cells drawn against the game's depth (game-depth.on): triangles in
// screen pixels with their depth as the game's (near_distance / distance), drawn by
// the renderer under the zones; a fragment behind the game's surface is left
// out, or dashed on an edge with `dash` >= 0 (counted in dash periods).
struct WorldDepthVertex {
    float x, y, depth, dash;
    ImU32 color;
};
struct WorldDepthDraw {
    double near_distance = 0.01;
    std::vector<WorldDepthVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct DrawResources {
    std::function<ImFont*(const std::string& path)> font;          // Loaded font, or the default one.
    std::function<ImTextureRef(Image& image)> texture;             // GPU texture of a decoded image.
    std::function<Offscreen()> offscreen;                          // Off-screen target, or none.
    std::function<WorldDepthDraw*()> world_depth;                  // Null without the game's depth this frame.
    // The game's depth (as read back, a few frames late) at a screen point, or a negative value.
    std::function<float(ImVec2)> game_depth_at;
};

// Lays out and draws every zone of the snapshot on the draw list.
void draw_scene(ImDrawList& list, const SceneSnapshot& scene, ImVec2 screen, const DrawResources& resources);

}  // namespace overlay
