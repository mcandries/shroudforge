// Layout and drawing of the zones. Items are stacked vertically inside a
// zone; a row places its items side by side, centered vertically, `grow`
// items taking the width left; a flow wraps words, chips and icons like a
// sentence. Sizes given by mods are reference pixels (the zone's reference
// screen height, 1080 by default) multiplied by the screen scale.

#include "draw_scene.h"
#include "local_camera.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>

namespace overlay {

namespace {

struct Context {
    ImDrawList& list;
    const DrawResources& resources;
    float scale;
    float opacity;
    std::string zone;  // Owner and id of the zone drawn, for what is kept between its updates.
};

ImU32 fade(const Context& c, ImU32 color) {
    const float alpha = ((color >> IM_COL32_A_SHIFT) & 0xFF) * c.opacity;
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
}

bool visible_color(ImU32 color) {
    return ((color >> IM_COL32_A_SHIFT) & 0xFF) != 0;
}

ImVec2 snap(ImVec2 point) {
    return {std::floor(point.x + 0.5f), std::floor(point.y + 0.5f)};
}

ImVec2 image_size(const Context& c, const Item& item) {
    const float native_w = static_cast<float>(item.image->width) * (item.uv.z - item.uv.x);
    const float native_h = static_cast<float>(item.image->height) * (item.uv.w - item.uv.y);
    float w = item.size.x, h = item.size.y;
    if (w <= 0.0f && h <= 0.0f) {
        w = native_w;
        h = native_h;
    } else if (w <= 0.0f) {
        w = native_h > 0.0f ? h * native_w / native_h : h;
    } else if (h <= 0.0f) {
        h = native_w > 0.0f ? w * native_h / native_w : w;
    }
    return {w * c.scale, h * c.scale};
}

ImVec2 text_size(const Context& c, const TextStyle& style, const char* begin, const char* end, float wrap) {
    ImFont* font = c.resources.font(style.font);
    return font->CalcTextSizeA(style.size * c.scale, FLT_MAX, wrap, begin, end);
}

void draw_text_run(const Context& c, const TextStyle& style, ImVec2 position, const char* begin, const char* end, float wrap) {
    ImFont* font = c.resources.font(style.font);
    const float size = style.size * c.scale;
    position = snap(position);
    if (style.outline > 0.0f) {
        const float o = std::max(1.0f, style.outline * c.scale);
        const ImU32 color = fade(c, style.outline_color);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) c.list.AddText(font, size, ImVec2(position.x + dx * o, position.y + dy * o), color, begin, end, wrap);
    }
    if (style.shadow) {
        const ImVec2 o(style.shadow_offset.x * c.scale, style.shadow_offset.y * c.scale);
        c.list.AddText(font, size, ImVec2(position.x + o.x, position.y + o.y), fade(c, style.shadow_color), begin, end, wrap);
    }
    c.list.AddText(font, size, position, fade(c, style.color), begin, end, wrap);
}

ImVec2 measure(const Context& c, const Item& item, float width);
void draw_item(const Context& c, const Item& item, ImVec2 position, ImVec2 size, float content_width, float wrap_width);

// ------------------------------------------------------------------ flow

// A flow is cut into tokens (words of its texts, and each other item), then
// broken into lines that fit the width.
struct Token {
    const Item* item;
    const char* begin = nullptr;  // Word range for a text item.
    const char* end = nullptr;
    ImVec2 size;
    bool word = false;
    bool tight = false;  // Continues the previous word, without a space.
};

// Reads one UTF-8 character and moves past it.
unsigned next_char(const char*& at) {
    const unsigned char lead = static_cast<unsigned char>(*at);
    const int length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    unsigned c = length == 1 ? lead : lead & (0xFF >> (length + 1));
    int read = 1;
    for (; read < length && (static_cast<unsigned char>(at[read]) & 0xC0) == 0x80; ++read)
        c = (c << 6) | (static_cast<unsigned char>(at[read]) & 0x3F);
    at += read;
    return c;
}

// Chinese and Japanese text has no spaces: a line may break between any two
// of its characters, except before closing punctuation, small kana or the
// long vowel mark, and after an opening bracket.
bool is_cjk(unsigned c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

bool cannot_start_line(unsigned c) {
    static constexpr unsigned kChars[] = {0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F,
        0xFF09, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x30FB, 0x30FC, 0x2026, 0x3041, 0x3043, 0x3045,
        0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3,
        0x30E3, 0x30E5, 0x30E7};
    return std::find(std::begin(kChars), std::end(kChars), c) != std::end(kChars);
}

bool cannot_end_line(unsigned c) {
    return c == 0xFF08 || c == 0x3008 || c == 0x300A || c == 0x300C || c == 0x300E || c == 0x3010;
}

bool can_break(unsigned before, unsigned after) {
    return (is_cjk(before) || is_cjk(after)) && !cannot_end_line(before) && !cannot_start_line(after);
}

struct FlowLine {
    size_t first = 0;
    size_t count = 0;
    float width = 0.0f;
    float height = 0.0f;
};

std::vector<Token> flow_tokens(const Context& c, const Item& flow) {
    std::vector<Token> tokens;
    for (const Item& child : flow.children) {
        if (child.kind != ItemKind::Text) {
            tokens.push_back({&child, nullptr, nullptr, measure(c, child, FLT_MAX), false});
            continue;
        }
        auto add_word = [&](const char* begin, const char* end, bool tight) {
            tokens.push_back({&child, begin, end, text_size(c, child.style, begin, end, 0.0f), true, tight});
        };
        const char* at = child.text.c_str();
        while (*at) {
            while (*at == ' ') ++at;
            const char* begin = at;
            bool tight = false;
            unsigned previous = 0;
            while (*at && *at != ' ') {
                const char* here = at;
                const unsigned ch = next_char(at);
                if (here > begin && can_break(previous, ch)) {
                    add_word(begin, here, tight);
                    begin = here;
                    tight = true;
                }
                previous = ch;
            }
            if (at > begin) add_word(begin, at, tight);
        }
    }
    return tokens;
}

// Gap before a token: none before punctuation or inside a word cut between
// Chinese or Japanese characters, a space between two words, the flow
// spacing otherwise.
float flow_gap(const Context& c, const Item& flow, const Token& previous, const Token& token) {
    if (token.word && (token.tight || std::strchr(",.;:!?)", token.begin[0]))) return 0.0f;
    if (previous.word && token.word)
        return text_size(c, token.item->style, " ", nullptr, 0.0f).x;
    return flow.spacing * c.scale;
}

std::vector<FlowLine> flow_lines(const Context& c, const Item& flow, const std::vector<Token>& tokens, float width) {
    std::vector<FlowLine> lines;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        const float gap = (lines.empty() || lines.back().count == 0) ? 0.0f : flow_gap(c, flow, tokens[i - 1], token);
        if (lines.empty() || (lines.back().count > 0 && lines.back().width + gap + token.size.x > width)) {
            lines.push_back({i, 0, 0.0f, 0.0f});
        }
        FlowLine& line = lines.back();
        line.width += (line.count ? gap : 0.0f) + token.size.x;
        line.height = std::max(line.height, token.size.y);
        ++line.count;
    }
    return lines;
}

ImVec2 measure_flow(const Context& c, const Item& flow, float width) {
    const auto tokens = flow_tokens(c, flow);
    const auto lines = flow_lines(c, flow, tokens, width);
    ImVec2 size{0.0f, 0.0f};
    for (const FlowLine& line : lines) {
        size.x = std::max(size.x, line.width);
        size.y += line.height;
    }
    if (lines.size() > 1) size.y += flow.line_gap * c.scale * (lines.size() - 1);
    return size;
}

void draw_flow(const Context& c, const Item& flow, ImVec2 position, float width) {
    const auto tokens = flow_tokens(c, flow);
    const auto lines = flow_lines(c, flow, tokens, width);
    float y = position.y;
    for (const FlowLine& line : lines) {
        float x = position.x;
        for (size_t i = line.first; i < line.first + line.count; ++i) {
            const Token& token = tokens[i];
            if (i > line.first) x += flow_gap(c, flow, tokens[i - 1], token);
            const ImVec2 at(x, y + (line.height - token.size.y) * 0.5f);
            if (token.word)
                draw_text_run(c, token.item->style, at, token.begin, token.end, 0.0f);
            else
                draw_item(c, *token.item, at, token.size, token.size.x, FLT_MAX);
            x += token.size.x;
        }
        y += line.height + flow.line_gap * c.scale;
    }
}

// ------------------------------------------------------------------ items

// Size of an item; `width` is the available width, or FLT_MAX when the zone
// fits its content (text then does not wrap).
ImVec2 measure(const Context& c, const Item& item, float width) {
    switch (item.kind) {
    case ItemKind::Text: {
        const float wrap = width < FLT_MAX ? width : 0.0f;
        return text_size(c, item.style, item.text.c_str(), nullptr, wrap);
    }
    case ItemKind::Image:
        return image_size(c, item);
    case ItemKind::Row: {
        ImVec2 size{0.0f, 0.0f};
        for (const Item& child : item.children) {
            const ImVec2 child_size = measure(c, child, FLT_MAX);
            size.x += child_size.x;
            size.y = std::max(size.y, child_size.y);
        }
        if (item.children.size() > 1) size.x += item.spacing * c.scale * (item.children.size() - 1);
        return size;
    }
    case ItemKind::Flow:
        return measure_flow(c, item, width);
    case ItemKind::Spacer:
        return {0.0f, item.size.y * c.scale};
    case ItemKind::Separator:
        return {0.0f, std::max(1.0f, item.thickness * c.scale)};
    case ItemKind::Progress: {
        const float w = item.size.x > 0.0f ? item.size.x * c.scale : (width < FLT_MAX ? width : 120.0f * c.scale);
        return {w, item.size.y * c.scale};
    }
    case ItemKind::Chip: {
        const ImVec2 text = text_size(c, item.style, item.text.c_str(), nullptr, 0.0f);
        return {std::max(item.size.x * c.scale, text.x + item.padding * 2.0f * c.scale),
            std::max(item.size.y * c.scale, text.y)};
    }
    case ItemKind::Icon:
    case ItemKind::Toggle:
        return {item.size.x * c.scale, item.size.y * c.scale};
    case ItemKind::Grow:
        return {0.0f, 0.0f};
    case ItemKind::Grid: {
        size_t columns = 0;
        for (const std::string& row : item.cells) columns = std::max(columns, row.size());
        auto span = [&](size_t count) {
            return count == 0 ? 0.0f : (count * item.cell + (count - 1) * item.gap) * c.scale;
        };
        return {span(columns), span(item.cells.size())};
    }
    case ItemKind::Voxels:
        return {item.size.x * c.scale, item.size.y * c.scale};
    case ItemKind::World:
        return {0.0f, 0.0f};
    }
    return {0.0f, 0.0f};
}

// Both edges of each cell are snapped, so small cells at a fractional screen
// scale keep even sizes and gaps instead of overlapping.
void draw_grid(const Context& c, const Item& item, ImVec2 position) {
    const float step = (item.cell + item.gap) * c.scale;
    const float side = std::max(1.0f, item.cell * c.scale);
    const float rounding = item.rounding * c.scale;
    for (size_t y = 0; y < item.cells.size(); ++y) {
        const std::string& row = item.cells[y];
        for (size_t x = 0; x < row.size(); ++x) {
            const unsigned char ch = static_cast<unsigned char>(row[x]);
            const ImU32 color = item.cell_colors[ch];
            const ImU32 mark = item.cell_marks[ch];
            if (!visible_color(color) && !visible_color(mark)) continue;
            const ImVec2 origin(position.x + x * step, position.y + y * step);
            const ImVec2 min = snap(origin);
            const ImVec2 max = snap(ImVec2(origin.x + side, origin.y + side));
            if (visible_color(color)) c.list.AddRectFilled(min, max, fade(c, color), rounding);
            if (visible_color(mark)) {
                // A centred square of about half the cell, at least a pixel.
                const float inset = std::floor((max.x - min.x) * 0.25f);
                c.list.AddRectFilled(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset),
                    fade(c, mark));
            }
        }
    }
}

