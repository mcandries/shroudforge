// Layout and drawing of the zones. Items are stacked vertically inside a
// zone; a row places its items side by side, centered vertically, `grow`
// items taking the width left; a flow wraps words, chips and icons like a
// sentence. Sizes given by mods are reference pixels (the zone's reference
// screen height, 1080 by default) multiplied by the screen scale.

#include "draw_scene.h"
#include "local_camera.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace overlay {

void log_line(const char* format, ...);  // log.cpp

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

// The lines of a flow within `width` (its content, inside the padding); the
// lines after the first have `indent` less.
std::vector<FlowLine> flow_lines(const Context& c, const Item& flow, const std::vector<Token>& tokens, float width) {
    std::vector<FlowLine> lines;
    const float indent = flow.flow_indent * c.scale;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        const float gap = (lines.empty() || lines.back().count == 0) ? 0.0f : flow_gap(c, flow, tokens[i - 1], token);
        const float available = lines.size() > 1 && width < FLT_MAX ? width - indent : width;
        if (lines.empty() || (lines.back().count > 0 && lines.back().width + gap + token.size.x > available)) {
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
    const ImVec2 padding(flow.frame_padding.x * c.scale, flow.frame_padding.y * c.scale);
    const float inner = width < FLT_MAX ? std::max(1.0f, width - padding.x * 2.0f) : FLT_MAX;
    const auto tokens = flow_tokens(c, flow);
    const auto lines = flow_lines(c, flow, tokens, inner);
    ImVec2 size{0.0f, 0.0f};
    for (size_t i = 0; i < lines.size(); ++i) {
        size.x = std::max(size.x, lines[i].width + (i > 0 ? flow.flow_indent * c.scale : 0.0f));
        size.y += lines[i].height;
    }
    if (lines.size() > 1) size.y += flow.line_gap * c.scale * (lines.size() - 1);
    return ImVec2(size.x + padding.x * 2.0f, size.y + padding.y * 2.0f);
}

void draw_flow(const Context& c, const Item& flow, ImVec2 position, float width) {
    // Its frame over the whole width, then its lines inside the padding.
    const ImVec2 padding(flow.frame_padding.x * c.scale, flow.frame_padding.y * c.scale);
    if (visible_color(flow.background) || visible_color(flow.frame_border)) {
        const ImVec2 size = measure_flow(c, flow, width);
        const ImVec2 min = snap(position);
        const ImVec2 max(min.x + (width < FLT_MAX ? width : size.x), min.y + size.y);
        const float rounding = flow.rounding * c.scale;
        if (visible_color(flow.background)) c.list.AddRectFilled(min, max, fade(c, flow.background), rounding);
        if (visible_color(flow.frame_border))
            c.list.AddRect(min, max, fade(c, flow.frame_border), rounding, std::max(1.0f, c.scale));
    }
    position = ImVec2(position.x + padding.x, position.y + padding.y);
    if (width < FLT_MAX) width = std::max(1.0f, width - padding.x * 2.0f);
    const auto tokens = flow_tokens(c, flow);
    const auto lines = flow_lines(c, flow, tokens, width);
    float y = position.y;
    for (const FlowLine& line : lines) {
        float x = position.x + (&line != &lines.front() ? flow.flow_indent * c.scale : 0.0f);
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
        size.x += item.frame_padding.x * 2.0f * c.scale;
        size.y += item.frame_padding.y * 2.0f * c.scale;
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
    case ItemKind::Drum:
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
// Light of a face by its direction (as kNormals): the top brightest.
constexpr float kLight[6] = {0.82f, 0.82f, 0.55f, 1.0f, 0.68f, 0.68f};

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
    // With the GPU camera's matrix: camera space is its clip x, y and w (w the
    // distance along the gaze, in meters), focal_x and focal_y 1: the very
    // projection of the image. Its clip y goes up (30/10: taken as Vulkan's,
    // down, world items mirrored as the view pitched): the game flips its
    // viewport.
    bool matrix = false;
    double m[16]{};

    // Camera space of a world point: x right, y up, z forward (meters).
    void to_camera(const double point[3], double out[3]) const {
        if (matrix) {
            out[0] = m[0] * point[0] + m[1] * point[1] + m[2] * point[2] + m[3];
            out[1] = m[4] * point[0] + m[5] * point[1] + m[6] * point[2] + m[7];
            out[2] = m[12] * point[0] + m[13] * point[1] + m[14] * point[2] + m[15];
            return;
        }
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

bool world_view(WorldView& view, double lead = 0.0, LocalCamera* read = nullptr, const LocalCamera* drawn = nullptr,
    bool keep_jitter = false) {
    LocalCamera camera;
    if (!local_camera(camera, lead)) return false;
    if (read) *read = camera;
    // The camera the game drew the scene with, when known: the cursor stays
    // the one read now.
    if (drawn) {
        for (int i = 0; i < 3; ++i) camera.position[i] = drawn->position[i];
        for (int i = 0; i < 4; ++i) camera.orientation[i] = drawn->orientation[i];
        camera.fov_y = drawn->fov_y;
        camera.fov_x = drawn->fov_x;
        camera.has_matrix = drawn->has_matrix;
        for (int i = 0; i < 16; ++i) camera.matrix[i] = drawn->matrix[i];
    }
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
    if (camera.has_matrix) {
        view.matrix = true;
        for (int i = 0; i < 16; ++i) view.m[i] = camera.matrix[i];
        // The temporal anti-aliasing's jitter: a fraction of a pixel added
        // to clip x and y as w times a new offset each frame, rows 0 and 1
        // taking a part of the gaze row 3. The camera's right and up are
        // square to its gaze: what of row 3 they hold is the jitter.
        if (!keep_jitter) {
            const double gaze = view.m[12] * view.m[12] + view.m[13] * view.m[13] + view.m[14] * view.m[14];
            for (int row = 0; row < 2 && gaze > 0.0; ++row) {
                double* r = view.m + row * 4;
                const double k = (r[0] * view.m[12] + r[1] * view.m[13] + r[2] * view.m[14]) / gaze;
                for (int i = 0; i < 4; ++i) r[i] -= k * view.m[12 + i];
            }
        }
        view.focal_x = 1.0;
        view.focal_y = 1.0;
        return true;
    }
    view.focal_y = std::tan(camera.fov_y * 0.5);
    view.focal_x = camera.fov_x > 0.0 ? std::tan(camera.fov_x * 0.5) : view.focal_y * view.screen.x / view.screen.y;
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

// A color made lighter, for what lies behind filled cells of the world.
ImU32 lighter(ImU32 color) {
    const auto channel = [&](int shift) {
        const float value = static_cast<float>((color >> shift) & 0xFF);
        return static_cast<ImU32>(value + (255.0f - value) * 0.6f + 0.5f) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
        (color & IM_COL32_A_MASK);
}

// Whether a world point lies behind filled cells of `solids` seen from `eye`:
// a walk, cell by cell, along the part of the segment from the point to the
// eye inside the box (at most its three sides added).
bool behind_solids(const WorldSolids& solids, const double point[3], const double eye[3]) {
    const int size[3] = {solids.sx, solids.sy, solids.sz};
    double start[3], d[3];
    double t0 = 0.0, t1 = 1.0;
    for (int i = 0; i < 3; ++i) {
        start[i] = (point[i] - solids.origin[i]) / solids.cell;
        d[i] = (eye[i] - solids.origin[i]) / solids.cell - start[i];
        if (std::fabs(d[i]) < 1e-9) {
            if (start[i] < 0.0 || start[i] >= size[i]) return false;
            continue;
        }
        double a = -start[i] / d[i], b = (size[i] - start[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 >= t1) return false;
    int cell[3], step[3];
    double next[3], delta[3];
    for (int i = 0; i < 3; ++i) {
        const double at = start[i] + d[i] * t0;
        cell[i] = std::clamp(static_cast<int>(std::floor(at)), 0, size[i] - 1);
        if (std::fabs(d[i]) < 1e-9) {
            step[i] = 0;
            next[i] = delta[i] = DBL_MAX;
        } else {
            step[i] = d[i] > 0.0 ? 1 : -1;
            const double border = d[i] > 0.0 ? cell[i] + 1.0 : static_cast<double>(cell[i]);
            next[i] = t0 + (border - at) / d[i];
            delta[i] = std::fabs(1.0 / d[i]);
        }
    }
    while (true) {
        if (solids.filled[(static_cast<size_t>(cell[2]) * size[1] + cell[1]) * size[0] + cell[0]]) return true;
        const int axis = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
        if (next[axis] >= t1) return false;
        cell[axis] += step[axis];
        if (cell[axis] < 0 || cell[axis] >= size[axis]) return false;
        next[axis] += delta[axis];
    }
}

// Whether a point (meters) is inside the box of some solids.
bool in_solids(const WorldSolids& solids, const double point[3]) {
    const int size[3] = {solids.sx, solids.sy, solids.sz};
    for (int i = 0; i < 3; ++i) {
        const double at = (point[i] - solids.origin[i]) / solids.cell;
        if (at < 0.0 || at > size[i]) return false;
    }
    return true;
}

// only_in_solids: whether the cell of a face is inside the solids' box (the
// eye being in it too, the walk from the face to the eye stays inside).
bool face_in_solids(const Item& item, const VoxelFace& face) {
    if (!item.world_only_in_solids || !item.world_solids) return true;
    const double s = item.world_cells_size;
    const double middle[3] = {item.world_cells_origin[0] + (face.x + 0.5) * s,
        item.world_cells_origin[1] + (face.y + 0.5) * s, item.world_cells_origin[2] + (face.z + 0.5) * s};
    return in_solids(*item.world_solids, middle);
}

// Whether a world point is behind the item's `solids` (or its own cells with
// `hide_own`), from a hair nearer the eye: a point on the side of a filled
// cell is not behind it.
bool behind_solids(const Item& item, const WorldView& view, const double point[3]) {
    if (!item.world_solids && !item.world_own_solids) return false;
    double d[3], length = 0.0;
    for (int i = 0; i < 3; ++i) {
        d[i] = view.eye[i] - point[i];
        length += d[i] * d[i];
    }
    length = std::sqrt(length);
    if (length < 1e-6) return false;
    const double cell = item.world_solids ? item.world_solids->cell : item.world_own_solids->cell;
    const double hair = 0.02 * cell / length;
    const double from[3] = {point[0] + d[0] * hair, point[1] + d[1] * hair, point[2] + d[2] * hair};
    return (item.world_solids && behind_solids(*item.world_solids, from, view.eye)) ||
        (item.world_own_solids && behind_solids(*item.world_own_solids, from, view.eye));
}

// Which faces of a world mesh are hidden from the eye (hide_hidden): every
// face is tested, not only the first kMaxHiddenTests, so the results are kept
// per mesh and occluders, and tested again after the eye moved, a part at
// each frame within kHiddenBudget for all of them; meanwhile the faces keep
// their last result (those not tested yet show).
struct HiddenFaces {
    const VoxelMesh* mesh = nullptr;
    const WorldSolids* solids = nullptr;
    const WorldSolids* own = nullptr;
    std::shared_ptr<const VoxelMesh> keep_mesh;
    std::shared_ptr<const WorldSolids> keep_solids, keep_own;
    double pass_eye[3]{};
    bool started = false;
    size_t next = 0;
    int last_frame = 0;
    // 0 shown, 1 hidden, 2 not tested yet, 3 in part: `mask` says which of
    // its 4 x 4 pieces are hidden (bit j * 4 + i, i along the face's first
    // axis after its normal's).
    std::vector<std::uint8_t> hidden;
    std::vector<std::uint16_t> mask;
};
constexpr auto kHiddenBudget = std::chrono::microseconds(2000);
constexpr double kHiddenEyeMove = 0.02;  // Meters the eye moves before a new pass.

HiddenFaces& hidden_faces_for(const Item& item, const std::shared_ptr<const VoxelMesh>& mesh) {
    static std::vector<HiddenFaces> cache;
    const int frame = ImGui::GetFrameCount();
    for (HiddenFaces& entry : cache)
        if (entry.mesh == mesh.get() && entry.solids == item.world_solids.get() &&
            entry.own == item.world_own_solids.get()) {
            entry.last_frame = frame;
            return entry;
        }
    // Entries unused for some 60 frames give their place.
    std::erase_if(cache, [&](const HiddenFaces& entry) { return frame - entry.last_frame > 60; });
    if (cache.size() >= 32) cache.erase(cache.begin());
    HiddenFaces entry;
    entry.mesh = mesh.get();
    entry.solids = item.world_solids.get();
    entry.own = item.world_own_solids.get();
    entry.keep_mesh = mesh;
    entry.keep_solids = item.world_solids;
    entry.keep_own = item.world_own_solids;
    entry.hidden.assign(mesh->faces.size(), 2);
    entry.mask.assign(mesh->faces.size(), 0);
    entry.last_frame = frame;
    cache.push_back(std::move(entry));
    return cache.back();
}

// Tests the faces of `hidden` from where its pass stopped, within what is left
// of this frame's budget.
void test_hidden_faces(const Item& item, const VoxelMesh& mesh, const WorldView& view, HiddenFaces& hidden) {
    static int budget_frame = -1;
    static std::chrono::steady_clock::time_point budget_end;
    const int frame = ImGui::GetFrameCount();
    if (budget_frame != frame) {
        budget_frame = frame;
        budget_end = std::chrono::steady_clock::now() + kHiddenBudget;
    }
    double moved = 0.0;
    for (int i = 0; i < 3; ++i) moved += (view.eye[i] - hidden.pass_eye[i]) * (view.eye[i] - hidden.pass_eye[i]);
    if (!hidden.started || moved > kHiddenEyeMove * kHiddenEyeMove) {
        for (int i = 0; i < 3; ++i) hidden.pass_eye[i] = view.eye[i];
        hidden.started = true;
        hidden.next = 0;
    }
    const double s = item.world_cells_size;
    for (size_t tested = 0; hidden.next < mesh.faces.size(); ++hidden.next, ++tested) {
        if (tested % 32 == 0 && std::chrono::steady_clock::now() >= budget_end) return;
        const VoxelFace& face = mesh.faces[hidden.next];
        if (face.inner || !face_in_solids(item, face)) continue;
        const int axis = face.direction / 2;
        const bool positive = face.direction % 2 == 1;
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const double base[3] = {item.world_cells_origin[0] + face.x * s, item.world_cells_origin[1] + face.y * s,
            item.world_cells_origin[2] + face.z * s};
        const double plane = base[axis] + (positive ? s : 0.0);
        // Faces turned away are not drawn: nothing to test.
        if ((view.eye[axis] - plane) * (positive ? 1.0 : -1.0) <= 0.0) continue;
        const auto behind_at = [&](double fu, double fv) {
            double point[3];
            point[axis] = plane;
            point[u] = base[u] + fu * s;
            point[v] = base[v] + fv * s;
            return behind_solids(item, view, point);
        };
        // Its middle and its corners (a little inside): all alike, the face
        // is shown or hidden whole; else an occluder's border crosses it,
        // and each of its 4 x 4 pieces is tested at its middle.
        const bool middle = behind_at(0.5, 0.5);
        constexpr double kIn = 0.05;
        const bool alike = behind_at(kIn, kIn) == middle && behind_at(1.0 - kIn, kIn) == middle &&
            behind_at(1.0 - kIn, 1.0 - kIn) == middle && behind_at(kIn, 1.0 - kIn) == middle;
        if (alike) {
            hidden.hidden[hidden.next] = middle ? 1 : 0;
            continue;
        }
        std::uint16_t mask = 0;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                if (behind_at((i + 0.5) / 4.0, (j + 0.5) / 4.0)) mask |= static_cast<std::uint16_t>(1u << (j * 4 + i));
        hidden.mask[hidden.next] = mask;
        hidden.hidden[hidden.next] = mask == 0 ? 0 : mask == 0xFFFF ? 1 : 3;
    }
}

// A line cut into dashes on the screen.
void dashed_line(const Context& c, ImVec2 a, ImVec2 b, ImU32 color, float width) {
    const float dash = 6.0f * c.scale, gap = 4.0f * c.scale;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.5f) return;
    for (float t = 0.0f; t < length; t += dash + gap) {
        const float e = std::min(t + dash, length);
        c.list.AddLine(ImVec2(a.x + dx * t / length, a.y + dy * t / length),
            ImVec2(a.x + dx * e / length, a.y + dy * e / length), color, width);
    }
}

// The characters of a mesh's outer faces, kept for the last few meshes.
const std::array<bool, 128>& characters_of(const std::shared_ptr<const VoxelMesh>& mesh) {
    struct Entry {
        std::weak_ptr<const VoxelMesh> mesh;
        const VoxelMesh* raw = nullptr;
        std::array<bool, 128> present{};
    };
    static std::vector<Entry> entries;
    for (const Entry& entry : entries)
        if (entry.raw == mesh.get() && entry.mesh.lock() == mesh) return entry.present;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return e.mesh.expired(); }),
        entries.end());
    if (entries.size() >= 16) entries.erase(entries.begin());
    Entry entry;
    entry.mesh = mesh;
    entry.raw = mesh.get();
    for (const VoxelFace& face : mesh->faces)
        if (!face.inner && face.ch < 128) entry.present[face.ch] = true;
    entries.push_back(entry);
    return entries.back().present;
}

// A box of cells in the world (`cells` of a world item): the faces turned to
// the camera filled with their cell's color, lit by their direction, the
// farthest first (at most kMaxWorldFaces); then over all of them their edges,
// in the color more opaque: each face's, or only the creases when smooth. A
// face behind the item's solids is drawn lighter (the first kMaxHiddenTests
// faces tested).
constexpr size_t kMaxWorldFaces = 30000;
constexpr size_t kMaxHiddenTests = 4000;
void draw_world_cells(const Context& c, const Item& item, const std::shared_ptr<const VoxelMesh>& shared_mesh,
    const WorldView& view) {
    constexpr double kNear = 0.05;
    const VoxelMesh& mesh = *shared_mesh;
    const double s = item.world_cells_size;
    struct Shown {
        double depth;
        double corners[4][3];  // Camera space.
        double world[4][3];    // Meters.
        ImVec2 points[8];
        float point_depths[8];  // As the game's: near / distance.
        double point_world[8][3];  // Meters.
        int count;
        ImU32 fill, edge;
        std::uint8_t edges;
        bool dashed;  // Hidden, shown by its edges dashed only.
        double margin;  // more margin against the game's depth (the wire view's offsets), meters
        bool back;      // turned away from the eye: only its creases with another turned away (back_edges)
        int direction;  // the face's (axis * 2 + positive)
    };
    // Against the game's depth (game-depth.on): every face sent to the
    // renderer, which leaves out, pixel by pixel, what the game's surface
    // hides; no test here.
    // The wire view goes there whether hidden faces are hidden or not
    // (30/10: drawn by ImGui, its edges over everything and its offsets as
    // geometry, a block removed looked two deep).
    const bool wire_style = item.world_cells_smooth && !item.world_own_solids;
    WorldDepthDraw* gpu = (item.world_hide_hidden || wire_style) && c.resources.world_depth ? c.resources.world_depth()
                                                                                           : nullptr;
    if (gpu && item.world_depth_margin >= 0.0f) gpu->margin = item.world_depth_margin;
    if (gpu && item.world_own_solids) gpu->solid = true;
    if (gpu && wire_style) gpu->wire = true;
    if (gpu && wire_style && !item.world_hide_hidden) gpu->show_hidden = true;
    if (gpu && item.world_dash_opacity >= 0.0f) gpu->dash_alpha = item.world_dash_opacity;
    // The solid style textured: the first item's textures for all.
    const bool textured = gpu && item.world_own_solids && item.world_textured;
    if (textured && !gpu->textured) {
        gpu->textures = item.world_textures;
        gpu->textured = true;
        gpu->texture_light = item.world_texture_light;
        gpu->relief = item.world_relief;
        for (int i = 0; i < 3; ++i) gpu->texture_origin[i] = std::floor(item.world_cells_origin[i] / 64.0) * 64.0;
    }
    if (gpu && wire_style && item.world_back_edges > 0.0f) gpu->back_alpha = item.world_back_edges;
    // Remove mode: what hides an edge is told by the world's blocks around.
    const bool through_blocks = item.world_hidden_dashed || item.world_through_blocks;
    if (gpu && wire_style && item.world_hide_hidden && through_blocks && item.world_solids && !gpu->blocks) {
        gpu->blocks = item.world_solids;
        for (int i = 0; i < 3; ++i)
            gpu->blocks_eye[i] = (view.eye[i] - item.world_solids->origin[i]) / item.world_solids->cell;
    }
    if (gpu && gpu->blocks && through_blocks && std::fabs(item.world_cells_size - gpu->blocks->cell) < 1e-6) {
        WorldDepthDraw::BlocksPart part;
        part.mesh = shared_mesh;
        for (int i = 0; i < 3; ++i)
            part.at[i] = static_cast<int>(std::lround((item.world_cells_origin[i] - gpu->blocks->origin[i]) / gpu->blocks->cell));
        gpu->blocks_build.push_back(std::move(part));
    }
    // A world point in cells of the blocks, times `depth` (as the shader interpolates it).
    const auto in_blocks = [&](const double world[3], float depth, float out[3]) {
        for (int i = 0; i < 3; ++i)
            out[i] = gpu && gpu->blocks
                ? static_cast<float>((world[i] - gpu->blocks->origin[i]) / gpu->blocks->cell * depth)
                : 0.0f;
    };
    if (gpu) {
        // Something standing less than the largest offset (and 10 cm) before
        // our faces is a block of the game the build sits in.
        double largest = item.world_face_offset;
        for (const auto& entry : item.world_face_offsets)
            for (float each : entry.second) largest = std::max(largest, static_cast<double>(each));
        gpu->own_tolerance = std::max(gpu->own_tolerance, largest + 0.1);
        // The game's blocks drawn past their cells, from a cell's box: one and
        // a half the farthest (a corner's bumps, diagonally) and 5 cm.
        gpu->blocks_relief = std::max(gpu->blocks_relief, 1.5 * largest + 0.05);
        gpu->focal_pixels = view.matrix
            ? view.screen.y * 0.5 * std::sqrt(view.m[4] * view.m[4] + view.m[5] * view.m[5] + view.m[6] * view.m[6])
            : view.screen.y * 0.5 / view.focal_y;
    }
    // 30/10: the matrix's near (6.5 mm) made the build look farther than the
    // game's depth says: hidden behind what stands in front, more with the
    // distance. 1 cm by default; the matrix's when an item asks.
    if (gpu && item.world_depth_near > 0.0f) gpu->near_distance = item.world_depth_near;
    if (gpu && item.world_depth_near == 0.0f && gpu->matrix_near > 0.0) gpu->near_distance = gpu->matrix_near;
    // Hide hidden faces: their results kept per mesh, tested a part per frame.
    HiddenFaces* hidden = nullptr;
    if (!gpu && item.world_hide_hidden && (item.world_solids || item.world_own_solids)) {
        hidden = &hidden_faces_for(item, shared_mesh);
        test_hidden_faces(item, mesh, view, *hidden);
    }
    // only_in_solids, the eye outside the solids' box: no cell is drawn, the
    // world between them and the eye not known.
    if (item.world_only_in_solids && item.world_solids && !in_solids(*item.world_solids, view.eye)) return;
    // face_offsets_largest: the largest offsets of the characters drawn.
    bool use_largest = false;
    std::array<double, 3> largest{0.0, 0.0, 0.0};
    if (item.world_face_offsets_largest) {
        const auto& present = characters_of(shared_mesh);
        for (int ch = 0; ch < 128; ++ch) {
            if (!present[ch]) continue;
            std::array<double, 3> own{item.world_face_offset, item.world_face_offset, item.world_face_offset};
            for (const auto& [key, each] : item.world_face_offsets)
                if (key == ch) own = {each[0], each[1], each[2]};
            for (int i = 0; i < 3; ++i) largest[i] = std::max(largest[i], own[i]);
            use_largest = true;
        }
    }
    std::vector<Shown> shown;
    shown.reserve(std::min(mesh.faces.size(), kMaxWorldFaces));
    static constexpr int kSquare[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    size_t tests = 0;
    for (size_t index = 0; index < mesh.faces.size(); ++index) {
        const VoxelFace& face = mesh.faces[index];
        if (face.inner || !face_in_solids(item, face)) continue;
        if (hidden && hidden->hidden[index] == 1 && !item.world_hidden_dashed) continue;
        if (shown.size() >= kMaxWorldFaces) break;
        const int axis = face.direction / 2;
        const bool positive = face.direction % 2 == 1;
        const double base[3] = {item.world_cells_origin[0] + face.x * s, item.world_cells_origin[1] + face.y * s,
            item.world_cells_origin[2] + face.z * s};
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        // face_offset: pushed out, its edges `grow` (u0, u1, v0, v1) longer at
        // an outer corner, shorter at an inner one, as the game draws its
        // blocks past their cells (the user's reading, 30/10).
        // Its offset toward a direction (axis * 2 + positive): the sides', the
        // top's (+y) or the bottom's (-y) of its character, else the item's.
        const auto offset_toward = [&](int direction) -> double {
            if (use_largest) return direction == 3 ? largest[1] : direction == 2 ? largest[2] : largest[0];
            for (const auto& [ch, own] : item.world_face_offsets)
                if (ch == face.ch) return direction == 3 ? own[1] : direction == 2 ? own[2] : own[0];
            return item.world_face_offset;
        };
        // The wire view keeps the cells on the grid (its edges must not
        // cross): the offset only lets the face show that far behind the
        // game's surface, as a margin.
        const bool offset_as_margin = gpu && gpu->wire;
        const double face_margin = offset_as_margin ? offset_toward(face.direction) : 0.0;
        const double offset = offset_as_margin ? 0.0 : offset_toward(face.direction);
        bool any_offset = !offset_as_margin && offset > 0.0;
        for (int direction = 0; direction < 6 && !any_offset && !offset_as_margin; ++direction)
            any_offset = offset_toward(direction) > 0.0;
        double grow[4] = {0.0, 0.0, 0.0, 0.0};
        if (any_offset) {
            const auto drawn = [&](int p[3]) {
                if (p[0] < 0 || p[1] < 0 || p[2] < 0 || p[0] >= mesh.sx || p[1] >= mesh.sy || p[2] >= mesh.sz) return false;
                return mesh.drawn[(static_cast<size_t>(p[2]) * mesh.sy + p[1]) * mesh.sx + p[0]] != 0;
            };
            // Edge k toward -v, +u, +v, -u; grow index u0, u1, v0, v1.
            static constexpr int kEdgeSide[4][2] = {{1, -1}, {0, 1}, {1, 1}, {0, -1}};
            static constexpr int kGrow[4] = {2, 1, 3, 0};
            for (int k = 0; k < 4; ++k) {
                int beyond[3] = {face.x, face.y, face.z};
                beyond[kEdgeSide[k][0] == 0 ? u : v] += kEdgeSide[k][1];
                int front[3] = {beyond[0], beyond[1], beyond[2]};
                front[axis] += positive ? 1 : -1;
                // Outer corner: longer by the offset of the face it meets
                // there (toward the edge); inner corner: shorter by the one
                // of the face coming back over it; a cell touching only along
                // this edge (diagonal): unchanged, the two would cross.
                const int edge_axis = kEdgeSide[k][0] == 0 ? u : v;
                const int toward = edge_axis * 2 + (kEdgeSide[k][1] > 0 ? 1 : 0);
                const int back = edge_axis * 2 + (kEdgeSide[k][1] > 0 ? 0 : 1);
                grow[kGrow[k]] = drawn(front) ? (drawn(beyond) ? -offset_toward(back) : 0.0)
                                              : drawn(beyond) ? 0.0 : offset_toward(toward);
            }
        }
        const double plane = base[axis] + (positive ? s + offset : -offset);
        // Only the faces turned to the camera; with back_edges (the wire
        // view), those turned away too, for their creases with another
        // turned away.
        const bool back = (view.eye[axis] - plane) * (positive ? 1.0 : -1.0) <= 0.0;
        const bool back_edges = gpu && gpu->wire && item.world_back_edges > 0.0f;
        if (back && !back_edges) continue;
        std::uint8_t back_creases = 0xF;
        if (back) {
            // The other face at each edge: the cell's own toward the edge (an
            // outer corner), or the one coming back over it (an inner one);
            // its plane is the edge's.
            const auto drawn_at = [&](const int p[3]) {
                if (p[0] < 0 || p[1] < 0 || p[2] < 0 || p[0] >= mesh.sx || p[1] >= mesh.sy || p[2] >= mesh.sz) return false;
                return mesh.drawn[(static_cast<size_t>(p[2]) * mesh.sy + p[1]) * mesh.sx + p[0]] != 0;
            };
            static constexpr int kSide[4][2] = {{1, -1}, {0, 1}, {1, 1}, {0, -1}};
            back_creases = 0;
            for (int k = 0; k < 4; ++k) {
                const int edge_axis = kSide[k][0] == 0 ? u : v;
                const int toward = kSide[k][1];
                int front[3] = {face.x, face.y, face.z};
                front[edge_axis] += toward;
                front[axis] += positive ? 1 : -1;
                const double at = base[edge_axis] + (toward > 0 ? s : 0.0);
                const double normal = drawn_at(front) ? -toward : toward;
                if ((view.eye[edge_axis] - at) * normal <= 0.0) back_creases |= static_cast<std::uint8_t>(1 << k);
            }
            if (!(back_creases & face.creases)) continue;
        }
        double in[4][3], at_world[4][3];
        double centre[3] = {0.0, 0.0, 0.0};
        for (int k = 0; k < 4; ++k) {
            double* corner = at_world[k];
            corner[axis] = plane;
            corner[u] = kSquare[k][0] ? base[u] + s + grow[1] : base[u] - grow[0];
            corner[v] = kSquare[k][1] ? base[v] + s + grow[3] : base[v] - grow[2];
            view.to_camera(corner, in[k]);
            for (int i = 0; i < 3; ++i) centre[i] += in[k][i] * 0.25;
        }
        ImU32 color = item.cell_colors[face.ch];
        if (!gpu && !hidden && item.world_solids && tests < kMaxHiddenTests) {
            ++tests;
            double middle[3];
            for (int i = 0; i < 3; ++i) middle[i] = base[i] + 0.5 * s;
            middle[axis] = plane;
            if (behind_solids(item, view, middle)) color = lighter(color);
        }
        // Textured: the texture's color, lit by the face's direction.
        const ImU32 fill = textured ? fade(c, shade(IM_COL32(255, 255, 255, 255), kLight[face.direction], 1.0f))
                                    : fade(c, shade(color, kLight[face.direction], 1.0f));
        const ImU32 edge = fade(c, (color & ~IM_COL32_A_MASK) |
            IM_COL32(0, 0, 0, static_cast<int>(item.world_cells_edge_opacity * 255.0f + 0.5f)));
        // A hidden part shown dashed: its edges in its color, nearly opaque,
        // whatever the edge opacity (none in the solid style).
        const int dash_alpha = item.world_dash_opacity >= 0.0f ? static_cast<int>(item.world_dash_opacity * 255.0f + 0.5f) : 220;
        const ImU32 dashed_edge = fade(c, (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, dash_alpha));
        const std::uint8_t face_edges = item.world_cells_smooth ? face.creases : std::uint8_t{0xF};
        // A piece of the face, u0..u1 x v0..v1 of its sides: its corners in
        // camera space, cut at the near plane; its edges only those on the
        // face's border.
        const auto add_piece = [&](double u0, double u1, double v0, double v1, bool piece_hidden) {
            Shown entry{};
            const double at[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i) {
                    entry.corners[k][i] = in[0][i] + (in[1][i] - in[0][i]) * at[k][0] + (in[3][i] - in[0][i]) * at[k][1];
                    entry.world[k][i] = at_world[0][i] + (at_world[1][i] - at_world[0][i]) * at[k][0] +
                        (at_world[3][i] - at_world[0][i]) * at[k][1];
                }
            const double near_distance = gpu ? gpu->near_distance : 1.0;
            for (int k = 0; k < 4; ++k) {
                const double* a = entry.corners[k];
                const double* b = entry.corners[(k + 1) % 4];
                const double* wa = entry.world[k];
                const double* wb = entry.world[(k + 1) % 4];
                if (a[2] >= kNear) {
                    entry.point_depths[entry.count] = static_cast<float>(near_distance / a[2]);
                    std::memcpy(entry.point_world[entry.count], wa, sizeof entry.point_world[0]);
                    entry.points[entry.count++] = view.to_screen(a);
                }
                if ((a[2] >= kNear) != (b[2] >= kNear)) {
                    const double t = (kNear - a[2]) / (b[2] - a[2]);
                    const double cut[3] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, kNear};
                    entry.point_depths[entry.count] = static_cast<float>(near_distance / kNear);
                    for (int i = 0; i < 3; ++i) entry.point_world[entry.count][i] = wa[i] + (wb[i] - wa[i]) * t;
                    entry.points[entry.count++] = view.to_screen(cut);
                }
            }
            if (entry.count < 3) return;
            const std::uint8_t border = (v0 == 0.0 ? 1 : 0) | (u1 == 1.0 ? 2 : 0) | (v1 == 1.0 ? 4 : 0) |
                (u0 == 0.0 ? 8 : 0);
            entry.depth = centre[2];
            entry.dashed = piece_hidden;
            entry.fill = fill;
            // Against the game's depth, a hidden part of an edge shows dashed
            // in this color (the renderer decides which part).
            entry.edge = piece_hidden ? dashed_edge : edge;
            entry.edges = static_cast<std::uint8_t>(border & (piece_hidden ? 0xF : face_edges) & back_creases);
            entry.margin = face_margin;
            entry.back = back;
            entry.direction = face.direction;
            shown.push_back(entry);
        };
        const std::uint8_t state = hidden ? hidden->hidden[index] : 0;
        if (state != 3) {
            add_piece(0.0, 1.0, 0.0, 1.0, state == 1);
            continue;
        }
        // In part: each row of pieces cut into runs alike, the hidden ones
        // left out (or dashed).
        const std::uint16_t mask = hidden->mask[index];
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4;) {
                const bool piece_hidden = (mask >> (j * 4 + i)) & 1;
                int end = i + 1;
                while (end < 4 && (((mask >> (j * 4 + end)) & 1) != 0) == piece_hidden) ++end;
                if (!piece_hidden || item.world_hidden_dashed)
                    add_piece(i / 4.0, end / 4.0, j / 4.0, (j + 1) / 4.0, piece_hidden);
                i = end;
            }
    }
    std::sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.depth > b.depth; });
    if (gpu) {
        // The fills, then the edges as thin quads, all with their depth.
        for (const Shown& entry : shown) {
            if (entry.back) continue;  // creases only
            const auto first = static_cast<std::uint32_t>(gpu->vertices.size());
            // Textured (the solid style): which texture and plane, the point in
            // meters from the texture origin.
            // 1 and 2 the sides facing x and z, 3 the top, 4 the bottom; a
            // side facing the negative way half more (its normal's sign).
            const int axis = entry.direction / 2;
            const float tex = !gpu->textured || !gpu->solid ? 0.0f
                : axis == 0 ? (entry.direction == 0 ? 1.5f : 1.0f) : axis == 2 ? (entry.direction == 4 ? 2.5f : 2.0f)
                : entry.direction == 3 ? 3.0f : 4.0f;
            for (int k = 0; k < entry.count; ++k) {
                float meters[3] = {0.0f, 0.0f, 0.0f};
                if (tex > 0.0f)
                    for (int i = 0; i < 3; ++i)
                        meters[i] = static_cast<float>((entry.point_world[k][i] - gpu->texture_origin[i]) * entry.point_depths[k]);
                gpu->vertices.push_back({entry.points[k].x, entry.points[k].y, entry.point_depths[k], -1.0f,
                    static_cast<float>(entry.margin / gpu->near_distance), entry.fill, {meters[0], meters[1], meters[2]},
                    1.0f, tex});
            }
            for (int k = 1; k + 1 < entry.count; ++k) gpu->indices.insert(gpu->indices.end(), {first, first + k, first + k + 1});
        }
        gpu->face_indices = gpu->indices.size();
        const float half = std::max(0.75f, 0.75f * c.scale);
        const float period = 10.0f * c.scale;  // 6 on, 4 off, as dashed_line
        // Edges keep 5 cm more: where a cell rests on the game's surface its
        // outline stays (30/10: with no margin, cubes looked open).
        const auto edge_extra = static_cast<float>(0.05 / gpu->near_distance);
        // Solid: edges a hair nearer than their faces, kept over them.
        const float edge_nearer = gpu->solid ? 1.001f : 1.0f;
        // Each edge once: two faces seen share theirs (31/10: drawn twice,
        // the creases between them more opaque than the outline, their
        // dashes out of step). Known by its ends, to the millimeter.
        std::unordered_set<std::uint64_t> drawn_edges;
        drawn_edges.reserve(shown.size() * 2);
        const auto edge_key = [](const double* p, const double* q) {
            long long ends[2][3];
            for (int i = 0; i < 3; ++i) {
                ends[0][i] = std::llround(p[i] * 1000.0);
                ends[1][i] = std::llround(q[i] * 1000.0);
            }
            if (std::lexicographical_compare(ends[1], ends[1] + 3, ends[0], ends[0] + 3)) std::swap(ends[0], ends[1]);
            std::uint64_t key = 1469598103934665603ull;
            for (const auto& end : ends)
                for (long long value : end) key = (key ^ static_cast<std::uint64_t>(value)) * 1099511628211ull;
            return key;
        };
        for (const Shown& entry : shown)
            for (int k = 0; k < 4; ++k) {
                if (!(entry.edges & (1 << k))) continue;
                if (!drawn_edges.insert(edge_key(entry.world[k], entry.world[(k + 1) % 4])).second) continue;
                // Unseen edges (the solid style) only matter dashed; the solid
                // style shows none (30/10: in Remove mode, dashes over the
                // terrain and the player).
                const bool hidden_dashed = item.world_hidden_dashed && !gpu->solid;
                if (!hidden_dashed && !(entry.edge & IM_COL32_A_MASK)) continue;
                double a[3], b[3], wa[3], wb[3];
                std::memcpy(a, entry.corners[k], sizeof a);
                std::memcpy(b, entry.corners[(k + 1) % 4], sizeof b);
                std::memcpy(wa, entry.world[k], sizeof wa);
                std::memcpy(wb, entry.world[(k + 1) % 4], sizeof wb);
                if (a[2] < kNear && b[2] < kNear) continue;
                if (a[2] < kNear || b[2] < kNear) {
                    double* behind = a[2] < kNear ? a : b;
                    const double* front = a[2] < kNear ? b : a;
                    double* world_behind = a[2] < kNear ? wa : wb;
                    const double* world_front = a[2] < kNear ? wb : wa;
                    const double t = (kNear - front[2]) / (behind[2] - front[2]);
                    for (int i = 0; i < 3; ++i) {
                        behind[i] = front[i] + (behind[i] - front[i]) * t;
                        world_behind[i] = world_front[i] + (world_behind[i] - world_front[i]) * t;
                    }
                }
                const ImVec2 pa = view.to_screen(a), pb = view.to_screen(b);
                const float dx = pb.x - pa.x, dy = pb.y - pa.y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length < 0.5f) continue;
                const float nx = -dy / length * half, ny = dx / length * half;
                const auto da = static_cast<float>(gpu->near_distance / a[2]) * edge_nearer;
                const auto db = static_cast<float>(gpu->near_distance / b[2]) * edge_nearer;
                // Dashes only where the edge is hidden: in Remove mode, and in
                // the wire view (where the build itself hides it).
                const bool dashes = hidden_dashed || gpu->wire;
                const float ua = dashes ? 0.0f : -1.0f;
                const float ub = dashes ? length / period : -1.0f;
                const auto first = static_cast<std::uint32_t>(gpu->vertices.size());
                const auto extra = edge_extra + static_cast<float>(entry.margin / gpu->near_distance);
                float ca[3], cb[3];
                in_blocks(wa, da, ca);
                in_blocks(wb, db, cb);
                // A crease turned away is behind the build's own faces: the
                // shader dashes it at back_edges.
                const float fade = 1.0f;
                gpu->vertices.push_back({pa.x + nx, pa.y + ny, da, ua, extra, entry.edge, {ca[0], ca[1], ca[2]}, fade});
                gpu->vertices.push_back({pb.x + nx, pb.y + ny, db, ub, extra, entry.edge, {cb[0], cb[1], cb[2]}, fade});
                gpu->vertices.push_back({pb.x - nx, pb.y - ny, db, ub, extra, entry.edge, {cb[0], cb[1], cb[2]}, fade});
                gpu->vertices.push_back({pa.x - nx, pa.y - ny, da, ua, extra, entry.edge, {ca[0], ca[1], ca[2]}, fade});
                gpu->indices.insert(gpu->indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
            }
        return;
    }
    // The fills without anti-aliased borders, whose overlaps would draw the
    // seams between faces; the edges after, so that no nearer fill covers
    // part of one.
    const ImDrawListFlags flags = c.list.Flags;
    c.list.Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (const Shown& entry : shown)
        if (!entry.dashed) c.list.AddConvexPolyFilled(entry.points, entry.count, entry.fill);
    c.list.Flags = flags;
    for (const Shown& entry : shown)
        for (int k = 0; k < 4; ++k) {
            if (!(entry.edges & (1 << k))) continue;
            double a[3], b[3];
            std::memcpy(a, entry.corners[k], sizeof a);
            std::memcpy(b, entry.corners[(k + 1) % 4], sizeof b);
            if (a[2] < kNear && b[2] < kNear) continue;
            if (a[2] < kNear || b[2] < kNear) {
                double* behind = a[2] < kNear ? a : b;
                const double* front = a[2] < kNear ? b : a;
                const double t = (kNear - front[2]) / (behind[2] - front[2]);
                for (int i = 0; i < 3; ++i) behind[i] = front[i] + (behind[i] - front[i]) * t;
            }
            if (entry.dashed)
                dashed_line(c, view.to_screen(a), view.to_screen(b), entry.edge, 1.5f * c.scale);
            else
                c.list.AddLine(view.to_screen(a), view.to_screen(b), entry.edge, 1.5f * c.scale);
        }
}

void draw_world(const Context& c, const Item& item) {
    WorldView view;
    LocalCamera camera, drawn;
    if (item.world_depth_picture && c.resources.want_depth_picture) c.resources.want_depth_picture();
    const bool synced = c.resources.frame_camera && c.resources.frame_camera(drawn, item.world_depth_camera);
    if (!world_view(view, synced ? 0.0 : item.camera_lead, &camera, synced ? &drawn : nullptr,
            item.world_depth_jitter_kept))
        return;
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
    if (const auto mesh = item.world_cells ? item.world_cells->shown() : nullptr) draw_world_cells(c, item, mesh, view);
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
        if (count < 3) continue;
        double middle[3] = {0.0, 0.0, 0.0};
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i) middle[i] += quad.corners[k][i] * 0.25;
        c.list.AddConvexPolyFilled(out, count, fade(c, behind_solids(item, view, middle) ? lighter(quad.color) : quad.color));
    }
    // Lighter behind the solids, from their middle.
    for (const WorldLine& line : item.world_lines) {
        const double middle[3] = {(line.from[0] + line.to[0]) * 0.5, (line.from[1] + line.to[1]) * 0.5,
            (line.from[2] + line.to[2]) * 0.5};
        segment(line.from, line.to, behind_solids(item, view, middle) ? lighter(line.color) : line.color, line.width);
    }
    double corners[4][3];
    if (item.aim_face && camera.has_cursor && !(camera.cursor_flags & 1u) && aimed_face(camera, item, corners)) {
        for (int k = 0; k < 4; ++k) segment(corners[k], corners[(k + 1) % 4], item.aim_face_color, item.aim_face_width);
        // The game's depth read back at the aimed face's middle, times its
        // distance: the game's near distance (depth = near / distance), to
        // check the one assumed (game-depth.on).
        static auto last_near_log = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (c.resources.game_depth_at && now - last_near_log >= std::chrono::seconds(5)) {
            double middle[3] = {0.0, 0.0, 0.0}, p[3];
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i) middle[i] += corners[k][i] * 0.25;
            view.to_camera(middle, p);
            const float game = p[2] > kNear ? c.resources.game_depth_at(view.to_screen(p)) : -1.0f;
            if (game > 0.0f) {
                last_near_log = now;
                log_line("game depth: at the aimed face, %.2f m away: depth %.6g, near %.5g m", p[2], game, game * p[2]);
            }
        }
    }
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

