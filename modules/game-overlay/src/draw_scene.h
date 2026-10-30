#pragma once

#include "local_camera.h"
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
    float extra;  // more margin, in units of 1 / depth (edges: kept visible where they touch the game's surface)
    ImU32 color;
    float cell[3];  // in cells of `blocks` (WorldDepthDraw), times depth
    float fade = 1.0f;  // its opacity, times (the wire view's creases turned away)
};
struct WorldDepthDraw {
    double near_distance = 0.01;
    double margin = -1.0;  // an item's (depth_margin), else game-depth.on's
    double matrix_near = 0.0;  // the GPU camera matrix's near distance, when known
    bool solid = false;  // opaque cells (solid style): drawn with a depth of their own, the nearest face kept
    // The wire view (smooth style): our faces' nearest depth drawn first, then
    // an edge hidden by the build itself dashed, hidden by something before
    // the build left out.
    bool wire = false;
    // The wire view showing what is hidden (hide_hidden off): an edge hidden
    // by something before the build dashed, not left out.
    bool show_hidden = false;
    // Wire: how far before our nearest face something hides an edge entirely (meters).
    double own_tolerance = 0.3;
    double focal_pixels = 0.0;  // screen pixels per unit of tan, vertically
    double dash_alpha = 220.0 / 255.0;  // a hidden edge's dashes, this opaque (cells.dash_opacity)
    // The wire view in Remove mode (hidden_dashed): an edge hidden by these
    // blocks of the world dashed, by anything else left out; the eye in
    // their cells.
    std::shared_ptr<const WorldSolids> blocks;
    double blocks_eye[3]{};
    double blocks_relief = 0.0;  // how far the game's surface may lie past the blocks (meters)
    // The build's own cells, blocks of the world too (the solids leave out
    // the cells a build changes): each tile's mesh at its place in `blocks`.
    struct BlocksPart {
        std::shared_ptr<const VoxelMesh> mesh;
        int at[3]{};
    };
    std::vector<BlocksPart> blocks_build;
    size_t face_indices = 0;  // the faces' indices come first
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
    // The camera the game drew the scene with, as `mode` asks (an item's
    // world_depth_camera), or false: then the one read at the present.
    std::function<bool(LocalCamera&, int mode)> frame_camera;
    // A world item asks for the picture of the game's depth (diagnostic).
    std::function<void()> want_depth_picture;
};

// Lays out and draws every zone of the snapshot on the draw list.
void draw_scene(ImDrawList& list, const SceneSnapshot& scene, ImVec2 screen, const DrawResources& resources);

}  // namespace overlay
