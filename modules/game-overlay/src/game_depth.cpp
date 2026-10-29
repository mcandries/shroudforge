// The game's scene depth image, followed through its frames, for the overlay
// to read it at the present (to draw the world items against the game's own
// depth). On only with a file `game-depth.on` next to the layer, read once
// when the game creates its device; off, no function is intercepted.
//
// Enshrouded (render probe, 29/10/2026) renders with vkCmdBeginRendering on
// one queue; its scene depth is a D32 image the size of its render, created
// with TRANSFER_SRC and SAMPLED, kept to the end of the frame, two of them
// taking turns. Reading it means knowing the layout the game left it in: the
// layer follows the depth images' layouts through the barriers and depth
// attachments the game records, applied in the order it submits them.

#include "overlay.h"
#include "game_depth.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace overlay::depth {

namespace {

#define DEPTH_FUNCTIONS(X) \
    X(CreateImage, vkCreateImage) X(DestroyImage, vkDestroyImage) X(CreateImageView, vkCreateImageView) \
    X(BeginCommandBuffer, vkBeginCommandBuffer) X(CmdBeginRendering, vkCmdBeginRendering) \
    X(CmdBeginRenderingKHR, vkCmdBeginRenderingKHR) X(CmdPipelineBarrier, vkCmdPipelineBarrier) \
    X(CmdPipelineBarrier2, vkCmdPipelineBarrier2) X(CmdPipelineBarrier2KHR, vkCmdPipelineBarrier2KHR) \
    X(QueueSubmit, vkQueueSubmit) X(QueueSubmit2, vkQueueSubmit2) X(QueueSubmit2KHR, vkQueueSubmit2KHR)

struct Next {
#define DEPTH_DECLARE(name, type) PFN_##type name{};
    DEPTH_FUNCTIONS(DEPTH_DECLARE)
#undef DEPTH_DECLARE
};

Next next;
VkDevice followed_device{};

// A depth image the overlay could read: one level, one layer, copyable.
struct Followed {
    int id = 0;
    VkFormat format{};
    VkExtent2D extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // as submitted so far
};

// What a command buffer does to them, in recording order.
struct Event {
    VkImage image{};
    VkImageLayout layout{};
    bool attachment = false;  // a depth attachment of a pass
};

std::mutex depth_mutex;
std::unordered_map<VkImage, Followed> followed;
std::unordered_map<VkImageView, VkImage> views;
std::unordered_map<VkCommandBuffer, std::vector<Event>> recordings;
std::vector<VkImage> attached;  // this frame's depth attachments, as submitted
int next_id = 1;

thread_local VkCommandBuffer cached_buffer{};
thread_local std::vector<Event>* cached_events{};

std::vector<Event>& events_of(VkCommandBuffer buffer) {
    // Called under depth_mutex.
    if (buffer == cached_buffer && cached_events) return *cached_events;
    cached_buffer = buffer;
    cached_events = &recordings[buffer];
    return *cached_events;
}

bool depth_format(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D32_SFLOAT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkImage* image) {
    const VkResult result = next.CreateImage(device, info, allocator, image);
    if (result != VK_SUCCESS) return result;
    if (depth_format(info->format) && (info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && info->mipLevels == 1 &&
            info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT && info->imageType == VK_IMAGE_TYPE_2D) {
        std::lock_guard lock(depth_mutex);
        followed[*image] = {next_id++, info->format, {info->extent.width, info->extent.height}, info->initialLayout};
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard lock(depth_mutex);
        followed.erase(image);
    }
    next.DestroyImage(device, image, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkImageView* view) {
    const VkResult result = next.CreateImageView(device, info, allocator, view);
    if (result != VK_SUCCESS) return result;
    std::lock_guard lock(depth_mutex);
    if (followed.count(info->image)) views[*view] = info->image;
    else views.erase(*view);  // a handle given again
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
    {
        std::lock_guard lock(depth_mutex);
        events_of(buffer).clear();
    }
    return next.BeginCommandBuffer(buffer, info);
}

void begin_rendering(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    if (!info->pDepthAttachment || !info->pDepthAttachment->imageView) return;
    std::lock_guard lock(depth_mutex);
    auto view = views.find(info->pDepthAttachment->imageView);
    if (view == views.end()) return;
    events_of(buffer).push_back({view->second, info->pDepthAttachment->imageLayout, true});
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    begin_rendering(buffer, info);
    next.CmdBeginRendering(buffer, info);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    begin_rendering(buffer, info);
    next.CmdBeginRenderingKHR(buffer, info);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer buffer, VkPipelineStageFlags source,
        VkPipelineStageFlags target, VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier* memory,
        uint32_t buffer_count, const VkBufferMemoryBarrier* buffers, uint32_t image_count,
        const VkImageMemoryBarrier* images) {
    if (image_count) {
        std::lock_guard lock(depth_mutex);
        for (uint32_t i = 0; i < image_count; ++i)
            if (followed.count(images[i].image)) events_of(buffer).push_back({images[i].image, images[i].newLayout, false});
    }
    next.CmdPipelineBarrier(buffer, source, target, flags, memory_count, memory, buffer_count, buffers, image_count, images);
}

void barrier2(VkCommandBuffer buffer, const VkDependencyInfo* info) {
    if (!info->imageMemoryBarrierCount) return;
    std::lock_guard lock(depth_mutex);
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2& barrier = info->pImageMemoryBarriers[i];
        if (followed.count(barrier.image)) events_of(buffer).push_back({barrier.image, barrier.newLayout, false});
    }
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer buffer, const VkDependencyInfo* info) {
    barrier2(buffer, info);
    next.CmdPipelineBarrier2(buffer, info);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2KHR(VkCommandBuffer buffer, const VkDependencyInfo* info) {
    barrier2(buffer, info);
    next.CmdPipelineBarrier2KHR(buffer, info);
}

void submitted(VkCommandBuffer buffer) {
    // Called under depth_mutex.
    auto found = recordings.find(buffer);
    if (found == recordings.end()) return;
    for (const Event& event : found->second) {
        auto image = followed.find(event.image);
        if (image == followed.end()) continue;
        image->second.layout = event.layout;
        if (event.attachment) attached.push_back(event.image);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    {
        std::lock_guard lock(depth_mutex);
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j) submitted(submits[i].pCommandBuffers[j]);
    }
    return next.QueueSubmit(queue, count, submits, fence);
}

void submitted2(uint32_t count, const VkSubmitInfo2* submits) {
    std::lock_guard lock(depth_mutex);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j)
            submitted(submits[i].pCommandBufferInfos[j].commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    submitted2(count, submits);
    return next.QueueSubmit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
        VkFence fence) {
    submitted2(count, submits);
    return next.QueueSubmit2KHR(queue, count, submits, fence);
}

std::wstring flag_path() {
    wchar_t path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&flag_path), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring file(path);
    file.erase(file.find_last_of(L'\\') + 1);
    return file + L"game-depth.on";
}

bool read_enabled() {
    return GetFileAttributesW(flag_path().c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::mutex settings_mutex;
Settings current;
ULONGLONG settings_checked = 0;
FILETIME settings_written{};

}  // namespace

bool enabled() {
    static const bool on = is_game_process() && read_enabled();
    return on;
}

void start(VkDevice device, PFN_vkGetDeviceProcAddr next_gdpa) {
    if (!enabled()) return;
    if (followed_device) {
        log_line("game depth: another device %p, not followed", static_cast<void*>(device));
        return;
    }
    followed_device = device;
#define DEPTH_LOAD(name, type) next.name = reinterpret_cast<PFN_##type>(next_gdpa(device, #type));
    DEPTH_FUNCTIONS(DEPTH_LOAD)
#undef DEPTH_LOAD
    log_line("game depth: on (game-depth.on), device %p", static_cast<void*>(device));
}

PFN_vkVoidFunction function(const char* requested) {
    if (!enabled()) return nullptr;
    // Only once the next layer's is known: never a function calling nothing.
#define DEPTH_INTERCEPT(name, type) \
    if (std::strcmp(requested, #type) == 0) return next.name ? reinterpret_cast<PFN_vkVoidFunction>(&name) : nullptr;
    DEPTH_FUNCTIONS(DEPTH_INTERCEPT)
#undef DEPTH_INTERCEPT
    return nullptr;
}

SceneDepth scene_depth(VkExtent2D screen) {
    SceneDepth best;
    if (!enabled() || !screen.height) return best;
    std::lock_guard lock(depth_mutex);
    const double aspect = static_cast<double>(screen.width) / screen.height;
    uint64_t best_area = 0;
    for (VkImage image : attached) {
        auto found = followed.find(image);
        if (found == followed.end() || !found->second.extent.height) continue;
        const Followed& f = found->second;
        const double shape = static_cast<double>(f.extent.width) / f.extent.height;
        if (std::abs(shape - aspect) > 0.02 * aspect) continue;
        const uint64_t area = static_cast<uint64_t>(f.extent.width) * f.extent.height;
        // The largest; the last used of the same size.
        if (area < best_area) continue;
        best_area = area;
        best = {image, f.format, f.extent, f.layout, f.id};
    }
    return best;
}

Settings settings() {
    std::lock_guard lock(settings_mutex);
    const ULONGLONG now = GetTickCount64();
    if (now - settings_checked < 1000) return current;
    settings_checked = now;
    static const std::wstring path = flag_path();
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
            CompareFileTime(&attributes.ftLastWriteTime, &settings_written) == 0)
        return current;
    settings_written = attributes.ftLastWriteTime;
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    Settings read;
    std::string word;
    while (text >> word) {
        const size_t equals = word.find('=');
        if (equals == std::string::npos) continue;
        const std::string name = word.substr(0, equals);
        const double value = std::atof(word.c_str() + equals + 1);
        if (name == "near" && value > 0.0) read.near_distance = value;
        if (name == "margin" && value >= 0.0) read.margin = value;
        if (name == "picture") read.show_picture = value != 0.0;
    }
    current = read;
    log_line("game depth: near %.4g m, margin %.3g m, picture %s", current.near_distance, current.margin,
        current.show_picture ? "shown" : "hidden");
    return current;
}

void presented() {
    if (!enabled()) return;
    std::lock_guard lock(depth_mutex);
    attached.clear();
}

}  // namespace overlay::depth
