// Zones published by mods, and the player's display options.
//
// Mods describe what to show (retained mode): ShroudForge's runtime calls the
// C API in scene.cpp from its own thread, and the present path draws the
// latest snapshot at each frame without waiting for any mod.
#pragma once

#include <imgui.h>

#include "voxel_mesh.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ImTextureData;

namespace overlay {

// A decoded RGBA image, shared by every zone that uses the same file. Its GPU
// texture is created and owned by the present path.
struct Image {
    std::string path;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
    ImTextureData* texture = nullptr;
};

struct TextStyle {
    std::string font;  // Absolute path to a TTF file, empty for the default font.
    float size = 18.0f;
    ImU32 color = IM_COL32_WHITE;
    bool shadow = false;
    ImU32 shadow_color = IM_COL32(0, 0, 0, 200);
    ImVec2 shadow_offset{1.5f, 1.5f};  // Reference pixels.
    float outline = 0.0f;
    ImU32 outline_color = IM_COL32(0, 0, 0, 255);
};

// One stroked subpath of an icon, in viewbox units.
struct Polyline {
    std::vector<ImVec2> points;
    bool closed = false;
};

enum class ItemKind { Text, Image, Row, Spacer, Separator, Progress, Chip, Icon, Toggle, Grow, Flow, Grid, Voxels, World, Drum };
enum class WorldShape { Ring, Cross, Dot };

// A point or a segment in the game world (meters), drawn where the game's
// camera puts it on the screen at every frame.
struct WorldPoint {
    double at[3]{};
    ImU32 color = IM_COL32(255, 255, 255, 255);
    float size = 12.0f;  // Reference pixels.
    WorldShape shape = WorldShape::Ring;
};
struct WorldLine {
    double from[3]{}, to[3]{};
    ImU32 color = IM_COL32(255, 255, 255, 255);
    float width = 2.0f;  // Reference pixels.
};
// A filled flat quadrilateral of the world (four corners in order, meters).
struct WorldQuad {
    double corners[4][3]{};
    ImU32 color = IM_COL32(255, 255, 255, 96);
};
// Filled cells of the world (`solids` of a world item): the world items
// behind them, seen from the camera, are drawn lighter. The corner of the
// first cell and the cell side in meters; 1 in `filled` for a filled cell,
// x fastest, then y, then z.
struct WorldSolids {
    int sx = 0, sy = 0, sz = 0;
    double origin[3]{};
    double cell = 0.5;
    std::vector<std::uint8_t> filled;
    std::vector<std::uint32_t> bits;  // `filled` a bit per cell, for the GPU (the wire view in Remove mode)
    std::shared_ptr<const std::string> data;  // The run-length text they were decoded from.
};
enum class Align { Start, Center, End };

struct Item {
    ItemKind kind = ItemKind::Text;
    Align align = Align::Start;
    // Text
    std::string text;
    TextStyle style;
    // Image, progress: size in reference pixels (0 = automatic).
    std::shared_ptr<Image> image;
    ImVec2 size{0.0f, 0.0f};
    ImU32 tint = IM_COL32_WHITE;
    ImVec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
    float rounding = 0.0f;
    // Row, flow
    std::vector<Item> children;
    // Row, flow: an optional frame around it (`background` filled, this
    // border, `rounding`), its content `frame_padding` inside; in a flow, a
    // row is kept whole on one line.
    ImU32 frame_border = IM_COL32(0, 0, 0, 0);
    ImVec2 frame_padding{0.0f, 0.0f};
    float spacing = 6.0f;
    float line_gap = 2.0f;  // Flow: space between wrapped lines.
    // Flow: lines after the first start this much further right (a hanging
    // indent: a caption first, then items aligned under each other).
    float flow_indent = 0.0f;
    // Separator, progress, toggle track (off)
    ImU32 color = IM_COL32(255, 255, 255, 120);
    ImU32 background = IM_COL32(0, 0, 0, 120);
    float thickness = 1.0f;
    float value = 0.0f;
    // Chip: text on a rounded background; padding is horizontal.
    float padding = 4.0f;
    // Icon: stroked paths in a square viewbox.
    std::vector<Polyline> icon;
    float viewbox = 24.0f;
    float stroke = 1.8f;
    // Drum: a choice of `drum_options` shown as a turning cylinder, the
    // current one (`drum_index`, 0-based) in the middle; `drum_id` keeps its
    // turn from one zone update to the next.
    std::vector<std::string> drum_options;
    int drum_index = 0;
    std::string drum_id;
    ImU32 drum_dim = IM_COL32(216, 211, 198, 140);
    ImU32 drum_border = IM_COL32(201, 162, 78, 140);
    // Toggle
    bool on = false;
    ImU32 on_color = IM_COL32(201, 162, 78, 255);
    ImU32 knob = IM_COL32(233, 230, 223, 255);
    // Grid: rows of ASCII characters, each one cell painted with the color of
    // its character (none: the cell stays empty), and a smaller centred square
    // in the mark color of its character, if any. Sizes in reference pixels.
    std::vector<std::string> cells;
    std::array<ImU32, 128> cell_colors{};
    std::array<ImU32, 128> cell_marks{};
    float cell = 8.0f;
    float gap = 1.0f;
    // Voxels: a box of cells seen in 3D in a view of `size` reference pixels;
    // colors and marks by character as for a grid (cell_colors, cell_marks).
    // The camera turns around the box: yaw (0 looks along +z) plus `spin`
    // degrees per second, pitch looking down, zoom (1 = the box fits).
    // Solid: only the faces seen from the camera, all at `opacity` (the view
    // over the game). X-ray: every face turned to the camera, inner ones
    // included, those of `ghost` characters at `ghost_alpha`, then `opacity`.
    // Its faces are built on the overlay's thread (VoxelSlot).
    std::shared_ptr<VoxelSlot> voxels;
    // World: points and segments in the world, over the whole screen.
    std::vector<WorldPoint> world_points;
    std::vector<WorldLine> world_lines;
    std::vector<WorldQuad> world_quads;
    // World: a box of cells in the world (`cells`), its faces built once
    // (colors by character in cell_colors); the corner of its first cell and
    // the cell side, in meters.
    std::shared_ptr<VoxelSlot> world_cells;
    double world_cells_origin[3]{};
    double world_cells_size = 0.5;
    // Outlined only where its surface turns, not around each cell.
    bool world_cells_smooth = false;
    // Opacity of its edges (0 to 1), in their cell's color.
    float world_cells_edge_opacity = 0.9f;
    std::shared_ptr<const WorldSolids> world_solids;
    // World: the faces of `cells` hidden from the eye (behind `solids`, or
    // behind its own cells with `hide_own`) are not drawn, or only their
    // edges dashed (`hidden_dashed`), instead of drawn lighter.
    bool world_hide_hidden = false;
    bool world_hidden_dashed = false;
    // World: the cells drawn only inside the box of `solids`, and none while
    // the eye is outside it: elsewhere what hides them is not known, and they
    // would show in front of the world.
    bool world_only_in_solids = false;
    // World: asks for the picture of the game's depth (game-depth.on), a diagnostic.
    bool world_depth_picture = false;
    // World: the camera drawing it against the game's depth (game-depth.on):
    // 0 the scene's from the GPU, 1 read with the depth's copy, 2 read at the
    // present; -1 as game-depth.on says.
    int world_depth_camera = -1;
    // World: how far behind the game's surface its cells still show (meters),
    // against the game's depth; negative: as game-depth.on says.
    float world_depth_margin = -1.0f;
    // World: the game's near distance for its depth (meters); 0: the GPU
    // camera matrix's; negative: as game-depth.on says.
    float world_depth_near = -1.0f;
    // World: the GPU camera's matrix kept as the game gives it, with its
    // temporal anti-aliasing jitter (depth_jitter = "keep"); by default
    // taken out, the build steady on the image (30/10: shimmering by a pixel
    // with FSR 3, not without anti-aliasing or with FXAA).
    bool world_depth_jitter_kept = false;
    // World: its cells drawn this much larger (meters) where their surface is
    // open, as the game draws its blocks past their cells; they stay on the
    // grid. Faces pushed out, their edges longer at an outer corner, shorter
    // at an inner one.
    float world_face_offset = 0.0f;
    // World: per cell character, its own face_offset: sides, top (+y),
    // bottom (-y) (the game's blocks: each material its own, KB 30/10).
    std::vector<std::pair<unsigned char, std::array<float, 3>>> world_face_offsets;
    // World: every face drawn with the largest offsets of the characters it
    // holds, direction by direction (face_offsets_largest): a build's faces
    // all as far out, whatever block was there (30/10: steps on a wall of
    // stone built over in wood, the stone's cells farther).
    bool world_face_offsets_largest = false;
    // World, the wire view: the creases between two faces turned away from
    // the eye drawn too, dashed, this opaque (cells.back_edges, 0 to 1; 0:
    // not drawn). 30/10: a crease seen from one side vanished from
    // the other, the build hard to read.
    float world_back_edges = 0.0f;
    // World: a hidden edge's dashes this opaque (cells.dash_opacity, 0 to 1;
    // negative: 220 / 255, nearly opaque, as before 31/10).
    float world_dash_opacity = -1.0f;
    std::shared_ptr<const WorldSolids> world_own_solids;
    // World: the camera foreseen this many seconds ahead (0 to 0.2).
    float camera_lead = 0.0f;
    // World: the face the building cursor aims at, outlined at every frame from
    // the cursor read for that frame (a grid of `aim_cell` meters from
    // `aim_origin`); not drawn while the cursor aims at nothing.
    bool aim_face = false;
    ImU32 aim_face_color = IM_COL32(255, 220, 60, 255);
    float aim_face_width = 2.0f;
    double aim_origin[3]{};
    double aim_cell = 0.5;
    float yaw = 0.0f;
    float pitch = 30.0f;
    float zoom = 1.0f;
    float spin = 0.0f;
    bool xray = false;
    std::string ghost;
    float ghost_alpha = 0.6f;
    float opacity = 1.0f;
    bool plain = false;  // Solid view: no outlines nor smoothed borders, one continuous surface.
    float ease = 0.0f;   // Seconds a new camera angle takes to be reached, 0 = at once.
    // Turning with the game's camera: its yaw read at every frame plus
    // camera_offset degrees, instead of `yaw` (kept when the camera cannot be read).
    bool follow_camera = false;
    float camera_offset = 0.0f;
    float perspective = 0.0f;  // 0 = parallel view, to 1 = a field of 90 degrees.
    // Voxels: most faces of the mesh drawn; a larger box is drawn from a
    // coarser mesh (VoxelMesh::coarser), to keep the game's frame rate.
    int max_faces = kDefaultVoxelFaces;
    ImU32 edge = IM_COL32(0, 0, 0, 110);
    int floor_grid = 0;  // Lines on the floor of the box every this many cells, 0 = none.
    ImU32 grid_color = IM_COL32(255, 255, 255, 40);
};

// Nine-slice frame drawn behind the zone content.
struct Frame {
    std::shared_ptr<Image> image;
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;  // Borders in image pixels.
    float scale = 1.0f;                                           // Border size on screen, in reference pixels per image pixel.
    ImU32 tint = IM_COL32_WHITE;
};

struct Zone {
    std::string owner;
    std::string id;
    // Position: the pivot point of the zone (fractions of its size) sits on
    // the anchor point of the screen (fractions of the screen), moved by
    // offset (reference pixels).
    ImVec2 anchor{0.0f, 0.0f};
    ImVec2 pivot{0.0f, 0.0f};
    ImVec2 offset{0.0f, 0.0f};
    ImVec2 size{0.0f, 0.0f};  // Reference pixels, 0 = fit the content.
    float reference = 1080.0f;  // Screen height the zone's pixel values are designed for.
    int order = 0;
    bool visible = true;
    ImU32 background = IM_COL32(0, 0, 0, 0);
    ImU32 border = IM_COL32(0, 0, 0, 0);
    float border_width = 0.0f;
    float rounding = 0.0f;
    ImVec2 padding{0.0f, 0.0f};
    float spacing = 4.0f;
    Frame frame;
    std::vector<Item> items;
};

// A key with exact modifiers, from a specification such as "Shift+F12".
struct KeyCombo {
    int key = 0;  // Windows virtual-key code, 0 for none.
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

// Player options: modules.gameOverlay in modloader-config.json (F9).
struct Options {
    bool enabled = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    KeyCombo hide_key;  // Shows/hides every zone.
    bool hide_in_menus = true;  // No zone while the game shows the mouse cursor.
};

// player_options.cpp: whether a game menu is open, from the mouse cursor (the
// game hides it while playing and shows it in its menus).
void set_menu_open(bool open);
bool is_menu_open();

// player_options.cpp: parses a key specification with the names mods use
// (modules/mod-runtime/lua/shroudforge/v1/game_keys.rs); none when invalid.
KeyCombo parse_key_combo(const std::string& spec);

// The coordinates mods use are reference pixels of a 1920x1080 screen: they
// are scaled by the screen height.
constexpr float kReferenceHeight = 1080.0f;

struct SceneSnapshot {
    std::vector<std::shared_ptr<const Zone>> zones;  // Sorted by order.
    Options options;
};

// What the present path reads at each frame (cheap: shared pointers).
SceneSnapshot snapshot_scene();
// Whether any zone could be drawn, checked before doing any GPU work.
bool scene_has_visible_zones();

// Current output, reported back to mods by ShroudforgeOverlay_GetStatus.
void set_output(bool active, int width, int height);

// Drops the GPU textures of cached images (their renderer was destroyed).
void forget_textures();

void set_options(const Options& options);
Options current_options();
// Hidden by the player's show/hide key.
void set_hidden(bool hidden);
bool is_hidden();

// player_options.cpp: rereads the player options when the config file changed
// (at most once per second) and handles the show/hide key. Called at each present.
void update_player_options();

// load_image.cpp: decodes a PNG or JPEG file, or returns null with a reason.
std::shared_ptr<Image> load_image(const std::string& path, std::string& error);

// parse_icon_path.cpp: the stroked subpaths of an SVG path (M L H V A Z,
// absolute or relative), or false with a reason.
bool parse_icon_path(const std::string& path, std::vector<Polyline>& out, std::string& error);

}  // namespace overlay