// ------------------------------------------------------------------ voxels

ImU32 shade(ImU32 color, float light, float alpha_factor) {
    const auto channel = [&](int shift) {
        return static_cast<ImU32>(std::min(255.0f, ((color >> shift) & 0xFF) * light + 0.5f)) << shift;
    };
    const float alpha = ((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha_factor;
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
        (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
}

constexpr float kNormals[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

// Whether a point of a face is seen from the camera: a ray from it towards
// the camera (`towards`, unit) crosses no drawn cell before it leaves the box
// (a grid walk, cell by cell). `a` and `b` move the point across the face.
bool point_seen(const VoxelMesh& mesh, const VoxelFace& face, const float towards[3], float a, float b) {
    const float* n = kNormals[face.direction];
    const int axis_n = face.direction / 2, axis_a = (axis_n + 1) % 3, axis_b = (axis_n + 2) % 3;
    float start[3] = {face.x + 0.5f + n[0] * 0.501f, face.y + 0.5f + n[1] * 0.501f, face.z + 0.5f + n[2] * 0.501f};
    start[axis_a] += a;
    start[axis_b] += b;
    const int size[3] = {mesh.sx, mesh.sy, mesh.sz};
    int cell[3], step[3];
    float next[3], delta[3];
    for (int axis = 0; axis < 3; ++axis) {
        cell[axis] = static_cast<int>(std::floor(start[axis]));
        const float d = towards[axis];
        if (std::fabs(d) < 1e-6f) {
            step[axis] = 0;
            next[axis] = delta[axis] = FLT_MAX;
        } else {
            step[axis] = d > 0.0f ? 1 : -1;
            const float border = d > 0.0f ? cell[axis] + 1.0f : static_cast<float>(cell[axis]);
            next[axis] = (border - start[axis]) / d;
            delta[axis] = std::fabs(1.0f / d);
        }
    }
    while (true) {
        if (cell[0] < 0 || cell[1] < 0 || cell[2] < 0 || cell[0] >= size[0] || cell[1] >= size[1] || cell[2] >= size[2])
            return true;
        if (mesh.drawn[(static_cast<size_t>(cell[2]) * size[1] + cell[1]) * size[0] + cell[0]]) return false;
        const int axis = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
        cell[axis] += step[axis];
        next[axis] += delta[axis];
    }
}

// Whether any of five points of a face (its centre and four towards its
// corners) is seen: a face half hidden is drawn whole.
bool seen_from_camera(const VoxelMesh& mesh, const VoxelFace& face, const float towards[3]) {
    static constexpr float kSamples[5][2] = {{0, 0}, {-0.35f, -0.35f}, {0.35f, -0.35f}, {-0.35f, 0.35f}, {0.35f, 0.35f}};
    for (const auto& sample : kSamples)
        if (point_seen(mesh, face, towards, sample[0], sample[1])) return true;
    return false;
}

// View of the box from a camera turning around its centre, parallel or in
// perspective (`perspective`): yaw
// 0 looks along +z with +x on the right and +y up (the game's axes), pitch
// looks down. Faces turned away are skipped and the others drawn from the
// farthest, lit by their direction. Solid and see-through (`opacity` or the
// layer's below 1): drawn opaque off screen, then laid over the game at
// once, so the view is the opaque one made lighter; without an off-screen
// target, only the faces seen from the camera, so what is hidden stays
// hidden. Opaque, the nearer faces simply cover the others. X-ray: every
// face turned to the camera, inner ones included.
// The camera angles a voxels view shows, per zone, eased towards the
// item's when `ease` is set: a new zone with another angle turns the view
// instead of jumping.
struct ShownCamera {
    float yaw = 0.0f, pitch = 0.0f;
    double time = 0.0;
};

// World items: points and segments of the game world projected with the
// game's camera read for this frame (local_camera), over the whole screen
// whatever the zone's place. The camera's orientation takes its axes to the
// world's; it looks along its +z, +x to the right, +y up; fovY in radians.
// Nothing is drawn while the camera cannot be read.
struct WorldView {
    double eye[3]{}, right[3]{}, up[3]{}, forward[3]{};
    double focal_x = 1.0, focal_y = 1.0;
    ImVec2 screen;

    // Camera space of a world point: x right, y up, z forward (meters).
    void to_camera(const double point[3], double out[3]) const {
        const double d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
        out[0] = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        out[1] = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        out[2] = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
    }
    ImVec2 to_screen(const double p[3]) const {
        return ImVec2(static_cast<float>(screen.x * 0.5 * (1.0 + p[0] / (p[2] * focal_x))),
            static_cast<float>(screen.y * 0.5 * (1.0 - p[1] / (p[2] * focal_y))));
    }
};

bool world_view(WorldView& view, double lead = 0.0, LocalCamera* read = nullptr) {
    LocalCamera camera;
    if (!local_camera(camera, lead)) return false;
    if (read) *read = camera;
    const double* q = camera.orientation;
    const auto rotate = [&](double x, double y, double z, double out[3]) {
        // v + 2w (q x v) + 2 q x (q x v)
        const double tx = 2.0 * (q[1] * z - q[2] * y), ty = 2.0 * (q[2] * x - q[0] * z), tz = 2.0 * (q[0] * y - q[1] * x);
        out[0] = x + q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = y + q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = z + q[3] * tz + (q[0] * ty - q[1] * tx);
    };
    for (int i = 0; i < 3; ++i) view.eye[i] = camera.position[i];
    rotate(1.0, 0.0, 0.0, view.right);
    rotate(0.0, 1.0, 0.0, view.up);
    rotate(0.0, 0.0, 1.0, view.forward);
    view.screen = ImGui::GetIO().DisplaySize;
    if (view.screen.x <= 0.0f || view.screen.y <= 0.0f) return false;
    view.focal_y = std::tan(camera.fov_y * 0.5);
    view.focal_x = view.focal_y * view.screen.x / view.screen.y;
    return view.focal_y > 0.0;
}

// The face the building cursor aims at: the cursor's transform is the corner
// of the placement cube, turned so that its local +y is the aimed face's
// normal; the placement cell is at its centre, the aimed cell half a cube
// further along local -y (as Architect Line's line.cursor_cells). Its four
// corners in meters, or false when the cells are not neighbours.
bool aimed_face(const LocalCamera& camera, const Item& item, double corners[4][3]) {
    const double* q = camera.cursor_orientation;
    const double h = item.aim_cell * 0.5;
    const auto cell_at = [&](double x, double y, double z, long long out[3]) {
        const double tx = 2.0 * (q[1] * z - q[2] * y), ty = 2.0 * (q[2] * x - q[0] * z), tz = 2.0 * (q[0] * y - q[1] * x);
        const double offset[3] = {x + q[3] * tx + (q[1] * tz - q[2] * ty), y + q[3] * ty + (q[2] * tx - q[0] * tz),
            z + q[3] * tz + (q[0] * ty - q[1] * tx)};
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<long long>(
                std::floor((camera.cursor_position[i] + offset[i] - item.aim_origin[i]) / item.aim_cell));
    };
    long long place[3], aim[3];
    cell_at(h, h, h, place);
    cell_at(h, -h, h, aim);
    int axis = -1;
    for (int i = 0; i < 3; ++i) {
        if (place[i] == aim[i]) continue;
        if (axis >= 0 || std::llabs(place[i] - aim[i]) != 1) return false;
        axis = i;
    }
    if (axis < 0) return false;
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    const double plane = item.aim_origin[axis] + static_cast<double>(std::max(place[axis], aim[axis])) * item.aim_cell;
    static constexpr int kSquare[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; ++k) {
        corners[k][axis] = plane;
        corners[k][u] = item.aim_origin[u] + static_cast<double>(aim[u] + kSquare[k][0]) * item.aim_cell;
        corners[k][v] = item.aim_origin[v] + static_cast<double>(aim[v] + kSquare[k][1]) * item.aim_cell;
    }
    return true;
}

// A box of cells in the world (`cells` of a world item): the faces turned to
// the camera, filled with their cell's color and outlined with it more
// opaque, the farthest first; at most kMaxWorldFaces.
constexpr size_t kMaxWorldFaces = 30000;
void draw_world_cells(const Context& c, const Item& item, const WorldView& view) {
    constexpr double kNear = 0.05;
    const VoxelMesh& mesh = *item.world_cells;
    const double s = item.world_cells_size;
    struct Shown {
        double depth;
        ImVec2 points[8];
        int count;
        ImU32 fill, edge;
    };
    std::vector<Shown> shown;
    shown.reserve(std::min(mesh.faces.size(), kMaxWorldFaces));
    static constexpr int kSquare[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const VoxelFace& face : mesh.faces) {
        if (face.inner) continue;
        if (shown.size() >= kMaxWorldFaces) break;
        const int axis = face.direction / 2;
        const bool positive = face.direction % 2 == 1;
        const double base[3] = {item.world_cells_origin[0] + face.x * s, item.world_cells_origin[1] + face.y * s,
            item.world_cells_origin[2] + face.z * s};
        const double plane = base[axis] + (positive ? s : 0.0);
        // Only the faces turned to the camera.
        if ((view.eye[axis] - plane) * (positive ? 1.0 : -1.0) <= 0.0) continue;
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        double in[4][3];
        double centre[3] = {0.0, 0.0, 0.0};
        for (int k = 0; k < 4; ++k) {
            double corner[3];
            corner[axis] = plane;
            corner[u] = base[u] + kSquare[k][0] * s;
            corner[v] = base[v] + kSquare[k][1] * s;
            view.to_camera(corner, in[k]);
            for (int i = 0; i < 3; ++i) centre[i] += in[k][i] * 0.25;
        }
        Shown entry{};
        for (int k = 0; k < 4; ++k) {
            const double* a = in[k];
            const double* b = in[(k + 1) % 4];
            if (a[2] >= kNear) entry.points[entry.count++] = view.to_screen(a);
            if ((a[2] >= kNear) != (b[2] >= kNear)) {
                const double t = (kNear - a[2]) / (b[2] - a[2]);
                const double cut[3] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, kNear};
                entry.points[entry.count++] = view.to_screen(cut);
            }
        }
        if (entry.count < 3) continue;
        const ImU32 color = item.cell_colors[face.ch];
        entry.depth = centre[2];
        entry.fill = fade(c, color);
        entry.edge = fade(c, (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 230));
        shown.push_back(entry);
    }
    std::sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.depth > b.depth; });
    for (const Shown& entry : shown) {
        c.list.AddConvexPolyFilled(entry.points, entry.count, entry.fill);
        c.list.AddPolyline(entry.points, entry.count, entry.edge, 1.5f * c.scale, ImDrawFlags_Closed);
    }
}

void draw_world(const Context& c, const Item& item) {
    WorldView view;
    LocalCamera camera;
    if (!world_view(view, item.camera_lead, &camera)) return;
    constexpr double kNear = 0.05;
    c.list.PushClipRectFullScreen();
    const auto segment = [&](const double from[3], const double to[3], ImU32 color, float width) {
        double a[3], b[3];
        view.to_camera(from, a);
        view.to_camera(to, b);
        if (a[2] < kNear && b[2] < kNear) return;
        // Cut at the near plane what lies behind the camera.
        if (a[2] < kNear || b[2] < kNear) {
            double* behind = a[2] < kNear ? a : b;
            const double* front = a[2] < kNear ? b : a;
            const double t = (kNear - front[2]) / (behind[2] - front[2]);
            for (int i = 0; i < 3; ++i) behind[i] = front[i] + (behind[i] - front[i]) * t;
        }
        c.list.AddLine(view.to_screen(a), view.to_screen(b), fade(c, color), width * c.scale);
    };
    if (item.world_cells) draw_world_cells(c, item, view);
    // Quads first, under the lines; the part behind the camera cut off.
    for (const WorldQuad& quad : item.world_quads) {
        double in[4][3];
        for (int k = 0; k < 4; ++k) view.to_camera(quad.corners[k], in[k]);
        ImVec2 out[8];
        int count = 0;
        for (int k = 0; k < 4; ++k) {
            const double* a = in[k];
            const double* b = in[(k + 1) % 4];
            if (a[2] >= kNear) out[count++] = view.to_screen(a);
            if ((a[2] >= kNear) != (b[2] >= kNear)) {
                const double t = (kNear - a[2]) / (b[2] - a[2]);
                const double cut[3] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, kNear};
                out[count++] = view.to_screen(cut);
            }
        }
        if (count >= 3) c.list.AddConvexPolyFilled(out, count, fade(c, quad.color));
    }
    for (const WorldLine& line : item.world_lines) segment(line.from, line.to, line.color, line.width);
    double corners[4][3];
    if (item.aim_face && camera.has_cursor && !(camera.cursor_flags & 1u) && aimed_face(camera, item, corners))
        for (int k = 0; k < 4; ++k) segment(corners[k], corners[(k + 1) % 4], item.aim_face_color, item.aim_face_width);
    for (const WorldPoint& point : item.world_points) {
        double p[3];
        view.to_camera(point.at, p);
        if (p[2] < kNear) continue;
        const ImVec2 at = view.to_screen(p);
        const float r = point.size * 0.5f * c.scale;
        const ImU32 color = fade(c, point.color);
        switch (point.shape) {
        case WorldShape::Ring:
            c.list.AddCircle(at, r, color, 24, std::max(1.5f, 2.0f * c.scale));
            break;
        case WorldShape::Cross:
            c.list.AddLine(ImVec2(at.x - r, at.y), ImVec2(at.x + r, at.y), color, std::max(1.5f, 2.0f * c.scale));
            c.list.AddLine(ImVec2(at.x, at.y - r), ImVec2(at.x, at.y + r), color, std::max(1.5f, 2.0f * c.scale));
            break;
        case WorldShape::Dot:
            c.list.AddCircleFilled(at, r, color, 16);
            break;
        }
    }
    c.list.PopClipRect();
}

