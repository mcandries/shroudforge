// Zones published by mods, and the player's display options.
//
// Mods describe what to show (retained mode): ShroudForge's runtime calls the
// C API in scene.cpp from its own thread, and the present path draws the
// latest snapshot at each frame without waiting for any mod.
#pragma once

#include <imgui.h>

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
    float outline = 0.0f;
    ImU32 outline_color = IM_COL32(0, 0, 0, 255);
};

enum class ItemKind { Text, Image, Row, Spacer, Separator, Progress };
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
    // Row
    std::vector<Item> children;
    float spacing = 6.0f;
    // Separator, progress
    ImU32 color = IM_COL32(255, 255, 255, 120);
    ImU32 background = IM_COL32(0, 0, 0, 120);
    float thickness = 1.0f;
    float value = 0.0f;
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

// Player options: modules.gameOverlay in modloader-config.json (F9).
struct Options {
    bool enabled = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    int hide_key = 0;  // Windows virtual-key code that shows/hides the overlay, 0 for none.
};

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

}  // namespace overlay