// Pixels a cell of the drawn mesh should at least take: smaller, a coarser
// mesh of the box is drawn (27/10: 256 cells a side in the preview's frame,
// about a pixel a cell, 21 ms a frame and 570 000 vertices). A mesh of more
// faces than the item's `max_faces` too.
constexpr float kLevelPixels = 2.0f;
// Depth buckets of the farthest-first order (an order by buckets, not a sort).
constexpr size_t kDepthBuckets = 16384;

void draw_voxels(const Context& c, const Item& item, ImVec2 position, ImVec2 size) {
    // Nothing until its first faces are built (then the last ones meanwhile).
    const auto shown = item.voxels ? item.voxels->shown() : nullptr;
    if (!shown) return;
    const VoxelMesh& full = *shown;
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
    const float diagonal = std::sqrt(static_cast<float>(full.sx * full.sx + full.sy * full.sy + full.sz * full.sz));
    const float k = item.zoom * std::min(size.x, size.y) / std::max(1.0f, diagonal);
    const ImVec2 centre(position.x + size.x * 0.5f, position.y + size.y * 0.5f);
    const float half[3] = {full.sx * 0.5f, full.sy * 0.5f, full.sz * 0.5f};
    // The mesh whose cells take at least kLevelPixels, within max_faces, or
    // the coarsest.
    const VoxelMesh* chosen = &full;
    for (const auto& coarse : full.coarser)
        if (k * chosen->scale < kLevelPixels || chosen->faces.size() > static_cast<size_t>(item.max_faces))
            chosen = coarse.get();
    const VoxelMesh& mesh = *chosen;
    const float s = static_cast<float>(mesh.scale);
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
    // Whether a face is turned to the camera (from its centre in perspective),
    // in cells of the box.
    const auto turned_to_camera = [&](const VoxelFace& face) {
        const float* n = kNormals[face.direction];
        if (distance <= 0.0f) return n[0] * f[0] + n[1] * f[1] + n[2] * f[2] < 0.0f;
        const float x = (face.x + 0.5f + n[0] * 0.5f) * s - half[0] + f[0] * distance;
        const float y = (face.y + 0.5f + n[1] * 0.5f) * s - half[1] + f[1] * distance;
        const float z = (face.z + 0.5f + n[2] * 0.5f) * s - half[2] + f[2] * distance;
        return n[0] * x + n[1] * y + n[2] * z < 0.0f;
    };

    c.list.PushClipRect(position, ImVec2(position.x + size.x, position.y + size.y), true);
    if (item.floor_grid > 0 && visible_color(item.grid_color)) {
        const ImU32 color = fade(c, shade(item.grid_color, 1.0f, item.opacity));
        for (int x = 0; x <= full.sx; x += item.floor_grid)
            c.list.AddLine(project(x, 0, 0), project(x, 0, full.sz), color);
        for (int z = 0; z <= full.sz; z += item.floor_grid)
            c.list.AddLine(project(0, 0, z), project(full.sx, 0, z), color);
    }

    // Corners of each face of the unit cube, in order around it.
    static constexpr float kCorners[6][4][3] = {
        {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}, {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}},
        {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}, {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}};
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
    // Farthest first, by depth buckets (no sort: 27/10, every face sorted at
    // every frame), found again only when the camera moves.
    VoxelOrder& order = mesh.order;
    if (!order.valid || order.yaw != yaw || order.pitch != pitch || order.distance != distance ||
        order.xray != item.xray || order.see_through != see_through) {
        static thread_local std::vector<float> depths;
        static thread_local std::vector<std::uint32_t> ids, counts;
        depths.clear();
        ids.clear();
        const auto add = [&](std::uint32_t i) {
            const VoxelFace& face = mesh.faces[i];
            const float* n = kNormals[face.direction];
            depths.push_back(depth((face.x + 0.5f + n[0] * 0.5f) * s, (face.y + 0.5f + n[1] * 0.5f) * s,
                (face.z + 0.5f + n[2] * 0.5f) * s));
            ids.push_back(i);
        };
        if (!see_through) {
            for (std::uint32_t i = 0; i < mesh.faces.size(); ++i) {
                const VoxelFace& face = mesh.faces[i];
                if ((item.xray || !face.inner) && turned_to_camera(face)) add(i);
            }
        } else {
            for (const std::uint32_t i : seen.faces) add(i);
        }
        order.faces.resize(ids.size());
        if (!ids.empty()) {
            const auto [low, high] = std::minmax_element(depths.begin(), depths.end());
            const float far = *high, range = std::max(1e-6f, *high - *low);
            const float to_bucket = (kDepthBuckets - 1) / range;
            counts.assign(kDepthBuckets + 1, 0);
            for (const float d : depths) ++counts[static_cast<size_t>((far - d) * to_bucket) + 1];
            for (size_t b = 1; b <= kDepthBuckets; ++b) counts[b] += counts[b - 1];
            for (size_t i = 0; i < ids.size(); ++i)
                order.faces[counts[static_cast<size_t>((far - depths[i]) * to_bucket)]++] = ids[i];
        }
        order.valid = true;
        order.yaw = yaw;
        order.pitch = pitch;
        order.distance = distance;
        order.xray = item.xray;
        order.see_through = see_through;
    }
    // Plain solid view: neither outlines nor anti-aliased borders, whose
    // overlaps would draw the seams between faces when see-through.
    const bool plain = item.plain && !item.xray;
    const bool edges = !plain && visible_color(item.edge) && k * s >= 3.0f;
    const ImDrawListFlags flags = faces.Flags;
    if (plain) faces.Flags &= ~ImDrawListFlags_AntiAliasedFill;
    // Off screen, the colors as they are: the opacity comes when laid over.
    const auto color = [&](ImU32 value) { return layered ? value : fade(c, value); };
    const auto alpha_of = [&](unsigned char ch) {
        const bool ghost = item.xray && item.ghost.find(static_cast<char>(ch)) != std::string::npos;
        return (ghost ? item.ghost_alpha : 1.0f) * (layered ? 1.0f : item.opacity);
    };
    // Colors by direction and character, made once per frame.
    ImU32 colors[6][128];
    bool colored[128] = {};
    const auto color_of = [&](const VoxelFace& face) {
        if (!colored[face.ch]) {
            const float alpha = alpha_of(face.ch);
            for (int d = 0; d < 6; ++d) colors[d][face.ch] = color(shade(item.cell_colors[face.ch], kLight[d], alpha));
            colored[face.ch] = true;
        }
        return colors[face.direction][face.ch];
    };
    // Corners in pixels: parallel, the projected origin plus a step per axis;
    // in perspective, each projected.
    const ImVec2 origin = project(0.0f, 0.0f, 0.0f);
    ImVec2 axis_step[3];
    for (int axis = 0; axis < 3; ++axis) {
        const ImVec2 end = project(axis == 0 ? s : 0.0f, axis == 1 ? s : 0.0f, axis == 2 ? s : 0.0f);
        axis_step[axis] = ImVec2(end.x - origin.x, end.y - origin.y);
    }
    const auto corners_of = [&](const VoxelFace& face, ImVec2 points[4]) {
        for (int corner = 0; corner < 4; ++corner) {
            const float* p = kCorners[face.direction][corner];
            const float x = face.x + p[0], y = face.y + p[1], z = face.z + p[2];
            points[corner] = distance > 0.0f
                ? project(x * s, y * s, z * s)
                : ImVec2(origin.x + axis_step[0].x * x + axis_step[1].x * y + axis_step[2].x * z,
                      origin.y + axis_step[0].y * x + axis_step[1].y * y + axis_step[2].y * z);
        }
    };
    if (plain) {
        // Plain quads written straight into the list, by batches.
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        constexpr size_t kBatch = 8192;
        for (size_t first = 0; first < order.faces.size(); first += kBatch) {
            const size_t count = std::min(kBatch, order.faces.size() - first);
            faces.PrimReserve(static_cast<int>(count * 6), static_cast<int>(count * 4));
            for (size_t j = first; j < first + count; ++j) {
                const VoxelFace& face = mesh.faces[order.faces[j]];
                ImVec2 points[4];
                corners_of(face, points);
                faces.PrimQuadUV(points[0], points[1], points[2], points[3], uv, uv, uv, uv, color_of(face));
            }
        }
    } else {
        for (const std::uint32_t i : order.faces) {
            const VoxelFace& face = mesh.faces[i];
            ImVec2 points[4];
            corners_of(face, points);
            faces.AddQuadFilled(points[0], points[1], points[2], points[3], color_of(face));
            if (edges)
                faces.AddQuad(points[0], points[1], points[2], points[3], color(shade(item.edge, 1.0f, alpha_of(face.ch))));
        }
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
    for (const VoxelMark& mark : full.marks)
        c.list.AddCircleFilled(project(mark.x + 0.5f, mark.y + 0.5f, mark.z + 0.5f), dot,
            fade(c, item.cell_marks[mark.ch]), 12);
    // Lines over everything, in cells of the box.
    for (const WorldLine& line : item.world_lines)
        c.list.AddLine(project(static_cast<float>(line.from[0]), static_cast<float>(line.from[1]),
                           static_cast<float>(line.from[2])),
            project(static_cast<float>(line.to[0]), static_cast<float>(line.to[1]), static_cast<float>(line.to[2])),
            fade(c, line.color), line.width * c.scale);
    c.list.PopClipRect();
}

