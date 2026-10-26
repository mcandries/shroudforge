// Zone store and the C API ShroudForge's runtime calls (same process).
//
// The runtime resolves file paths inside the mod's own folder before calling
// in; the JSON format is documented in modules/game-overlay/README.md.

#include "overlay.h"
#include "scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace overlay {

namespace {

using json = nlohmann::json;

constexpr size_t kMaxZonesPerOwner = 32;
constexpr size_t kMaxItemsPerZone = 256;
constexpr size_t kMaxTextLength = 4096;
constexpr size_t kMaxImages = 100;
constexpr int kMaxDepth = 3;
constexpr size_t kMaxGridSide = 64;

std::mutex scene_mutex;
std::map<std::pair<std::string, std::string>, std::shared_ptr<const Zone>> zones;
Options options;
bool hidden = false;
bool menu_open = false;
std::map<std::string, std::shared_ptr<Image>> images;  // Kept for the session: textures are never released mid-game.

std::mutex output_mutex;
bool output_active = false;
int output_width = 0;
int output_height = 0;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

ImU32 parse_color(const json& value, const char* field) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (!text.empty() && text[0] == '#') text.erase(0, 1);
        if ((text.size() == 6 || text.size() == 8) && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            const unsigned long rgba = std::stoul(text, nullptr, 16);
            if (text.size() == 6) return IM_COL32((rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255, 255);
            return IM_COL32((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255);
        }
    }
    if (value.is_array() && (value.size() == 3 || value.size() == 4)) {
        auto channel = [&](size_t i) { return std::clamp(value[i].get<int>(), 0, 255); };
        return IM_COL32(channel(0), channel(1), channel(2), value.size() == 4 ? channel(3) : 255);
    }
    fail(std::string(field) + ": expected a color \"#RRGGBB\", \"#RRGGBBAA\" or {r, g, b, a}");
}

float parse_number(const json& value, const char* field) {
    if (!value.is_number()) fail(std::string(field) + ": expected a number");
    return value.get<float>();
}

// {x, y}, [x, y] or a single number for both.
ImVec2 parse_pair(const json& value, const char* field) {
    if (value.is_number()) return {value.get<float>(), value.get<float>()};
    if (value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number())
        return {value[0].get<float>(), value[1].get<float>()};
    if (value.is_object() && value.contains("x") && value.contains("y"))
        return {parse_number(value["x"], field), parse_number(value["y"], field)};
    fail(std::string(field) + ": expected {x, y}");
}

// A screen or zone point: a name, or {x, y} as fractions.
ImVec2 parse_point(const json& value, const char* field) {
    if (value.is_string()) {
        static const std::map<std::string, ImVec2> names = {
            {"top-left", {0.0f, 0.0f}}, {"top", {0.5f, 0.0f}}, {"top-right", {1.0f, 0.0f}},
            {"left", {0.0f, 0.5f}}, {"center", {0.5f, 0.5f}}, {"right", {1.0f, 0.5f}},
            {"bottom-left", {0.0f, 1.0f}}, {"bottom", {0.5f, 1.0f}}, {"bottom-right", {1.0f, 1.0f}},
        };
        auto found = names.find(value.get<std::string>());
        if (found == names.end()) fail(std::string(field) + ": unknown point " + value.get<std::string>());
        return found->second;
    }
    return parse_pair(value, field);
}

Align parse_align(const json& value) {
    const std::string text = value.is_string() ? value.get<std::string>() : "";
    if (text == "center") return Align::Center;
    if (text == "right" || text == "end" || text == "bottom") return Align::End;
    if (text.empty() || text == "left" || text == "start" || text == "top") return Align::Start;
    fail("align: expected left, center or right");
}

std::shared_ptr<Image> image_for(const json& value, const char* field) {
    if (!value.is_string()) fail(std::string(field) + ": expected an image path");
    const std::string path = value.get<std::string>();
    {
        std::lock_guard lock(scene_mutex);
        auto found = images.find(path);
        if (found != images.end()) return found->second;
        if (images.size() >= kMaxImages) fail("too many different images (100 at most)");
    }
    std::string error;
    auto image = load_image(path, error);
    if (!image) fail(error);
    std::lock_guard lock(scene_mutex);
    return images.emplace(path, image).first->second;
}

void parse_text_style(const json& value, TextStyle& style) {
    if (value.contains("font")) {
        if (!value["font"].is_string()) fail("font: expected a TTF path");
        style.font = value["font"].get<std::string>();
    }
    if (value.contains("size")) style.size = std::clamp(parse_number(value["size"], "size"), 4.0f, 256.0f);
    if (value.contains("color")) style.color = parse_color(value["color"], "color");
    if (value.contains("shadow")) style.shadow = value["shadow"].is_boolean() ? value["shadow"].get<bool>() : true;
    if (value.contains("shadow_color")) style.shadow_color = parse_color(value["shadow_color"], "shadow_color");
    if (value.contains("shadow_offset")) style.shadow_offset = parse_pair(value["shadow_offset"], "shadow_offset");
    if (value.contains("outline")) style.outline = std::clamp(parse_number(value["outline"], "outline"), 0.0f, 8.0f);
    if (value.contains("outline_color")) style.outline_color = parse_color(value["outline_color"], "outline_color");
}

Item parse_item(const json& value, const TextStyle& text_style, int depth, size_t& count) {
    if (++count > kMaxItemsPerZone) fail("too many items in the zone (256 at most)");
    Item item;
    item.style = text_style;
    if (value.is_string()) {
        item.text = value.get<std::string>();
        if (item.text.size() > kMaxTextLength) fail("text longer than 4096 bytes");
        return item;
    }
    if (!value.is_object()) fail("items: expected text or a table");
    const std::string type = value.value("type", value.contains("text") ? "text" : "");
    if (value.contains("align")) item.align = parse_align(value["align"]);
    if (type == "text") {
        item.kind = ItemKind::Text;
        if (!value.contains("text") || !value["text"].is_string()) fail("text item: expected text");
        item.text = value["text"].get<std::string>();
        if (item.text.size() > kMaxTextLength) fail("text longer than 4096 bytes");
        parse_text_style(value, item.style);
    } else if (type == "image") {
        item.kind = ItemKind::Image;
        if (!value.contains("image")) fail("image item: expected image");
        item.image = image_for(value["image"], "image");
        if (value.contains("size")) item.size = parse_pair(value["size"], "size");
        if (value.contains("tint")) item.tint = parse_color(value["tint"], "tint");
        if (value.contains("rounding")) item.rounding = parse_number(value["rounding"], "rounding");
        if (value.contains("uv")) {
            const json& uv = value["uv"];
            if (!uv.is_array() || uv.size() != 4) fail("uv: expected {u0, v0, u1, v1}");
            item.uv = {uv[0].get<float>(), uv[1].get<float>(), uv[2].get<float>(), uv[3].get<float>()};
        }
    } else if (type == "row" || type == "flow") {
        item.kind = type == "row" ? ItemKind::Row : ItemKind::Flow;
        if (depth >= kMaxDepth) fail("rows nested too deeply");
        if (!value.contains("items") || !value["items"].is_array()) fail(type + ": expected items");
        item.spacing = type == "row" ? 6.0f : 5.0f;
        if (value.contains("spacing")) item.spacing = parse_number(value["spacing"], "spacing");
        if (value.contains("line_gap")) item.line_gap = parse_number(value["line_gap"], "line_gap");
        TextStyle row_style = text_style;
        parse_text_style(value, row_style);
        for (const json& child : value["items"]) {
            Item parsed = parse_item(child, row_style, depth + 1, count);
            if (item.kind == ItemKind::Flow && (parsed.kind == ItemKind::Row || parsed.kind == ItemKind::Flow ||
                    parsed.kind == ItemKind::Grow || parsed.kind == ItemKind::Separator))
                fail("flow: only text, chip, icon, image and toggle items");
            item.children.push_back(std::move(parsed));
        }
    } else if (type == "chip") {
        item.kind = ItemKind::Chip;
        if (!value.contains("text") || !value["text"].is_string()) fail("chip: expected text");
        item.text = value["text"].get<std::string>();
        if (item.text.size() > 64) fail("chip text longer than 64 bytes");
        // Dark text on a light background unless the chip says otherwise.
        item.style.color = IM_COL32(22, 23, 27, 255);
        item.style.shadow = false;
        item.style.outline = 0.0f;
        parse_text_style(value, item.style);
        item.background = IM_COL32(242, 239, 231, 255);
        if (value.contains("background")) item.background = parse_color(value["background"], "background");
        item.rounding = 3.0f;
        if (value.contains("rounding")) item.rounding = parse_number(value["rounding"], "rounding");
        if (value.contains("padding")) item.padding = parse_number(value["padding"], "padding");
        // `size` is the text size; `box` the minimum width and the height.
        item.size = {17.0f, 17.0f};
        if (value.contains("box")) item.size = parse_pair(value["box"], "box");
    } else if (type == "icon") {
        item.kind = ItemKind::Icon;
        if (!value.contains("path") || !value["path"].is_string()) fail("icon: expected path (SVG path data)");
        std::string error;
        if (!parse_icon_path(value["path"].get<std::string>(), item.icon, error)) fail(error);
        item.size = {18.0f, 18.0f};
        if (value.contains("size")) item.size = parse_pair(value["size"], "size");
        if (value.contains("viewbox")) item.viewbox = std::max(1.0f, parse_number(value["viewbox"], "viewbox"));
        if (value.contains("stroke")) item.stroke = std::clamp(parse_number(value["stroke"], "stroke"), 0.1f, 16.0f);
        item.color = item.style.color;
        if (value.contains("color")) item.color = parse_color(value["color"], "color");
    } else if (type == "toggle") {
        item.kind = ItemKind::Toggle;
        item.on = value.value("on", false);
        item.size = {26.0f, 14.0f};
        if (value.contains("size")) item.size = parse_pair(value["size"], "size");
        item.color = IM_COL32(255, 255, 255, 71);
        if (value.contains("color")) item.color = parse_color(value["color"], "color");
        if (value.contains("on_color")) item.on_color = parse_color(value["on_color"], "on_color");
        if (value.contains("knob")) item.knob = parse_color(value["knob"], "knob");
    } else if (type == "grid") {
        item.kind = ItemKind::Grid;
        if (!value.contains("rows") || !value["rows"].is_array()) fail("grid: expected rows (strings)");
        if (value["rows"].size() > kMaxGridSide) fail("grid: 64 rows at most");
        for (const json& row : value["rows"]) {
            if (!row.is_string()) fail("grid: each row is a string");
            std::string text = row.get<std::string>();
            if (text.size() > kMaxGridSide) fail("grid: 64 cells per row at most");
            for (const char ch : text)
                if (static_cast<unsigned char>(ch) >= 128) fail("grid: rows use ASCII characters");
            item.cells.push_back(std::move(text));
        }
        if (!value.contains("colors") || !value["colors"].is_object()) fail("grid: expected colors (character -> color)");
        for (const auto& [key, color] : value["colors"].items()) {
            if (key.size() != 1 || static_cast<unsigned char>(key[0]) >= 128)
                fail("grid: color keys are single ASCII characters");
            item.cell_colors[static_cast<unsigned char>(key[0])] = parse_color(color, "colors");
        }
        if (value.contains("marks")) {
            if (!value["marks"].is_object()) fail("grid: marks: expected character -> color");
            for (const auto& [key, color] : value["marks"].items()) {
                if (key.size() != 1 || static_cast<unsigned char>(key[0]) >= 128)
                    fail("grid: mark keys are single ASCII characters");
                item.cell_marks[static_cast<unsigned char>(key[0])] = parse_color(color, "marks");
            }
        }
        if (value.contains("cell")) item.cell = std::clamp(parse_number(value["cell"], "cell"), 1.0f, 64.0f);
        if (value.contains("gap")) item.gap = std::clamp(parse_number(value["gap"], "gap"), 0.0f, 16.0f);
        if (value.contains("rounding")) item.rounding = parse_number(value["rounding"], "rounding");
    } else if (type == "voxels") {
        item.kind = ItemKind::Voxels;
        if (!value.contains("box") || !value["box"].is_array() || value["box"].size() != 3)
            fail("voxels: expected box = {x, y, z} cells");
        int box[3];
        for (int i = 0; i < 3; ++i) {
            if (!value["box"][i].is_number_integer()) fail("voxels: box sides are integers");
            box[i] = value["box"][i].get<int>();
        }
        if (!value.contains("cells") || !value["cells"].is_string()) fail("voxels: expected cells (text)");
        if (!value.contains("colors") || !value["colors"].is_object()) fail("voxels: expected colors (character -> color)");
        for (const auto& [key, color] : value["colors"].items()) {
            if (key.size() != 1 || static_cast<unsigned char>(key[0]) >= 128 || (key[0] >= '0' && key[0] <= '9'))
                fail("voxels: color keys are single ASCII characters, not digits");
            item.cell_colors[static_cast<unsigned char>(key[0])] = parse_color(color, "colors");
        }
        if (value.contains("marks")) {
            if (!value["marks"].is_object()) fail("voxels: marks: expected character -> color");
            for (const auto& [key, color] : value["marks"].items()) {
                if (key.size() != 1 || static_cast<unsigned char>(key[0]) >= 128)
                    fail("voxels: mark keys are single ASCII characters");
                item.cell_marks[static_cast<unsigned char>(key[0])] = parse_color(color, "marks");
            }
        }
        try {
            item.voxels = std::make_shared<const VoxelMesh>(build_voxel_mesh(box[0], box[1], box[2],
                value["cells"].get<std::string>(), item.cell_colors, item.cell_marks));
        } catch (const std::runtime_error& error) {
            fail(error.what());
        }
        item.size = {240.0f, 180.0f};
        if (value.contains("size")) item.size = parse_pair(value["size"], "size");
        if (value.contains("yaw")) item.yaw = parse_number(value["yaw"], "yaw");
        if (value.contains("pitch")) item.pitch = std::clamp(parse_number(value["pitch"], "pitch"), -89.0f, 89.0f);
        if (value.contains("zoom")) item.zoom = std::clamp(parse_number(value["zoom"], "zoom"), 0.1f, 16.0f);
        if (value.contains("spin")) item.spin = std::clamp(parse_number(value["spin"], "spin"), -360.0f, 360.0f);
        item.xray = value.value("xray", false);
        item.plain = value.value("plain", false);
        if (value.contains("ease")) item.ease = std::clamp(parse_number(value["ease"], "ease"), 0.0f, 5.0f);
        item.follow_camera = value.value("follow_camera", false);
        if (value.contains("camera_offset")) item.camera_offset = parse_number(value["camera_offset"], "camera_offset");
        if (value.contains("perspective"))
            item.perspective = std::clamp(parse_number(value["perspective"], "perspective"), 0.0f, 1.0f);
        if (value.contains("ghost")) {
            if (!value["ghost"].is_string()) fail("voxels: ghost: expected characters");
            item.ghost = value["ghost"].get<std::string>();
        }
        if (value.contains("ghost_alpha"))
            item.ghost_alpha = std::clamp(parse_number(value["ghost_alpha"], "ghost_alpha"), 0.0f, 1.0f);
        if (value.contains("opacity")) item.opacity = std::clamp(parse_number(value["opacity"], "opacity"), 0.0f, 1.0f);
        if (value.contains("edge")) item.edge = parse_color(value["edge"], "edge");
        if (value.contains("grid"))
            item.floor_grid = std::clamp(static_cast<int>(parse_number(value["grid"], "grid")), 0, kMaxVoxelSide);
        if (value.contains("grid_color")) item.grid_color = parse_color(value["grid_color"], "grid_color");
    } else if (type == "world") {
        item.kind = ItemKind::World;
        const auto position = [&](const json& at, const char* field) {
            if (!at.is_array() || at.size() != 3 || !at[0].is_number() || !at[1].is_number() || !at[2].is_number())
                fail(std::string("world: ") + field + ": expected {x, y, z} in meters");
            return std::array<double, 3>{at[0].get<double>(), at[1].get<double>(), at[2].get<double>()};
        };
        if (value.contains("points")) {
            if (!value["points"].is_array() || value["points"].size() > 256) fail("world: points: at most 256");
            for (const auto& entry : value["points"]) {
                if (!entry.is_object() || !entry.contains("at")) fail("world: a point needs at = {x, y, z}");
                WorldPoint point;
                const auto at = position(entry["at"], "at");
                for (int i = 0; i < 3; ++i) point.at[i] = at[i];
                if (entry.contains("color")) point.color = parse_color(entry["color"], "color");
                if (entry.contains("size")) point.size = std::clamp(parse_number(entry["size"], "size"), 1.0f, 128.0f);
                const std::string shape = entry.value("shape", std::string("ring"));
                if (shape == "ring") point.shape = WorldShape::Ring;
                else if (shape == "cross") point.shape = WorldShape::Cross;
                else if (shape == "dot") point.shape = WorldShape::Dot;
                else fail("world: shape is ring, cross or dot");
                item.world_points.push_back(point);
            }
        }
        if (value.contains("camera_lead"))
            item.camera_lead = std::clamp(parse_number(value["camera_lead"], "camera_lead"), 0.0f, 0.2f);
        if (value.contains("aim_face")) {
            const auto& face = value["aim_face"];
            if (!face.is_object()) fail("world: aim_face: expected {color, width, origin, cell}");
            item.aim_face = true;
            if (face.contains("color")) item.aim_face_color = parse_color(face["color"], "aim_face.color");
            if (face.contains("width"))
                item.aim_face_width = std::clamp(parse_number(face["width"], "aim_face.width"), 0.5f, 32.0f);
            if (face.contains("origin")) {
                const auto origin = position(face["origin"], "aim_face.origin");
                for (int i = 0; i < 3; ++i) item.aim_origin[i] = origin[i];
            }
            if (face.contains("cell")) item.aim_cell = std::clamp(parse_number(face["cell"], "aim_face.cell"), 0.05f, 16.0f);
        }
        if (value.contains("lines")) {
            if (!value["lines"].is_array() || value["lines"].size() > 256) fail("world: lines: at most 256");
            for (const auto& entry : value["lines"]) {
                if (!entry.is_object() || !entry.contains("from") || !entry.contains("to"))
                    fail("world: a line needs from and to = {x, y, z}");
                WorldLine segment;
                const auto from = position(entry["from"], "from");
                const auto to = position(entry["to"], "to");
                for (int i = 0; i < 3; ++i) {
                    segment.from[i] = from[i];
                    segment.to[i] = to[i];
                }
                if (entry.contains("color")) segment.color = parse_color(entry["color"], "color");
                if (entry.contains("width")) segment.width = std::clamp(parse_number(entry["width"], "width"), 0.5f, 32.0f);
                item.world_lines.push_back(segment);
            }
        }
    } else if (type == "grow") {
        item.kind = ItemKind::Grow;
    } else if (type == "spacer") {
        item.kind = ItemKind::Spacer;
        item.size.y = value.contains("size") ? parse_number(value["size"], "size") : 8.0f;
    } else if (type == "separator") {
        item.kind = ItemKind::Separator;
        if (value.contains("color")) item.color = parse_color(value["color"], "color");
        if (value.contains("thickness")) item.thickness = parse_number(value["thickness"], "thickness");
    } else if (type == "progress") {
        item.kind = ItemKind::Progress;
        item.value = value.contains("value") ? std::clamp(parse_number(value["value"], "value"), 0.0f, 1.0f) : 0.0f;
        item.size = {0.0f, 8.0f};
        if (value.contains("size")) item.size = parse_pair(value["size"], "size");
        item.color = IM_COL32(226, 176, 64, 255);
        if (value.contains("color")) item.color = parse_color(value["color"], "color");
        if (value.contains("background")) item.background = parse_color(value["background"], "background");
        if (value.contains("rounding")) item.rounding = parse_number(value["rounding"], "rounding");
    } else {
        fail("unknown item type " + type);
    }
    return item;
}

std::shared_ptr<Zone> parse_zone(const std::string& owner, const std::string& id, const json& value) {
    if (!value.is_object()) fail("zone: expected a table");
    auto zone = std::make_shared<Zone>();
    zone->owner = owner;
    zone->id = id;
    if (value.contains("anchor")) zone->anchor = parse_point(value["anchor"], "anchor");
    zone->pivot = value.contains("pivot") ? parse_point(value["pivot"], "pivot") : zone->anchor;
    if (value.contains("offset")) zone->offset = parse_pair(value["offset"], "offset");
    if (value.contains("size")) zone->size = parse_pair(value["size"], "size");
    if (value.contains("reference"))
        zone->reference = std::clamp(parse_number(value["reference"], "reference"), 240.0f, 4320.0f);
    if (value.contains("order")) zone->order = static_cast<int>(parse_number(value["order"], "order"));
    if (value.contains("visible")) zone->visible = value["visible"].is_boolean() && value["visible"].get<bool>();
    if (value.contains("background")) zone->background = parse_color(value["background"], "background");
    if (value.contains("border")) {
        zone->border = parse_color(value["border"], "border");
        zone->border_width = 1.0f;
    }
    if (value.contains("border_width")) zone->border_width = parse_number(value["border_width"], "border_width");
    if (value.contains("rounding")) zone->rounding = parse_number(value["rounding"], "rounding");
    if (value.contains("padding")) zone->padding = parse_pair(value["padding"], "padding");
    if (value.contains("spacing")) zone->spacing = parse_number(value["spacing"], "spacing");
    if (value.contains("frame")) {
        const json& frame = value["frame"];
        if (!frame.is_object() || !frame.contains("image")) fail("frame: expected {image = ..., slice = ...}");
        zone->frame.image = image_for(frame["image"], "frame.image");
        if (frame.contains("slice")) {
            const json& slice = frame["slice"];
            if (slice.is_number()) {
                zone->frame.left = zone->frame.top = zone->frame.right = zone->frame.bottom = slice.get<float>();
            } else if (slice.is_array() && slice.size() == 4) {
                zone->frame.left = slice[0].get<float>();
                zone->frame.top = slice[1].get<float>();
                zone->frame.right = slice[2].get<float>();
                zone->frame.bottom = slice[3].get<float>();
            } else {
                fail("frame.slice: expected a number or {left, top, right, bottom}");
            }
        }
        if (frame.contains("scale")) zone->frame.scale = parse_number(frame["scale"], "frame.scale");
        if (frame.contains("tint")) zone->frame.tint = parse_color(frame["tint"], "frame.tint");
    }
    TextStyle text_style;
    if (value.contains("text")) {
        if (!value["text"].is_object()) fail("text: expected the zone's default text style");
        parse_text_style(value["text"], text_style);
    }
    if (value.contains("items")) {
        if (!value["items"].is_array()) fail("items: expected a list");
        size_t count = 0;
        for (const json& item : value["items"]) zone->items.push_back(parse_item(item, text_style, 1, count));
    }
    return zone;
}

void write_error(const std::string& message, char* error, uint32_t error_size) {
    if (!error || error_size == 0) return;
    strncpy_s(error, error_size, message.c_str(), _TRUNCATE);
}

}  // namespace

