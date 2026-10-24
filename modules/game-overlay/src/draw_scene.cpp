// Layout and drawing of the zones. Items are stacked vertically inside a
// zone; a row places its items side by side, centered vertically. Sizes given
// by mods are reference pixels (1080p) multiplied by the screen scale.

#include "draw_scene.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace overlay {

namespace {

struct Context {
    ImDrawList& list;
    const DrawResources& resources;
    float scale;
    float opacity;
};

ImU32 fade(const Context& c, ImU32 color) {
    const float alpha = ((color >> IM_COL32_A_SHIFT) & 0xFF) * c.opacity;
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
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

// Size of an item; `width` is the available width, or FLT_MAX when the zone
// fits its content (text then does not wrap).
ImVec2 measure(const Context& c, const Item& item, float width) {
    switch (item.kind) {
    case ItemKind::Text: {
        ImFont* font = c.resources.font(item.style.font);
        const float wrap = width < FLT_MAX ? width : 0.0f;
        return font->CalcTextSizeA(item.style.size * c.scale, FLT_MAX, wrap, item.text.c_str());
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
    case ItemKind::Spacer:
        return {0.0f, item.size.y * c.scale};
    case ItemKind::Separator:
        return {0.0f, std::max(1.0f, item.thickness * c.scale)};
    case ItemKind::Progress: {
        const float w = item.size.x > 0.0f ? item.size.x * c.scale : (width < FLT_MAX ? width : 120.0f * c.scale);
        return {w, item.size.y * c.scale};
    }
    }
    return {0.0f, 0.0f};
}

void draw_text(const Context& c, const Item& item, ImVec2 position, float width) {
    ImFont* font = c.resources.font(item.style.font);
    const float size = item.style.size * c.scale;
    const float wrap = width < FLT_MAX ? width : 0.0f;
    const char* text = item.text.c_str();
    position = snap(position);
    if (item.style.outline > 0.0f) {
        const float o = std::max(1.0f, item.style.outline * c.scale);
        const ImU32 color = fade(c, item.style.outline_color);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) c.list.AddText(font, size, ImVec2(position.x + dx * o, position.y + dy * o), color, text, nullptr, wrap);
    }
    if (item.style.shadow) {
        const float o = std::max(1.0f, 1.5f * c.scale);
        c.list.AddText(font, size, ImVec2(position.x + o, position.y + o), fade(c, item.style.shadow_color), text, nullptr, wrap);
    }
    c.list.AddText(font, size, position, fade(c, item.style.color), text, nullptr, wrap);
}

// `wrap_width` is the zone's fixed content width, or FLT_MAX when the zone fits
// its content; `content_width` is the width separators and bars span.
void draw_item(const Context& c, const Item& item, ImVec2 position, ImVec2 size, float content_width, float wrap_width) {
    switch (item.kind) {
    case ItemKind::Text:
        draw_text(c, item, position, wrap_width);
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
    case ItemKind::Row: {
        float x = position.x;
        for (const Item& child : item.children) {
            const ImVec2 child_size = measure(c, child, FLT_MAX);
            draw_item(c, child, ImVec2(x, position.y + (size.y - child_size.y) * 0.5f), child_size, child_size.x, FLT_MAX);
            x += child_size.x + item.spacing * c.scale;
        }
        break;
    }
    case ItemKind::Spacer:
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
    if ((zone.background >> IM_COL32_A_SHIFT) & 0xFF) c.list.AddRectFilled(min, max, fade(c, zone.background), rounding);
    if (zone.frame.image) draw_frame(c, zone.frame, min, max);
    if (zone.border_width > 0.0f && ((zone.border >> IM_COL32_A_SHIFT) & 0xFF))
        c.list.AddRect(min, max, fade(c, zone.border), rounding, std::max(1.0f, zone.border_width * c.scale));

    c.list.PushClipRect(min, max, true);
    float y = min.y + padding.y;
    const float x = min.x + padding.x;
    for (size_t i = 0; i < zone.items.size(); ++i) {
        const Item& item = zone.items[i];
        const ImVec2 item_size = sizes[i];
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
    const Context c{list, resources, screen.y / kReferenceHeight * scene.options.scale, scene.options.opacity};
    for (const auto& zone : scene.zones) draw_zone(c, *zone, screen);
}

}  // namespace overlay
