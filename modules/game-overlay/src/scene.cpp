// Zone store and the C API ShroudForge's runtime calls (same process).
//
// The runtime resolves file paths inside the mod's own folder before calling
// in; the JSON format is documented in modules/game-overlay/README.md.

#include "overlay.h"
#include "scene.h"
#include "game_depth.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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
// Lines of an item once its dashed lines and boxes are cut into segments.
constexpr size_t kMaxWorldLines = 16384;
constexpr int kMaxDashes = 256;

std::mutex scene_mutex;
std::map<std::pair<std::string, std::string>, std::shared_ptr<const Zone>> zones;
Options options;
bool hidden = false;
bool menu_open = false;
std::map<std::string, std::shared_ptr<Image>> images;  // Kept for the session: textures are never released mid-game.

// Long texts a mod gave once (ShroudforgeOverlay_SetBlob), named in its zones
// by {"blob": id} instead of the text: a 3D view of a few megabytes is not
// copied through JSON at every change of its zone.
std::mutex blobs_mutex;
std::map<std::pair<std::string, std::uint64_t>, std::shared_ptr<const std::string>> blobs;
// The mod whose zone is being parsed, for its blobs.
thread_local const std::string* parsing_owner = nullptr;

std::mutex output_mutex;
bool output_active = false;
int output_width = 0;
int output_height = 0;
// The game's frame time, measured between its successive presents
// (count_frame) and guarded by output_mutex: the frames of the current
// window, and the average of the last complete one.
using FrameClock = std::chrono::steady_clock;
constexpr auto kFrameWindow = std::chrono::seconds(1);
FrameClock::time_point frame_window_start{};
FrameClock::time_point last_frame{};
int frames_in_window = 0;
double average_frame_ms = 0.0;

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
// `line` as `dashes` segments of equal length with gaps as long between
// them, the first at `from`, the last ending at `to` (1: the whole line).
void add_dashed(std::vector<WorldLine>& lines, const WorldLine& line, int dashes) {
    if (lines.size() + static_cast<size_t>(dashes) > kMaxWorldLines)
        fail("lines: at most " + std::to_string(kMaxWorldLines) + " segments with the dashes and boxes");
    const double step = 1.0 / (2 * dashes - 1);
    for (int k = 0; k < dashes; ++k) {
        WorldLine dash = line;
        for (int i = 0; i < 3; ++i) {
            dash.from[i] = line.from[i] + (line.to[i] - line.from[i]) * (2 * k) * step;
            dash.to[i] = line.from[i] + (line.to[i] - line.from[i]) * (2 * k + 1) * step;
        }
        lines.push_back(dash);
    }
}

int parse_dashes(const json& entry) {
    if (!entry.contains("dashes")) return 1;
    if (!entry["dashes"].is_number_integer()) fail("dashes: expected a whole number");
    return std::clamp(entry["dashes"].get<int>(), 1, kMaxDashes);
}

// The 12 edges of the box between the corners `from` and `to`, each cut into
// `dashes`.
void add_box_edges(std::vector<WorldLine>& lines, const WorldLine& box, int dashes) {
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        for (int side = 0; side < 4; ++side) {
            WorldLine edge = box;
            const bool high_u = side == 1 || side == 2, high_v = side >= 2;
            edge.from[u] = edge.to[u] = high_u ? box.to[u] : box.from[u];
            edge.from[v] = edge.to[v] = high_v ? box.to[v] : box.from[v];
            edge.from[axis] = box.from[axis];
            edge.to[axis] = box.to[axis];
            add_dashed(lines, edge, dashes);
        }
    }
}

// The text of a `cells` or `data` field, shared by the slot and its job: a
// string, or {"blob": id}, a text the mod gave before.
std::shared_ptr<const std::string> cells_text(const json& value) {
    if (value.is_string()) return std::make_shared<const std::string>(value.get_ref<const std::string&>());
    if (!value.is_object() || !value.contains("blob") || !value["blob"].is_number_unsigned() || !parsing_owner)
        fail("cells: expected a text or {blob = id}");
    const auto id = value["blob"].get<std::uint64_t>();
    std::lock_guard lock(blobs_mutex);
    const auto found = blobs.find({*parsing_owner, id});
    if (found == blobs.end()) fail("unknown blob " + std::to_string(id));
    return found->second;
}