SceneSnapshot snapshot_scene() {
    SceneSnapshot snapshot;
    std::lock_guard lock(scene_mutex);
    snapshot.options = options;
    for (auto& [key, zone] : zones) {
        if (zone->visible) snapshot.zones.push_back(zone);
    }
    std::stable_sort(snapshot.zones.begin(), snapshot.zones.end(),
        [](const auto& a, const auto& b) { return a->order < b->order; });
    return snapshot;
}

bool scene_has_visible_zones() {
    std::lock_guard lock(scene_mutex);
    if (!options.enabled || hidden || options.opacity <= 0.0f) return false;
    if (options.hide_in_menus && menu_open) return false;
    for (auto& [key, zone] : zones) {
        if (zone->visible) return true;
    }
    return false;
}

void set_options(const Options& value) {
    std::lock_guard lock(scene_mutex);
    options = value;
}

Options current_options() {
    std::lock_guard lock(scene_mutex);
    return options;
}

void set_menu_open(bool value) {
    std::lock_guard lock(scene_mutex);
    menu_open = value;
}

bool is_menu_open() {
    std::lock_guard lock(scene_mutex);
    return menu_open;
}

void set_hidden(bool value) {
    std::lock_guard lock(scene_mutex);
    hidden = value;
}

bool is_hidden() {
    std::lock_guard lock(scene_mutex);
    return hidden;
}

