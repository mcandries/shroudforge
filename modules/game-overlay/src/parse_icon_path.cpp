// Icons are SVG path data, stroked as polylines: lines (M L H V Z) and
// elliptical arcs (A), absolute or relative. Curves (C S Q T) are refused.

#include "scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace overlay {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr size_t kMaxPoints = 4096;

struct Reader {
    const char* at;

    void skip() {
        while (*at && (std::isspace(static_cast<unsigned char>(*at)) || *at == ',')) ++at;
    }

    bool number(float& value) {
        skip();
        char* end = nullptr;
        value = std::strtof(at, &end);
        if (end == at) return false;
        at = end;
        return true;
    }

    // Arc flags may be written without separators ("0 01" or "011").
    bool flag(bool& value) {
        skip();
        if (*at != '0' && *at != '1') return false;
        value = *at++ == '1';
        return true;
    }

    bool more_numbers() {
        skip();
        return *at && (std::isdigit(static_cast<unsigned char>(*at)) || *at == '-' || *at == '+' || *at == '.');
    }
};

// SVG arc from `from` to `to` (SVG 1.1, F.6.5), appended without its first point.
void add_arc(std::vector<ImVec2>& points, ImVec2 from, float rx, float ry, float rotation, bool large, bool sweep, ImVec2 to) {
    if (rx == 0.0f || ry == 0.0f) {
        points.push_back(to);
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const float phi = rotation * kPi / 180.0f;
    const float c = std::cos(phi), s = std::sin(phi);
    const float dx = (from.x - to.x) / 2.0f, dy = (from.y - to.y) / 2.0f;
    const float x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
    const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0f) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const float numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const float denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float factor = denominator > 0.0f ? std::sqrt(std::max(0.0f, numerator / denominator)) : 0.0f;
    if (large == sweep) factor = -factor;
    const float cx1 = factor * rx * y1 / ry, cy1 = -factor * ry * x1 / rx;
    const float cx = c * cx1 - s * cy1 + (from.x + to.x) / 2.0f;
    const float cy = s * cx1 + c * cy1 + (from.y + to.y) / 2.0f;
    auto angle = [](float ux, float uy, float vx, float vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const float start = angle(1.0f, 0.0f, (x1 - cx1) / rx, (y1 - cy1) / ry);
    float delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && delta > 0.0f) delta -= 2.0f * kPi;
    if (sweep && delta < 0.0f) delta += 2.0f * kPi;
    const int steps = std::max(4, static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 12.0f))));
    for (int i = 1; i <= steps; ++i) {
        const float t = start + delta * i / steps;
        const float ex = rx * std::cos(t), ey = ry * std::sin(t);
        points.push_back(i == steps ? to : ImVec2(c * ex - s * ey + cx, s * ex + c * ey + cy));
    }
}

}  // namespace

bool parse_icon_path(const std::string& path, std::vector<Polyline>& out, std::string& error) {
    Reader reader{path.c_str()};
    ImVec2 current{0.0f, 0.0f};
    ImVec2 start{0.0f, 0.0f};
    char command = 0;
    auto open = [&]() -> Polyline& {
        if (out.empty() || out.back().closed || out.back().points.empty()) {
            if (out.empty() || !out.back().points.empty()) out.emplace_back();
            out.back().points.push_back(current);
        }
        return out.back();
    };
    size_t total = 0;
    while (true) {
        reader.skip();
        if (!*reader.at) break;
        if (std::isalpha(static_cast<unsigned char>(*reader.at))) {
            command = *reader.at++;
        } else if (!command) {
            error = "icon path must start with a command";
            return false;
        }
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const ImVec2 base = relative ? current : ImVec2(0.0f, 0.0f);
        switch (std::toupper(static_cast<unsigned char>(command))) {
        case 'M': {
            float x, y;
            if (!reader.number(x) || !reader.number(y)) { error = "icon path: M needs x y"; return false; }
            current = ImVec2(base.x + x, base.y + y);
            start = current;
            out.emplace_back();
            out.back().points.push_back(current);
            command = relative ? 'l' : 'L';  // Extra pairs after M are lines.
            break;
        }
        case 'L': {
            float x, y;
            if (!reader.number(x) || !reader.number(y)) { error = "icon path: L needs x y"; return false; }
            current = ImVec2(base.x + x, base.y + y);
            open().points.push_back(current);
            break;
        }
        case 'H': {
            float x;
            if (!reader.number(x)) { error = "icon path: H needs x"; return false; }
            current.x = base.x + x;
            open().points.push_back(current);
            break;
        }
        case 'V': {
            float y;
            if (!reader.number(y)) { error = "icon path: V needs y"; return false; }
            current.y = (relative ? current.y : 0.0f) + y;
            open().points.push_back(current);
            break;
        }
        case 'A': {
            float rx, ry, rotation, x, y;
            bool large, sweep;
            if (!reader.number(rx) || !reader.number(ry) || !reader.number(rotation) || !reader.flag(large) ||
                    !reader.flag(sweep) || !reader.number(x) || !reader.number(y)) {
                error = "icon path: A needs rx ry rotation large-arc sweep x y";
                return false;
            }
            const ImVec2 to(base.x + x, base.y + y);
            add_arc(open().points, current, rx, ry, rotation, large, sweep, to);
            current = to;
            break;
        }
        case 'Z':
            if (!out.empty() && !out.back().points.empty()) out.back().closed = true;
            current = start;
            command = 0;
            continue;
        default:
            error = std::string("icon path: unsupported command ") + command;
            return false;
        }
        for (const Polyline& line : out) total += line.points.size();
        if (total > kMaxPoints) {
            error = "icon path too long";
            return false;
        }
        total = 0;
        if (!reader.more_numbers() && reader.at[0] && !std::isalpha(static_cast<unsigned char>(reader.at[0]))) {
            error = "icon path: unexpected character";
            return false;
        }
    }
    std::erase_if(out, [](const Polyline& line) { return line.points.size() < 2; });
    return true;
}

}  // namespace overlay