bool is_text(const json& value) {
    return value.is_string() || (value.is_object() && value.contains("blob"));
}

}  // namespace

size_t bc7_bytes(int width, int height, int levels, bool half) {
    size_t bytes = 0;
    for (int level = 0; level < levels; ++level) {
        const int w = std::max(1, width >> level), h = std::max(1, height >> level);
        bytes += static_cast<size_t>((w + 3) / 4) * static_cast<size_t>((h + 3) / 4) * (half ? 8 : 16);
    }
    return bytes;
}

namespace {

// One texture: {data = its levels (a blob), width, height, levels,
// format = "bc7" (default), "bc1", "bc4" or "bc5", tiling}.
void parse_one_texture(const json& face, WorldTexture& out, const std::string& name) {
    if (!face.contains("data") || !is_text(face["data"]) || !face.contains("width") ||
        !face["width"].is_number_integer() || !face.contains("height") || !face["height"].is_number_integer() ||
        !face.contains("levels") || !face["levels"].is_number_integer())
        fail("world: cells.texture." + name + ": {data, width, height, levels, format, tiling}");
    out.data = cells_text(face["data"]);
    out.width = std::clamp(face["width"].get<int>(), 1, 4096);
    out.height = std::clamp(face["height"].get<int>(), 1, 4096);
    out.levels = std::clamp(face["levels"].get<int>(), 1, 13);
    if (face.contains("format") && face["format"].is_string()) {
        const std::string format = face["format"].get<std::string>();
        if (format == "bc7") out.format = TextureFormat::bc7;
        else if (format == "bc1") out.format = TextureFormat::bc1;
        else if (format == "bc4") out.format = TextureFormat::bc4;
        else if (format == "bc5") out.format = TextureFormat::bc5;
        else fail("world: cells.texture." + name + ".format: bc7, bc1, bc4 or bc5");
    }
    if (face.contains("tiling") && face["tiling"].is_number())
        out.tiling = std::clamp(face["tiling"].get<float>(), 0.001f, 64.0f);
    if (out.data->size() < bc7_bytes(out.width, out.height, out.levels, half_blocks(out.format)))
        fail("world: cells.texture." + name + ": fewer bytes than its levels");
}

// cells.texture = {side, top, bottom}, each a texture (parse_one_texture)
// and optionally `ao`, its ambient occlusion (a texture too, its first
// channel read; laid as the color), and `normal`, its normal map (x and y in
// its first two channels): the faces of the solid style textured.
void parse_world_texture(Item& item, const json& texture) {
    static const char* kFaces[3] = {"side", "top", "bottom"};
    for (int i = 0; i < 3; ++i) {
        if (!texture.contains(kFaces[i]) || !texture[kFaces[i]].is_object())
            fail(std::string("world: cells.texture needs side, top and bottom; no ") + kFaces[i]);
        const json& face = texture[kFaces[i]];
        parse_one_texture(face, item.world_textures[i], kFaces[i]);
        if (face.contains("ao") && face["ao"].is_object())
            parse_one_texture(face["ao"], item.world_textures[3 + i], std::string(kFaces[i]) + ".ao");
        if (face.contains("normal") && face["normal"].is_object())
            parse_one_texture(face["normal"], item.world_textures[6 + i], std::string(kFaces[i]) + ".normal");
    }
    item.world_textured = true;
}

// cells.texture_keep = {data...}: textures held though not drawn.
void parse_texture_keep(Item& item, const json& list) {
    for (const json& data : list)
        if (is_text(data)) item.world_texture_keep.push_back(cells_text(data));
}

// A slot to build from these cells once the zone is kept (start_meshes).
std::shared_ptr<VoxelSlot> slot_for(int sx, int sy, int sz, std::shared_ptr<const std::string> cells,
    const std::array<ImU32, 128>& colors, const std::array<ImU32, 128>& marks, bool levels, bool one_shape = false) {
    auto source = std::make_shared<VoxelSource>();
    source->sx = sx;
    source->sy = sy;
    source->sz = sz;
    source->cells = std::move(cells);
    source->colors = colors;
    source->marks = marks;
    source->levels = levels;
    source->one_shape = one_shape;
    auto slot = std::make_shared<VoxelSlot>();
    slot->source = std::move(source);
    return slot;
}

// Solids decoded lately: the items of a zone often share them (a world item
// per tile of a build, each with the filled cells around it), and a zone set
// again keeps them while its build does not move (decoded on the mods loop).
std::mutex solids_mutex;
std::vector<std::weak_ptr<const WorldSolids>> recent_solids;

std::shared_ptr<const WorldSolids> decoded_solids(const int box[3], const std::array<double, 3>& origin, double cell,
    std::shared_ptr<const std::string> data) {
    std::lock_guard lock(solids_mutex);
    std::erase_if(recent_solids, [](const auto& entry) { return entry.expired(); });
    for (const auto& entry : recent_solids) {
        const auto known = entry.lock();
        if (known && known->sx == box[0] && known->sy == box[1] && known->sz == box[2] && known->cell == cell &&
            known->origin[0] == origin[0] && known->origin[1] == origin[1] && known->origin[2] == origin[2] &&
            (known->data == data || *known->data == *data))
            return known;
    }
    auto shape = std::make_shared<WorldSolids>();
    shape->sx = box[0];
    shape->sy = box[1];
    shape->sz = box[2];
    for (int i = 0; i < 3; ++i) shape->origin[i] = origin[i];
    shape->cell = cell;
    const std::vector<unsigned char> grid = decode_voxel_cells(box[0], box[1], box[2], *data);
    shape->filled.resize(grid.size());
    shape->bits.assign((grid.size() + 31) / 32, 0u);
    for (size_t i = 0; i < grid.size(); ++i) {
        shape->filled[i] = grid[i] == '#' ? 1 : 0;
        if (shape->filled[i]) shape->bits[i >> 5] |= 1u << (i & 31);
    }
    shape->data = std::move(data);
    if (recent_solids.size() >= 16) recent_solids.erase(recent_solids.begin());
    recent_solids.push_back(shape);
    return shape;
}

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
        // A frame, for a row or a flow.
        item.background = IM_COL32(0, 0, 0, 0);
        if (value.contains("background")) item.background = parse_color(value["background"], "background");
        if (value.contains("border")) item.frame_border = parse_color(value["border"], "border");
        if (value.contains("padding")) item.frame_padding = parse_pair(value["padding"], "padding");
        if (value.contains("rounding")) item.rounding = parse_number(value["rounding"], "rounding");
        if (type == "flow" && value.contains("indent"))
            item.flow_indent = std::max(0.0f, parse_number(value["indent"], "indent"));
        if (value.contains("line_gap")) item.line_gap = parse_number(value["line_gap"], "line_gap");
        TextStyle row_style = text_style;
        parse_text_style(value, row_style);
        for (const json& child : value["items"]) {
            Item parsed = parse_item(child, row_style, depth + 1, count);
            if (item.kind == ItemKind::Flow && (parsed.kind == ItemKind::Flow || parsed.kind == ItemKind::Grow ||
                    parsed.kind == ItemKind::Separator))
                fail("flow: only text, chip, icon, image, toggle and row items");
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
    } else if (type == "drum") {
        item.kind = ItemKind::Drum;
        if (!value.contains("options") || !value["options"].is_array() || value["options"].empty() ||
            value["options"].size() > 8)
            fail("drum: options: 1 to 8 texts");
        for (const json& option : value["options"]) {
            if (!option.is_string() || option.get<std::string>().size() > 64) fail("drum: options are texts of 64 bytes at most");
            item.drum_options.push_back(option.get<std::string>());
        }
        const int count = static_cast<int>(item.drum_options.size());
        if (value.contains("index"))
            item.drum_index = std::clamp(static_cast<int>(parse_number(value["index"], "index")), 1, count) - 1;
        if (value.contains("id")) {
            if (!value["id"].is_string()) fail("drum: id: expected text");
            item.drum_id = value["id"].get<std::string>();
        }
        item.style.color = IM_COL32(240, 234, 216, 255);
        item.style.shadow = false;
        item.style.outline = 0.0f;
        parse_text_style(value, item.style);
        item.background = IM_COL32(12, 11, 9, 230);
        if (value.contains("background")) item.background = parse_color(value["background"], "background");
        if (value.contains("dim")) item.drum_dim = parse_color(value["dim"], "dim");
        if (value.contains("border")) item.drum_border = parse_color(value["border"], "border");
        // `size` is the text size; `box` the drum's width and height.
        item.size = {96.0f, 26.0f};
        if (value.contains("box")) item.size = parse_pair(value["box"], "box");
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
        if (!value.contains("cells") || !is_text(value["cells"])) fail("voxels: expected cells (text)");
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
        auto text = cells_text(value["cells"]);
        try {
            check_voxel_cells(box[0], box[1], box[2], *text);
        } catch (const std::runtime_error& error) {
            fail(error.what());
        }
        item.voxels = slot_for(box[0], box[1], box[2], std::move(text), item.cell_colors, item.cell_marks, true);
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
        if (value.contains("max_faces"))
            item.max_faces = static_cast<int>(
                std::clamp(parse_number(value["max_faces"], "max_faces"), 1000.0f, static_cast<float>(kMaxVoxelFaces)));
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
        if (value.contains("lines")) {
            if (!value["lines"].is_array() || value["lines"].size() > 256) fail("voxels: lines: at most 256");
            for (const auto& entry : value["lines"]) {
                if (!entry.is_object() || !entry.contains("from") || !entry.contains("to"))
                    fail("voxels: a line needs from and to = {x, y, z} in cells");
                WorldLine segment;
                for (int end = 0; end < 2; ++end) {
                    const json& at = entry[end == 0 ? "from" : "to"];
                    if (!at.is_array() || at.size() != 3 || !at[0].is_number() || !at[1].is_number() || !at[2].is_number())
                        fail("voxels: lines: expected {x, y, z} in cells");
                    for (int i = 0; i < 3; ++i) (end == 0 ? segment.from : segment.to)[i] = at[i].get<double>();
                }
                if (entry.contains("color")) segment.color = parse_color(entry["color"], "color");
                if (entry.contains("width"))
                    segment.width = std::clamp(parse_number(entry["width"], "width"), 0.5f, 16.0f);
                add_dashed(item.world_lines, segment, parse_dashes(entry));
            }
        }
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
            item.camera_lead = std::clamp(parse_number(value["camera_lead"], "camera_lead"), -0.2f, 0.2f);
        item.world_depth_picture = value.value("depth_picture", false);
        if (value.contains("depth_near")) {
            if (value["depth_near"].is_number()) item.world_depth_near = std::clamp(value["depth_near"].get<float>(), 0.0001f, 1.0f);
            else if (value["depth_near"] == "matrix") item.world_depth_near = 0.0f;
        }
        if (value.contains("face_offset") && value["face_offset"].is_number())
            item.world_face_offset = std::clamp(value["face_offset"].get<float>(), 0.0f, 0.25f);
        if (value.contains("face_offsets") && value["face_offsets"].is_object())
            for (const auto& [key, offset] : value["face_offsets"].items()) {
                if (key.size() != 1) continue;
                // One number for all, or {sides, top, bottom}.
                std::array<float, 3> each{};
                if (offset.is_number()) {
                    each.fill(std::clamp(offset.get<float>(), 0.0f, 1.0f));
                } else if (offset.is_array() && offset.size() == 3 && offset[0].is_number() && offset[1].is_number() &&
                           offset[2].is_number()) {
                    for (int i = 0; i < 3; ++i) each[i] = std::clamp(offset[i].get<float>(), 0.0f, 1.0f);
                } else {
                    continue;
                }
                item.world_face_offsets.push_back({static_cast<unsigned char>(key[0]), each});
            }
        if (value.contains("depth_margin") && value["depth_margin"].is_number())
            item.world_depth_margin = std::clamp(value["depth_margin"].get<float>(), 0.0f, 2.0f);
        if (value.contains("depth_jitter") && value["depth_jitter"].is_string())
            item.world_depth_jitter_kept = value["depth_jitter"].get<std::string>() == "keep";
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("back_edges") &&
            value["cells"]["back_edges"].is_number())
            item.world_back_edges = std::clamp(value["cells"]["back_edges"].get<float>(), 0.0f, 1.0f);
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("through_blocks") &&
            value["cells"]["through_blocks"].is_boolean())
            item.world_through_blocks = value["cells"]["through_blocks"].get<bool>();
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("dash_opacity") &&
            value["cells"]["dash_opacity"].is_number())
            item.world_dash_opacity = std::clamp(value["cells"]["dash_opacity"].get<float>(), 0.0f, 1.0f);
        if (value.contains("face_offsets_largest") && value["face_offsets_largest"].is_boolean())
            item.world_face_offsets_largest = value["face_offsets_largest"].get<bool>();
        if (value.contains("depth_camera") && value["depth_camera"].is_string()) {
            const std::string mode = value["depth_camera"].get<std::string>();
            item.world_depth_camera = mode == "gpu" ? 0 : mode == "copy" ? 1 : mode == "present" ? 2 : -1;
        }
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
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("texture") &&
            value["cells"]["texture"].is_object())
            parse_world_texture(item, value["cells"]["texture"]);
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("texture_light") &&
            value["cells"]["texture_light"].is_boolean())
            item.world_texture_light = value["cells"]["texture_light"].get<bool>();
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("under_interface") &&
            value["cells"]["under_interface"].is_boolean())
            item.world_under_interface = value["cells"]["under_interface"].get<bool>();
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("texture_relief") &&
            value["cells"]["texture_relief"].is_number())
            item.world_relief = std::clamp(value["cells"]["texture_relief"].get<float>(), 0.0f, 1.0f);
        if (value.contains("cells") && value["cells"].is_object() && value["cells"].contains("texture_keep") &&
            value["cells"]["texture_keep"].is_array())
            parse_texture_keep(item, value["cells"]["texture_keep"]);
        if (value.contains("cells")) {
            const auto& cells = value["cells"];
            if (!cells.is_object() || !cells.contains("box") || !cells["box"].is_array() || cells["box"].size() != 3 ||
                !cells.contains("data") || !is_text(cells["data"]) || !cells.contains("colors") ||
                !cells["colors"].is_object() || !cells.contains("origin"))
                fail("world: cells needs box = {x, y, z}, data (text), colors and origin = {x, y, z}");
            int box[3];
            for (int i = 0; i < 3; ++i) {
                if (!cells["box"][i].is_number_integer()) fail("world: cells: box sides are integers");
                box[i] = cells["box"][i].get<int>();
            }
            for (const auto& [key, color] : cells["colors"].items()) {
                if (key.size() != 1 || static_cast<unsigned char>(key[0]) >= 128 || (key[0] >= '0' && key[0] <= '9'))
                    fail("world: cells: color keys are single ASCII characters, not digits");
                item.cell_colors[static_cast<unsigned char>(key[0])] = parse_color(color, "colors");
            }
            const auto origin = position(cells["origin"], "cells.origin");
            for (int i = 0; i < 3; ++i) item.world_cells_origin[i] = origin[i];
            if (cells.contains("cell"))
                item.world_cells_size = std::clamp(parse_number(cells["cell"], "cells.cell"), 0.05f, 16.0f);
            item.world_cells_smooth = cells.value("smooth", false);
            if (cells.contains("edge_opacity"))
                item.world_cells_edge_opacity =
                    std::clamp(parse_number(cells["edge_opacity"], "cells.edge_opacity"), 0.0f, 1.0f);
            auto text = cells_text(cells["data"]);
            try {
                check_voxel_cells(box[0], box[1], box[2], *text);
            } catch (const std::runtime_error& error) {
                fail(error.what());
            }
            item.world_hide_hidden = cells.value("hide_hidden", false);
            item.world_hidden_dashed = cells.value("hidden_dashed", false);
            item.world_only_in_solids = cells.value("only_in_solids", false);
            if (cells.value("hide_own", false)) {
                // Its own cells, those with a color, hide each other too.
                auto own = std::make_shared<WorldSolids>();
                own->sx = box[0];
                own->sy = box[1];
                own->sz = box[2];
                for (int i = 0; i < 3; ++i) own->origin[i] = item.world_cells_origin[i];
                own->cell = item.world_cells_size;
                const std::vector<unsigned char> grid = decode_voxel_cells(box[0], box[1], box[2], *text);
                own->filled.resize(grid.size());
                for (size_t i = 0; i < grid.size(); ++i)
                    own->filled[i] = (item.cell_colors[grid[i]] & IM_COL32_A_MASK) != 0 ? 1 : 0;
                own->data = text;
                item.world_own_solids = std::move(own);
            }
            item.world_cells = slot_for(box[0], box[1], box[2], std::move(text), item.cell_colors, item.cell_marks, false,
                cells.value("one_shape", false));
        }
        if (value.contains("solids")) {
            const auto& solids = value["solids"];
            if (!solids.is_object() || !solids.contains("box") || !solids["box"].is_array() || solids["box"].size() != 3 ||
                !solids.contains("data") || !is_text(solids["data"]) || !solids.contains("origin"))
                fail("world: solids needs box = {x, y, z}, data (text) and origin = {x, y, z}");
            int box[3];
            for (int i = 0; i < 3; ++i) {
                if (!solids["box"][i].is_number_integer()) fail("world: solids: box sides are integers");
                box[i] = solids["box"][i].get<int>();
            }
            const auto origin = position(solids["origin"], "solids.origin");
            double cell = 0.5;
            if (solids.contains("cell")) cell = std::clamp(parse_number(solids["cell"], "solids.cell"), 0.05f, 16.0f);
            try {
                item.world_solids = decoded_solids(box, origin, cell, cells_text(solids["data"]));
            } catch (const std::runtime_error& error) {
                fail(error.what());
            }
        }
        if (value.contains("quads")) {
            if (!value["quads"].is_array() || value["quads"].size() > 64) fail("world: quads: at most 64");
            for (const auto& entry : value["quads"]) {
                if (!entry.is_object() || !entry.contains("corners") || !entry["corners"].is_array() ||
                    entry["corners"].size() != 4)
                    fail("world: a quad needs corners = four {x, y, z}");
                WorldQuad quad;
                for (int k = 0; k < 4; ++k) {
                    const auto corner = position(entry["corners"][k], "corners");
                    for (int i = 0; i < 3; ++i) quad.corners[k][i] = corner[i];
                }
                if (entry.contains("color")) quad.color = parse_color(entry["color"], "color");
                item.world_quads.push_back(quad);
            }
        }
        if (value.contains("lines")) {
            if (!value["lines"].is_array() || value["lines"].size() > 2048) fail("world: lines: at most 2048");
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
                add_dashed(item.world_lines, segment, parse_dashes(entry));
            }
        }
        if (value.contains("boxes")) {
            if (!value["boxes"].is_array() || value["boxes"].size() > 1024) fail("world: boxes: at most 1024");
            for (const auto& entry : value["boxes"]) {
                if (!entry.is_object() || !entry.contains("from") || !entry.contains("to"))
                    fail("world: a box needs from and to = {x, y, z}, two opposite corners");
                WorldLine box;
                const auto from = position(entry["from"], "from");
                const auto to = position(entry["to"], "to");
                for (int i = 0; i < 3; ++i) {
                    box.from[i] = from[i];
                    box.to[i] = to[i];
                }
                if (entry.contains("color")) box.color = parse_color(entry["color"], "color");
                if (entry.contains("width")) box.width = std::clamp(parse_number(entry["width"], "width"), 0.5f, 32.0f);
                add_box_edges(item.world_lines, box, parse_dashes(entry));
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
    parsing_owner = &owner;
    struct Done {
        ~Done() { parsing_owner = nullptr; }
    } done;
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

// The meshes of a zone's items, in order: voxels views, then world cells.
template <typename Items, typename Slot>
void collect_slots(Items& items, std::vector<Slot*>& voxels, std::vector<Slot*>& world) {
    for (auto& item : items) {
        if (item.voxels) voxels.push_back(&item.voxels);
        if (item.world_cells) world.push_back(&item.world_cells);
        collect_slots(item.children, voxels, world);
    }
}

// The meshes of a zone set, replacing `old_zone` (or none). A mesh of the
// same source as one of the old zone (a 3D view whose text or labels
// changed, a tile of a build that did not) takes its slot: nothing is built
// again. Every other is built on the overlay's thread, showing meanwhile the
// one at the same place in the old zone (the n-th voxels view, the n-th world
// cells), so that the view does not blink at every change.
void start_meshes(const Zone* old_zone, Zone& zone) {
    using Old = const std::shared_ptr<VoxelSlot>;
    std::vector<Old*> old_voxels, old_world;
    std::vector<std::shared_ptr<VoxelSlot>*> voxels, world;
    if (old_zone) collect_slots(old_zone->items, old_voxels, old_world);
    collect_slots(zone.items, voxels, world);
    const auto start = [](std::vector<std::shared_ptr<VoxelSlot>*>& slots, const std::vector<Old*>& old_slots) {
        for (size_t i = 0; i < slots.size(); ++i) {
            std::shared_ptr<VoxelSlot>& slot = *slots[i];
            const auto same = std::find_if(old_slots.begin(), old_slots.end(), [&](Old* old) {
                return (*old)->source && slot->source && (*old)->source->same(*slot->source);
            });
            if (same != old_slots.end()) {
                slot = **same;
                continue;
            }
            if (i < old_slots.size()) slot->previous = (*old_slots[i])->shown();
            build_voxel_mesh_later(slot);
        }
    };
    start(voxels, old_voxels);
    start(world, old_world);
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

void count_frame() {
    const auto now = FrameClock::now();
    std::lock_guard lock(output_mutex);
    if (frame_window_start == FrameClock::time_point{}) {
        frame_window_start = now;
    } else {
        ++frames_in_window;
        if (now - frame_window_start >= kFrameWindow) {
            average_frame_ms = std::chrono::duration<double, std::milli>(now - frame_window_start).count() / frames_in_window;
            frame_window_start = now;
            frames_in_window = 0;
        }
    }
    last_frame = now;
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
        const auto old = zones.find(key);
        if (old == zones.end()) {
            const auto owned = std::count_if(zones.begin(), zones.end(), [&](auto& entry) { return entry.first.first == owner; });
            if (static_cast<size_t>(owned) >= kMaxZonesPerOwner) fail("too many zones for this mod (32 at most)");
        }
        start_meshes(old != zones.end() ? old->second.get() : nullptr, *zone);
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
    std::lock_guard blobs_lock(blobs_mutex);
    std::erase_if(blobs, [&](auto& entry) { return entry.first.first == owner; });
}

// Keeps `size` bytes of `data` as the blob `id` of `owner` (a mod id), for its
// zones to name: {"blob": id} where a `cells` or `data` text is expected. A
// zone keeps the texts it was set with; a blob dropped is only not named
// again. Returns 0, or -1 without an owner.
OVERLAY_EXPORT int ShroudforgeOverlay_SetBlob(const char* owner, std::uint64_t id, const char* data, std::uint64_t size) {
    if (!owner || !*owner || (!data && size)) return -1;
    auto text = std::make_shared<const std::string>(data ? data : "", static_cast<size_t>(size));
    std::lock_guard lock(blobs_mutex);
    blobs[{owner, id}] = std::move(text);
    return 0;
}

OVERLAY_EXPORT void ShroudforgeOverlay_DropBlob(const char* owner, std::uint64_t id) {
    if (!owner) return;
    std::lock_guard lock(blobs_mutex);
    blobs.erase({owner, id});
}

// Writes {"active": bool, "width": n, "height": n, "scale": n, "menu": bool,
// "frame_ms": n, "features": [...]} into `out`.
OVERLAY_EXPORT int ShroudforgeOverlay_GetStatus(char* out, uint32_t out_size) {
    json status;
    {
        const auto now = FrameClock::now();
        std::lock_guard lock(output_mutex);
        status = {{"active", output_active}, {"width", output_width}, {"height", output_height}};
        // The average over the last second of frames, 0 before it. When the
        // game has shown no frame for longer, the time since its last frame.
        double frame_ms = average_frame_ms;
        if (last_frame != FrameClock::time_point{} && now - last_frame > kFrameWindow)
            frame_ms = std::chrono::duration<double, std::milli>(now - last_frame).count();
        status["frame_ms"] = std::round(frame_ms * 100.0) / 100.0;
    }
    {
        std::lock_guard lock(scene_mutex);
        status["scale"] = (status["height"].get<int>() / kReferenceHeight) * options.scale;
        status["menu"] = menu_open;
    }
    // What this overlay knows beyond API version 1, for mods that fall back
    // without it (an older overlay ignores the fields it does not know).
    status["features"] = json::array({"dashes", "same_mesh", "blobs", "frame_time", "hide_hidden", "only_in_solids"});
    // Drawing the world items against the game's depth (game-depth.on).
    if (depth::enabled()) status["features"].push_back("game_depth");
    // cells.texture: the solid style textured (BC7 levels, 31/10).
    if (depth::enabled()) status["features"].push_back("textures");
    const std::string text = status.dump();
    if (!out || out_size <= text.size()) return -1;
    memcpy(out, text.c_str(), text.size() + 1);
    return 0;
}