void forget_textures() {
    std::lock_guard lock(scene_mutex);
    for (auto& [path, image] : images) image->texture = nullptr;
}

void set_output(bool active, int width, int height) {
    std::lock_guard lock(output_mutex);
    output_active = active;
    output_width = width;
    output_height = height;
}

}  // namespace overlay

using namespace overlay;

// Version of this C API; the runtime refuses to use a layer it does not know.
OVERLAY_EXPORT int ShroudforgeOverlay_ApiVersion() {
    return 1;
}

// Creates or replaces the zone `id` of `owner` (a mod id). Returns 0, or -1
// with a message in `error`.
OVERLAY_EXPORT int ShroudforgeOverlay_SetZone(const char* owner, const char* id, const char* zone_json,
        char* error, uint32_t error_size) {
    try {
        if (!owner || !*owner || !id || !*id || !zone_json) fail("owner, id and zone are required");
        auto zone = parse_zone(owner, id, json::parse(zone_json));
        std::lock_guard lock(scene_mutex);
        const auto key = std::make_pair(std::string(owner), std::string(id));
        if (!zones.count(key)) {
            const auto owned = std::count_if(zones.begin(), zones.end(), [&](auto& entry) { return entry.first.first == owner; });
            if (static_cast<size_t>(owned) >= kMaxZonesPerOwner) fail("too many zones for this mod (32 at most)");
        }
        zones[key] = std::move(zone);
        return 0;
    } catch (const std::exception& exception) {
        write_error(exception.what(), error, error_size);
        return -1;
    }
}

OVERLAY_EXPORT void ShroudforgeOverlay_RemoveZone(const char* owner, const char* id) {
    if (!owner || !id) return;
    std::lock_guard lock(scene_mutex);
    zones.erase(std::make_pair(std::string(owner), std::string(id)));
}

// Removes every zone of a mod (disabled, reloaded or stopped).
OVERLAY_EXPORT void ShroudforgeOverlay_ClearOwner(const char* owner) {
    if (!owner) return;
    std::lock_guard lock(scene_mutex);
    std::erase_if(zones, [&](auto& entry) { return entry.first.first == owner; });
}

// Writes {"active": bool, "width": n, "height": n, "scale": n} into `out`.
OVERLAY_EXPORT int ShroudforgeOverlay_GetStatus(char* out, uint32_t out_size) {
    json status;
    {
        std::lock_guard lock(output_mutex);
        status = {{"active", output_active}, {"width", output_width}, {"height", output_height}};
    }
    {
        std::lock_guard lock(scene_mutex);
        status["scale"] = (status["height"].get<int>() / kReferenceHeight) * options.scale;
        status["menu"] = menu_open;
    }
    const std::string text = status.dump();
    if (!out || out_size <= text.size()) return -1;
    memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}