void draw_voxels(const Context& c, const Item& item, ImVec2 position, ImVec2 size) {
    const VoxelMesh& mesh = *item.voxels;
    constexpr float kRadians = 3.14159265f / 180.0f;
    const double now = ImGui::GetTime();
    float yaw_degrees = item.yaw + item.spin * static_cast<float>(now), pitch_degrees = item.pitch;
    // Following the game's camera: its yaw for this frame, plus the offset.
    // Not eased: it already moves with the camera at every frame.
    bool following = false;
    WorldView game;
    if (item.follow_camera && world_view(game)) {
        yaw_degrees = static_cast<float>(std::atan2(game.forward[0], game.forward[2])) / kRadians + item.camera_offset +
            item.spin * static_cast<float>(now);
        following = true;
    }
    if (item.ease > 0.0f) {
        static std::unordered_map<std::string, ShownCamera> shown;
        const auto [entry, created] = shown.try_emplace(c.zone, ShownCamera{yaw_degrees, pitch_degrees, now});
        ShownCamera& camera = entry->second;
        if (!created) {
            // About 95 % of the way after `ease` seconds, the short way round.
            const float step = 1.0f - std::exp(-static_cast<float>(now - camera.time) * 3.0f / item.ease);
            camera.yaw = following ? yaw_degrees : camera.yaw + std::remainder(yaw_degrees - camera.yaw, 360.0f) * step;
            camera.pitch += (pitch_degrees - camera.pitch) * step;
        }
        camera.time = now;
        yaw_degrees = camera.yaw;
        pitch_degrees = camera.pitch;
    }
    const float yaw = yaw_degrees * kRadians;
    const float pitch = pitch_degrees * kRadians;
    const float fx = std::sin(yaw), fz = std::cos(yaw);
    // Right, view direction and up of the camera.
    const float r[3] = {fz, 0.0f, -fx};
    const float f[3] = {fx * std::cos(pitch), -std::sin(pitch), fz * std::cos(pitch)};
    const float u[3] = {fx * std::sin(pitch), std::cos(pitch), fz * std::sin(pitch)};
    const float diagonal = std::sqrt(static_cast<float>(mesh.sx * mesh.sx + mesh.sy * mesh.sy + mesh.sz * mesh.sz));
    const float k = item.zoom * std::min(size.x, size.y) / std::max(1.0f, diagonal);
    const ImVec2 centre(position.x + size.x * 0.5f, position.y + size.y * 0.5f);
    const float half[3] = {mesh.sx * 0.5f, mesh.sy * 0.5f, mesh.sz * 0.5f};
    // Perspective: the camera, `distance` from the centre, sees the box in a
    // field of up to 90 degrees; nearer points are drawn larger. The sphere
    // around the box shows as large as in the parallel view, so it still fits.
    const float radius = std::max(0.5f, diagonal * 0.5f);
    const float field = item.perspective * 90.0f * kRadians;
    const float distance = field > 0.001f ? radius / std::tan(field * 0.5f) + radius : 0.0f;
    const float fit = distance > 0.0f ? std::sqrt(distance * distance - radius * radius) : 0.0f;
    const auto project = [&](float x, float y, float z) {
        x -= half[0];
        y -= half[1];
        z -= half[2];
        const float s = distance > 0.0f ? fit / (distance + x * f[0] + y * f[1] + z * f[2]) : 1.0f;
        return ImVec2(centre.x + (x * r[0] + y * r[1] + z * r[2]) * k * s,
            centre.y - (x * u[0] + y * u[1] + z * u[2]) * k * s);
    };
    // Farther first: along the view, or from the camera in perspective.
    const auto depth = [&](float x, float y, float z) {
        x -= half[0];
        y -= half[1];
        z -= half[2];
        if (distance <= 0.0f) return x * f[0] + y * f[1] + z * f[2];
        x += f[0] * distance;
        y += f[1] * distance;
        z += f[2] * distance;
        return std::sqrt(x * x + y * y + z * z);
    };
    // Whether a face is turned to the camera (from its centre in perspective).
    const auto turned_to_camera = [&](const VoxelFace& face) {
        const float* n = kNormals[face.direction];
        if (distance <= 0.0f) return n[0] * f[0] + n[1] * f[1] + n[2] * f[2] < 0.0f;
        const float x = face.x + 0.5f + n[0] * 0.5f - half[0] + f[0] * distance;
        const float y = face.y + 0.5f + n[1] * 0.5f - half[1] + f[1] * distance;
        const float z = face.z + 0.5f + n[2] * 0.5f - half[2] + f[2] * distance;
        return n[0] * x + n[1] * y + n[2] * z < 0.0f;
    };

    c.list.PushClipRect(position, ImVec2(position.x + size.x, position.y + size.y), true);
    if (item.floor_grid > 0 && visible_color(item.grid_color)) {
        const ImU32 color = fade(c, shade(item.grid_color, 1.0f, item.opacity));
        for (int x = 0; x <= mesh.sx; x += item.floor_grid)
            c.list.AddLine(project(x, 0, 0), project(x, 0, mesh.sz), color);
        for (int z = 0; z <= mesh.sz; z += item.floor_grid)
            c.list.AddLine(project(0, 0, z), project(mesh.sx, 0, z), color);
    }

    // Corners of each face of the unit cube, in order around it.
    static constexpr float kCorners[6][4][3] = {
        {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}, {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}},
        {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}, {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}};
    static constexpr float kLight[6] = {0.82f, 0.82f, 0.55f, 1.0f, 0.68f, 0.68f};
    // Solid and lighter than opaque: the faces are drawn opaque off screen,
    // then laid over the game with that opacity, at once. (Not in X-ray: its
    // partly transparent faces would be darkened by the second blending.)
    const float over = item.opacity * c.opacity;
    const Offscreen target =
        !item.xray && over < 0.999f && c.resources.offscreen ? c.resources.offscreen() : Offscreen{};
    const bool layered = target.list != nullptr && target.size.x > 0.0f && target.size.y > 0.0f;
    ImDrawList& faces = layered ? *target.list : c.list;
    if (layered) faces.PushClipRect(position, ImVec2(position.x + size.x, position.y + size.y), false);
    // Without it, see-through solid view: the faces seen from this camera,
    // found again only when it moves.
    const bool see_through = !layered && !item.xray && item.opacity < 0.999f;
    VoxelVisibility& seen = mesh.visibility;
    if (see_through && (!seen.valid || seen.yaw != yaw || seen.pitch != pitch || seen.distance != distance)) {
        const float towards[3] = {-f[0], -f[1], -f[2]};
        seen.faces.clear();
        for (std::uint32_t i = 0; i < mesh.faces.size(); ++i) {
            const VoxelFace& face = mesh.faces[i];
            if (face.inner || !turned_to_camera(face)) continue;
            if (seen_from_camera(mesh, face, towards)) seen.faces.push_back(i);
        }
        seen.valid = true;
        seen.yaw = yaw;
        seen.pitch = pitch;
        seen.distance = distance;
    }
    std::vector<std::pair<float, std::uint32_t>> order;
    const auto add = [&](std::uint32_t i) {
        const VoxelFace& face = mesh.faces[i];
        const float* n = kNormals[face.direction];
        order.emplace_back(depth(face.x + 0.5f + n[0] * 0.5f, face.y + 0.5f + n[1] * 0.5f, face.z + 0.5f + n[2] * 0.5f), i);
    };
    if (!see_through) {
        order.reserve(mesh.faces.size());
        for (std::uint32_t i = 0; i < mesh.faces.size(); ++i) {
            const VoxelFace& face = mesh.faces[i];
            if ((item.xray || !face.inner) && turned_to_camera(face)) add(i);
        }
    } else {
        order.reserve(seen.faces.size());
        for (const std::uint32_t i : seen.faces) add(i);
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    // Plain solid view: neither outlines nor anti-aliased borders, whose
    // overlaps would draw the seams between faces when see-through.
    const bool plain = item.plain && !item.xray;
    const bool edges = !plain && visible_color(item.edge) && k >= 3.0f;
    const ImDrawListFlags flags = faces.Flags;
    if (plain) faces.Flags &= ~ImDrawListFlags_AntiAliasedFill;
    // Off screen, the colors as they are: the opacity comes when laid over.
    const auto color = [&](ImU32 value) { return layered ? value : fade(c, value); };
    for (const auto& [unused, i] : order) {
        const VoxelFace& face = mesh.faces[i];
        const bool ghost = item.xray && item.ghost.find(static_cast<char>(face.ch)) != std::string::npos;
        const float alpha = (ghost ? item.ghost_alpha : 1.0f) * (layered ? 1.0f : item.opacity);
        ImVec2 points[4];
        for (int corner = 0; corner < 4; ++corner) {
            const float* p = kCorners[face.direction][corner];
            points[corner] = project(face.x + p[0], face.y + p[1], face.z + p[2]);
        }
        faces.AddQuadFilled(points[0], points[1], points[2], points[3],
            color(shade(item.cell_colors[face.ch], kLight[face.direction], alpha)));
        if (edges) faces.AddQuad(points[0], points[1], points[2], points[3], color(shade(item.edge, 1.0f, alpha)));
    }
    faces.Flags = flags;
    if (layered) {
        faces.PopClipRect();
        const ImVec2 end(position.x + size.x, position.y + size.y);
        c.list.AddImage(target.texture, position, end, ImVec2(position.x / target.size.x, position.y / target.size.y),
            ImVec2(end.x / target.size.x, end.y / target.size.y),
            IM_COL32(255, 255, 255, static_cast<int>(over * 255.0f + 0.5f)));
    }
    // Marks on top: a dot at the centre of their cell.
    const float dot = std::max(2.0f, k * 0.35f);
    for (const VoxelMark& mark : mesh.marks)
        c.list.AddCircleFilled(project(mark.x + 0.5f, mark.y + 0.5f, mark.z + 0.5f), dot,
            fade(c, item.cell_marks[mark.ch]), 12);
    c.list.PopClipRect();
}