// A drum: the options around a cylinder seen from the front, the current one
// in the middle of a dark band, the previous and the next squashed and dimmed
// above and below it; shaded top and bottom. A new index turns the drum
// there, the short way round, in about a quarter of a second (kept per zone
// and drum id).
ImU32 mix(ImU32 a, ImU32 b, float t) {
    const auto channel = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t + 0.5f) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | channel(IM_COL32_A_SHIFT);
}

void draw_drum(const Context& c, const Item& item, ImVec2 position, ImVec2 size) {
    struct Turn {
        float shown = 0.0f;
        double time = 0.0;
    };
    static std::unordered_map<std::string, Turn> turns;
    const int count = static_cast<int>(item.drum_options.size());
    const double now = ImGui::GetTime();
    float shown = static_cast<float>(item.drum_index);
    if (!item.drum_id.empty()) {
        const auto [entry, created] = turns.try_emplace(c.zone + "/" + item.drum_id, Turn{shown, now});
        Turn& turn = entry->second;
        if (!created) {
            const float left = std::remainder(shown - turn.shown, static_cast<float>(count));
            const float step = 1.0f - std::exp(-static_cast<float>(now - turn.time) * 14.0f);
            turn.shown = std::fabs(left) < 0.01f ? shown : turn.shown + left * step;
        }
        turn.time = now;
        shown = turn.shown;
    }
    const ImVec2 min = snap(position);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    const float rounding = 5.0f * c.scale;
    c.list.AddRectFilled(min, max, fade(c, item.background), rounding);
    c.list.PushClipRect(min, max, true);
    // Each option, and its copies a turn away, where it stands on the cylinder.
    for (int i = 0; i < count; ++i)
        for (int turn = -1; turn <= 1; ++turn) {
            const float d = static_cast<float>(i + turn * count) - shown;
            if (std::fabs(d) > 1.3f) continue;
            const float angle = d * 0.95f;
            const float facing = std::cos(angle);
            if (facing <= 0.15f) continue;
            const float away = std::min(1.0f, std::fabs(d));
            TextStyle style = item.style;
            style.size = item.style.size * (1.0f - 0.5f * away);
            style.color = mix(item.style.color, item.drum_dim, away);
            const char* text = item.drum_options[static_cast<size_t>(i)].c_str();
            const ImVec2 extent = text_size(c, style, text, nullptr, 0.0f);
            const float y = min.y + size.y * 0.5f + std::sin(angle) * size.y * 0.4f - extent.y * 0.5f;
            draw_text_run(c, style, ImVec2(min.x + (size.x - extent.x) * 0.5f, y), text, nullptr, 0.0f);
        }
    // Shaded towards the top and the bottom, as a cylinder.
    const float band = size.y * 0.3f;
    const ImU32 dark = fade(c, IM_COL32(0, 0, 0, 170)), clear = fade(c, IM_COL32(0, 0, 0, 0));
    c.list.AddRectFilledMultiColor(min, ImVec2(max.x, min.y + band), dark, dark, clear, clear);
    c.list.AddRectFilledMultiColor(ImVec2(min.x, max.y - band), max, clear, clear, dark, dark);
    c.list.PopClipRect();
    c.list.AddRect(min, max, fade(c, item.drum_border), rounding, std::max(1.0f, c.scale));
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
    // Its frame, then its content inside the padding.
    const float width = content_width < FLT_MAX ? content_width : size.x;
    if (visible_color(item.background) || visible_color(item.frame_border)) {
        const ImVec2 min = snap(position);
        const ImVec2 max(min.x + width, min.y + size.y);
        const float rounding = item.rounding * c.scale;
        if (visible_color(item.background)) c.list.AddRectFilled(min, max, fade(c, item.background), rounding);
        if (visible_color(item.frame_border))
            c.list.AddRect(min, max, fade(c, item.frame_border), rounding, std::max(1.0f, c.scale));
    }
    const ImVec2 padding(item.frame_padding.x * c.scale, item.frame_padding.y * c.scale);
    position = ImVec2(position.x + padding.x, position.y + padding.y);
    size = ImVec2(size.x - padding.x * 2.0f, size.y - padding.y * 2.0f);
    if (content_width < FLT_MAX) content_width -= padding.x * 2.0f;
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
    case ItemKind::Drum:
        draw_drum(c, item, position, size);
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
