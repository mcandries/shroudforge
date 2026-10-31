#pragma once

#include "game_depth.h"
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

// World cells drawn against the game's depth (game_depth.cpp): triangles in
// screen pixels with their depth as the game's (near_distance / distance), drawn by
// the renderer under the zones; a fragment behind the game's surface is left
// out, or dashed on an edge with `dash` >= 0 (counted in dash periods).
struct WorldDepthVertex {
    float x, y, depth, dash;
    float extra;  // more margin, in units of 1 / depth (edges: kept visible where they touch the game's surface)
    ImU32 color;
    float cell[3];  // in cells of `blocks` (WorldDepthDraw), times depth
    float fade = 1.0f;  // its opacity, times (the wire view's creases turned away)
    // Textured faces (the solid style): 1 a side across x (u along z), 2 a
    // side across z (u along x), 3 the top, 4 the bottom; 0 none. `cell` is
    // then the point in meters from `texture_origin`, times depth.
    float tex = 0.0f;
};
struct WorldDepthDraw {
    double near_distance = depth::kNearDistance;
    bool solid = false;  // opaque cells (solid style): drawn with a depth of their own, the nearest face kept
    // The wire view (smooth style): our faces' nearest depth drawn first, then
    // an edge hidden by the build itself dashed, hidden by something before
    // the build left out.
    bool wire = false;
    // Wire: how far before our nearest face something hides an edge entirely (meters).
    double own_tolerance = 0.3;
    double focal_pixels = 0.0;  // screen pixels per unit of tan, vertically
    double dash_alpha = 220.0 / 255.0;  // a hidden edge's dashes, this opaque (cells.dash_opacity)
    double back_alpha = -1.0;  // the wire view: those behind the build's own faces (back_edges); negative: dash_alpha
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
    // The solid style's textures (sides, top, bottom), from the first item
    // with them; their meters counted from texture_origin (a multiple of 64 m).
    std::array<WorldTexture, 9> textures;
    float relief = 1.0f;  // how much the normal maps tilt the faces
    bool textured = false;
    float glow = 1.0f;  // the hologram's glow (cells.texture_glow)
    double texture_origin[3]{};
    double texture_eye[3]{};  // the eye in meters from texture_origin (the hologram's view)
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
    // The camera the game drew the scene with (from the GPU, else read with
    // the depth's copy), or false: then the one read at the present.
    std::function<bool(LocalCamera&)> frame_camera;
    // A world item asks for the picture of the game's depth (diagnostic).
    std::function<void()> want_depth_picture;
    // The zones of world items drawn on the list, the others not yet (the
    // game's interface laid back between them, renderer.cpp); may be empty.
    std::function<void(ImDrawList&)> world_drawn;
};

// Lays out and draws every zone of the snapshot on the draw list.
void draw_scene(ImDrawList& list, const SceneSnapshot& scene, ImVec2 screen, const DrawResources& resources);

}  // namespace overlay