void draw_icon(const Context& c, const Item& item, ImVec2 position, ImVec2 size) {
    const float k = std::min(size.x, size.y) / item.viewbox;
    const ImVec2 origin(position.x + (size.x - item.viewbox * k) * 0.5f, position.y + (size.y - item.viewbox * k) * 0.5f);
    const ImU32 color = fade(c, item.color);
    const float thickness = std::max(1.0f, item.stroke * k);
    std::vector<ImVec2> points;
    for (const Polyline& line : item.icon) {
        points.clear();
        for (const ImVec2& point : line.points) points.emplace_back(origin.x + point.x * k, origin.y + point.y * k);
        c.list.AddPolyline(points.data(), static_cast<int>(points.size()), color, thickness,
            line.closed ? ImDrawFlags_Closed : ImDrawFlags_None);
        // Round joins and caps, as the stroke-linecap/linejoin of the source icons.
        for (const ImVec2& point : points) c.list.AddCircleFilled(point, thickness * 0.5f, color, 8);
    }
}

void draw_row(const Context& c, const Item& item, ImVec2 position, ImVec2 size, float content_width) {
    std::vector<ImVec2> sizes;
    float natural = 0.0f;
    int grows = 0;
    for (const Item& child : item.children) {
        sizes.push_back(measure(c, child, FLT_MAX));
        natural += sizes.back().x;
        if (child.kind == ItemKind::Grow) ++grows;
    }
    if (item.children.size() > 1) natural += item.spacing * c.scale * (item.children.size() - 1);
    // Grow items share the width the row has beyond its content.
    const float extra = grows && content_width < FLT_MAX ? std::max(0.0f, content_width - natural) / grows : 0.0f;
    float x = position.x;
    for (size_t i = 0; i < item.children.size(); ++i) {
        const Item& child = item.children[i];
        ImVec2 child_size = sizes[i];
        if (child.kind == ItemKind::Grow) child_size.x += extra;
        draw_item(c, child, ImVec2(x, position.y + (size.y - child_size.y) * 0.5f), child_size, child_size.x, FLT_MAX);
        x += child_size.x + item.spacing * c.scale;
    }
}

// `wrap_width` is the zone's fixed content width, or FLT_MAX when the zone fits
// its content; `content_width` is the width separators, bars and rows span.
void draw_item(const Context& c, const Item& item, ImVec2 position, ImVec2 size, float content_width, float wrap_width) {
    switch (item.kind) {
    case ItemKind::Text:
        draw_text_run(c, item.style, position, item.text.c_str(), nullptr, wrap_width < FLT_MAX ? wrap_width : 0.0f);
        break;
    case ItemKind::Image: {
        const ImVec2 min = snap(position);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        const ImTextureRef texture = c.resources.texture(*item.image);
        if (item.rounding > 0.0f)
            c.list.AddImageRounded(texture, min, max, ImVec2(item.uv.x, item.uv.y), ImVec2(item.uv.z, item.uv.w),
                fade(c, item.tint), item.rounding * c.scale);
        else
            c.list.AddImage(texture, min, max, ImVec2(item.uv.x, item.uv.y), ImVec2(item.uv.z, item.uv.w), fade(c, item.tint));
        break;
    }
    case ItemKind::Row:
        draw_row(c, item, position, size, content_width);
        break;
    case ItemKind::Flow:
        draw_flow(c, item, position, wrap_width);
        break;
    case ItemKind::Spacer:
    case ItemKind::Grow:
        break;
    case ItemKind::Separator: {
        const float y = std::floor(position.y + size.y * 0.5f);
        c.list.AddLine(ImVec2(position.x, y), ImVec2(position.x + content_width, y), fade(c, item.color), size.y);
        break;
    }
    case ItemKind::Progress: {
        const ImVec2 min = snap(position);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        const float rounding = item.rounding * c.scale;
        c.list.AddRectFilled(min, max, fade(c, item.background), rounding);
        if (item.value > 0.0f)
            c.list.AddRectFilled(min, ImVec2(min.x + size.x * item.value, max.y), fade(c, item.color), rounding);
        break;
    }
    case ItemKind::Chip: {
        const ImVec2 min = snap(position);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        c.list.AddRectFilled(min, max, fade(c, item.background), item.rounding * c.scale);
        const ImVec2 text = text_size(c, item.style, item.text.c_str(), nullptr, 0.0f);
        draw_text_run(c, item.style, ImVec2(min.x + (size.x - text.x) * 0.5f, min.y + (size.y - text.y) * 0.5f),
            item.text.c_str(), nullptr, 0.0f);
        break;
    }
    case ItemKind::Icon:
        draw_icon(c, item, position, size);
        break;
    case ItemKind::Toggle: {
        const ImVec2 min = snap(position);
        const ImVec2 max(min.x + size.x, min.y + size.y);
        const float radius = size.y * 0.5f;
        c.list.AddRectFilled(min, max, fade(c, item.on ? item.on_color : item.color), radius);
        const float knob = radius - 2.0f * c.scale;
        const float x = item.on ? max.x - radius : min.x + radius;
        c.list.AddCircleFilled(ImVec2(x, min.y + radius), std::max(1.0f, knob), fade(c, item.knob), 16);
        break;
    }
    case ItemKind::Grid:
        draw_grid(c, item, position);
        break;
    case ItemKind::Voxels:
        draw_voxels(c, item, position, size);
        break;
    case ItemKind::World:
        draw_world(c, item);
        break;
    }
}

// Nine-slice: corners keep their size, edges stretch along one axis, the
// center stretches along both.
void draw_frame(const Context& c, const Frame& frame, ImVec2 min, ImVec2 max) {
    const Image& image = *frame.image;
    const ImTextureRef texture = c.resources.texture(*frame.image);
    const float iw = static_cast<float>(image.width), ih = static_cast<float>(image.height);
    const float k = frame.scale * c.scale;
    const float xs[4] = {min.x, min.x + frame.left * k, max.x - frame.right * k, max.x};
    const float ys[4] = {min.y, min.y + frame.top * k, max.y - frame.bottom * k, max.y};
    const float us[4] = {0.0f, frame.left / iw, 1.0f - frame.right / iw, 1.0f};
    const float vs[4] = {0.0f, frame.top / ih, 1.0f - frame.bottom / ih, 1.0f};
    const ImU32 tint = fade(c, frame.tint);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            if (xs[column + 1] <= xs[column] || ys[row + 1] <= ys[row]) continue;
            c.list.AddImage(texture, ImVec2(xs[column], ys[row]), ImVec2(xs[column + 1], ys[row + 1]),
                ImVec2(us[column], vs[row]), ImVec2(us[column + 1], vs[row + 1]), tint);
        }
}

void draw_zone(const Context& c, const Zone& zone, ImVec2 screen) {
    const ImVec2 padding(zone.padding.x * c.scale, zone.padding.y * c.scale);
    const float spacing = zone.spacing * c.scale;
    const float fixed_width = zone.size.x > 0.0f ? zone.size.x * c.scale - padding.x * 2.0f : FLT_MAX;

    std::vector<ImVec2> sizes;
    sizes.reserve(zone.items.size());
    ImVec2 content{0.0f, 0.0f};
    for (const Item& item : zone.items) {
        const ImVec2 size = measure(c, item, fixed_width);
        sizes.push_back(size);
        content.x = std::max(content.x, size.x);
        content.y += size.y;
    }
    if (zone.items.size() > 1) content.y += spacing * (zone.items.size() - 1);
    const float content_width = fixed_width < FLT_MAX ? std::max(0.0f, fixed_width) : content.x;
    const ImVec2 size(zone.size.x > 0.0f ? zone.size.x * c.scale : content_width + padding.x * 2.0f,
        zone.size.y > 0.0f ? zone.size.y * c.scale : content.y + padding.y * 2.0f);

    const ImVec2 min = snap(ImVec2(zone.anchor.x * screen.x - zone.pivot.x * size.x + zone.offset.x * c.scale,
        zone.anchor.y * screen.y - zone.pivot.y * size.y + zone.offset.y * c.scale));
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const float rounding = zone.rounding * c.scale;
    if (visible_color(zone.background)) c.list.AddRectFilled(min, max, fade(c, zone.background), rounding);
    if (zone.frame.image) draw_frame(c, zone.frame, min, max);
    if (zone.border_width > 0.0f && visible_color(zone.border))
        c.list.AddRect(min, max, fade(c, zone.border), rounding, std::max(1.0f, zone.border_width * c.scale));

    c.list.PushClipRect(min, max, true);
    float y = min.y + padding.y;
    const float x = min.x + padding.x;
    for (size_t i = 0; i < zone.items.size(); ++i) {
        const Item& item = zone.items[i];
        ImVec2 item_size = sizes[i];
        // A row with grow items spans the content width.
        if (item.kind == ItemKind::Row &&
                std::any_of(item.children.begin(), item.children.end(), [](const Item& child) { return child.kind == ItemKind::Grow; }))
            item_size.x = content_width;
        float item_x = x;
        if (item.align == Align::Center) item_x += (content_width - item_size.x) * 0.5f;
        if (item.align == Align::End) item_x += content_width - item_size.x;
        draw_item(c, item, ImVec2(item_x, y), item_size, content_width, fixed_width);
        y += item_size.y + spacing;
    }
    c.list.PopClipRect();
}

}  // namespace

void draw_scene(ImDrawList& list, const SceneSnapshot& scene, ImVec2 screen, const DrawResources& resources) {
    // Zones of world items only first: what lies in the game world stays
    // under the panels, as the game's own building cursor does.
    const auto world_only = [](const Zone& zone) {
        return !zone.items.empty() &&
            std::all_of(zone.items.begin(), zone.items.end(), [](const Item& item) { return item.kind == ItemKind::World; });
    };
    for (const bool world : {true, false})
        for (const auto& zone : scene.zones) {
            if (world_only(*zone) != world) continue;
            const Context c{list, resources, screen.y / zone->reference * scene.options.scale, scene.options.opacity,
                zone->owner + "/" + zone->id};
            draw_zone(c, *zone, screen);
        }
}

}  // namespace overlay
