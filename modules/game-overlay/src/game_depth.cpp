// The game's scene depth, copied as the game draws it, for the overlay to
// read at the present (to draw the world items against the game's own
// depth). On only with a file `game-depth.on` next to the layer, read once
// when the game creates its device; off, no function is intercepted.
//
// Enshrouded (render probe, 29/10/2026) renders with vkCmdBeginRendering on
// one queue; its scene depth is a D32 image the size of its render. Read at
// the present it holds only zeros (test of 29/10): its memory is used again
// later in the frame. So, as ReShade's generic depth does, the layer copies
// it into an image of its own right after each pass that writes it (stored,
// shaped like the screen, at least 30 % of its area): barriers and a copy
// recorded into the game's command buffer, the game's image put back in its
// layout. The overlay samples the copy, left in SHADER_READ_ONLY_OPTIMAL.
//
// The game's interface layer (want_interface_layer): from the game's first
// pass of a frame loading a swapchain image (31/10, capture: its interface;
// the scene is put on it before, by a compute pass), its passes on that image
// draw into an image of ours, cleared to transparent black, for the overlay
// to lay it back over its world items (renderer.cpp).
//
// The HUD's blurred scene (hud_blur_target): right after the game's barrier
// making its 320x180 blur readable, the overlay's own blur (the scene with the
// world items, from the last present) is copied over it.

#include "overlay.h"
#include "game_depth.h"
#include "gpu_camera.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace overlay::depth {

namespace {

#define DEPTH_FUNCTIONS(X) \
    X(CreateImage, vkCreateImage) X(DestroyImage, vkDestroyImage) X(CreateImageView, vkCreateImageView) \
    X(BeginCommandBuffer, vkBeginCommandBuffer) X(CmdBeginRendering, vkCmdBeginRendering) \
    X(CmdBeginRenderingKHR, vkCmdBeginRenderingKHR) X(CmdEndRendering, vkCmdEndRendering) \
    X(CmdEndRenderingKHR, vkCmdEndRenderingKHR) X(QueueSubmit, vkQueueSubmit) X(QueueSubmit2, vkQueueSubmit2) \
    X(QueueSubmit2KHR, vkQueueSubmit2KHR) X(CreateBuffer, vkCreateBuffer) X(DestroyBuffer, vkDestroyBuffer) \
    X(BindBufferMemory, vkBindBufferMemory) X(BindBufferMemory2, vkBindBufferMemory2) \
    X(BindBufferMemory2KHR, vkBindBufferMemory2KHR) X(AllocateMemory, vkAllocateMemory) X(MapMemory, vkMapMemory) \
    X(UnmapMemory, vkUnmapMemory) X(FreeMemory, vkFreeMemory) X(UpdateDescriptorSets, vkUpdateDescriptorSets) \
    X(CmdBindDescriptorSets, vkCmdBindDescriptorSets) X(CmdPushConstants, vkCmdPushConstants) \
    X(GetBufferDeviceAddress, vkGetBufferDeviceAddress) X(GetBufferDeviceAddressKHR, vkGetBufferDeviceAddressKHR) \
    X(GetBufferDeviceAddressEXT, vkGetBufferDeviceAddressEXT) X(CmdPipelineBarrier, vkCmdPipelineBarrier)

struct Next {
#define DEPTH_DECLARE(name, type) PFN_##type name{};
    DEPTH_FUNCTIONS(DEPTH_DECLARE)
#undef DEPTH_DECLARE
    // Called, not intercepted.
    PFN_vkCmdCopyImage CmdCopyImage{};
    PFN_vkCmdBlitImage CmdBlitImage{};
    PFN_vkCmdClearColorImage CmdClearColorImage{};
};

Next next;
DeviceData* device_data{};

// A depth image the overlay could copy: one level, one layer, copyable.
struct Followed {
    int id = 0;
    VkFormat format{};
    VkExtent2D extent{};
};

// A color image of the game the light view may copy: its G-buffer's color
// (R8G8B8A8_UNORM, made copyable) and normals (A2B10G10R10, told apart).
struct FollowedColor {
    VkFormat format{};
    VkExtent2D extent{};
};

// The pass being recorded in a command buffer: its depth attachment.
struct Recording {
    std::vector<VkImage> layers;  // swapchain images whose interface it draws into our layer
    VkImage albedo{};             // the G-buffer's color being written, to copy as the pass ends
    VkImageLayout albedo_layout{};
    bool albedo_copied = false;   // a copy recorded in this command buffer
    VkImage depth{};
    VkImageLayout layout{};
    bool stored = false;     // its depth is written and kept
    bool suspending = false;  // resumed in another command buffer: no copy between
    bool copied = false;      // a copy recorded in this command buffer
    LocalCamera camera;       // read as the copy was recorded
    bool has_camera = false;
    bool scene = false;       // the pass recording is the scene's (its depth cleared)
};

std::mutex depth_mutex;
std::unordered_map<VkImage, Followed> followed;
std::unordered_map<VkImage, FollowedColor> colors;
std::unordered_map<VkImageView, VkImage> views;
std::unordered_map<VkCommandBuffer, Recording> recordings;
std::unordered_map<VkDeviceMemory, VkDeviceSize> memory_sizes;
std::unordered_set<VkImage> presentable;  // the swapchain's images
VkFormat presentable_format{};
std::unordered_map<VkImageView, VkFormat> presentable_views;  // the game's views of them, their format
VkExtent2D screen{};  // the swapchain's
int next_id = 1;

// The copy: an image of our own, made for the size and format copied.
struct Copy {
    VkImage image{};
    VkDeviceMemory memory{};
    VkExtent2D extent{};
    VkFormat format{};
    int id = 0;
    bool failed = false;
    uint64_t submitted_at = 0;  // the present count when a copy was last submitted
    LocalCamera camera;  // of the last copy submitted
    bool has_camera = false;
    gpu::Camera gpu_camera;  // the scene's, as the game handed it to the GPU
};
Copy copy;
uint64_t presents = 0;

// The light view's copy of the G-buffer's color, a quarter of its size.
struct AlbedoCopy {
    VkImage image{};
    VkDeviceMemory memory{};
    VkExtent2D extent{};
    int id = 0;
    bool failed = false;
    uint64_t submitted_at = 0;
};
AlbedoCopy albedo;

// The game's interface drawn into an image of ours, one per swapchain image.
struct Layer {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkExtent2D extent{};
    VkFormat format{};
    int id = 0;
    VkImageLayout layout{};  // the game's passes'
    bool begun = false;      // cleared and drawn into since its image was last presented
    bool submitted = false;  // a command buffer drawing into it submitted since
};
std::unordered_map<VkImage, Layer> layers;  // by swapchain image
std::atomic<bool> layer_wanted{false};
bool layer_failed = false;

// The HUD's blurred scene: the game's images that may be it (small
// R8G8B8A8_UNORM storage images, made copyable into), and ours.
std::unordered_map<VkImage, VkExtent2D> blur_candidates;
struct OwnBlur {
    VkImage image{};
    VkDeviceMemory memory{};
    VkExtent2D extent{};
    bool failed = false;
    bool said = false;
};
OwnBlur own_blur;
std::atomic<bool> blur_filled{false};

Recording& recording_of(VkCommandBuffer buffer) {
    // Called under depth_mutex.
    return recordings[buffer];
}

bool depth_format(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D32_SFLOAT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

uint32_t device_local_memory(uint32_t bits) {
    auto properties_of = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        device_data->instance->next_gipa(device_data->instance->instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!properties_of) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties properties{};
    properties_of(device_data->physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    return UINT32_MAX;
}

// Our image for a copy of this size and format (called under depth_mutex).
// The previous one, if any, is left (rare: the game's render size changed).
bool make_copy(VkExtent2D extent, VkFormat format) {
    if (copy.image && copy.extent.width == extent.width && copy.extent.height == extent.height && copy.format == format)
        return true;
    if (copy.failed || !device_data || !device_data->instance) return false;
    DeviceData& d = *device_data;
    Copy made;
    made.extent = extent;
    made.format = format;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements requirements{};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    const bool ok = d.fn.CreateImage(d.device, &info, nullptr, &made.image) == VK_SUCCESS &&
        (d.fn.GetImageMemoryRequirements(d.device, made.image, &requirements), true) &&
        (allocate.allocationSize = requirements.size,
            allocate.memoryTypeIndex = device_local_memory(requirements.memoryTypeBits),
            allocate.memoryTypeIndex != UINT32_MAX) &&
        d.fn.AllocateMemory(d.device, &allocate, nullptr, &made.memory) == VK_SUCCESS &&
        d.fn.BindImageMemory(d.device, made.image, made.memory, 0) == VK_SUCCESS;
    if (!ok) {
        if (made.image) d.fn.DestroyImage(d.device, made.image, nullptr);
        if (made.memory) d.fn.FreeMemory(d.device, made.memory, nullptr);
        copy.failed = true;
        log_line("game depth: no image of our own for a copy (%ux%u, format %d)", extent.width, extent.height,
            static_cast<int>(format));
        return false;
    }
    made.id = next_id++;
    copy = made;
    log_line("game depth: copied into an image of our own, #%d %ux%u", copy.id, extent.width, extent.height);
    return true;
}

}  // namespace

void make_light_levels(VkCommandBuffer buffer, VkImage image, VkExtent2D extent, PFN_vkCmdPipelineBarrier barrier_fn,
        PFN_vkCmdBlitImage blit_fn) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    for (uint32_t level = 1; level < kLightLevels; ++level) {
        // The level above, written: a source now; this one, a destination.
        VkImageMemoryBarrier pair[2] = {barrier, barrier};
        pair[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pair[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        pair[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pair[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        pair[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
        pair[1].srcAccessMask = 0;
        pair[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pair[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pair[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pair[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        barrier_fn(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
            pair);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(std::max(1u, extent.width >> (level - 1))),
            static_cast<int32_t>(std::max(1u, extent.height >> (level - 1))), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {static_cast<int32_t>(std::max(1u, extent.width >> level)),
            static_cast<int32_t>(std::max(1u, extent.height >> level)), 1};
        blit_fn(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
            VK_FILTER_LINEAR);
    }
    // Every level but the last a source now, the last a destination: all for the shaders.
    VkImageMemoryBarrier done[2] = {barrier, barrier};
    done[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    done[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    done[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    done[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    done[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLightLevels - 1, 0, 1};
    done[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    done[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    done[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    done[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, kLightLevels - 1, 1, 0, 1};
    barrier_fn(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2,
        done);
}

namespace {

// Our image for the G-buffer's color made smaller (under depth_mutex).
bool make_albedo(VkExtent2D source) {
    const VkExtent2D extent{std::max(1u, source.width / 4), std::max(1u, source.height / 4)};
    if (albedo.image && albedo.extent.width == extent.width && albedo.extent.height == extent.height) return true;
    if (albedo.failed || !device_data || !device_data->instance) return false;
    DeviceData& d = *device_data;
    AlbedoCopy made;
    made.extent = extent;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = kLightLevels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements requirements{};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    const bool ok = d.fn.CreateImage(d.device, &info, nullptr, &made.image) == VK_SUCCESS &&
        (d.fn.GetImageMemoryRequirements(d.device, made.image, &requirements), true) &&
        (allocate.allocationSize = requirements.size,
            allocate.memoryTypeIndex = device_local_memory(requirements.memoryTypeBits),
            allocate.memoryTypeIndex != UINT32_MAX) &&
        d.fn.AllocateMemory(d.device, &allocate, nullptr, &made.memory) == VK_SUCCESS &&
        d.fn.BindImageMemory(d.device, made.image, made.memory, 0) == VK_SUCCESS;
    if (!ok) {
        if (made.image) d.fn.DestroyImage(d.device, made.image, nullptr);
        if (made.memory) d.fn.FreeMemory(d.device, made.memory, nullptr);
        albedo.failed = true;
        log_line("light view: no image of our own for the G-buffer's color");
        return false;
    }
    made.id = next_id++;
    albedo = made;
    log_line("light view: the G-buffer's color copied into #%d %ux%u", albedo.id, extent.width, extent.height);
    return true;
}

// The G-buffer's color made smaller into ours, as its pass ends, the game's
// image put back in its layout.
void record_albedo(VkCommandBuffer buffer, VkImage source, VkImageLayout layout, VkExtent2D extent) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier before[2]{};
    before[0].sType = before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[0].oldLayout = layout;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].image = source;
    before[1].srcAccessMask = 0;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].image = albedo.image;
    for (auto& barrier : before) {
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = range;
    }
    before[1].subresourceRange.levelCount = kLightLevels;
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {static_cast<int32_t>(albedo.extent.width), static_cast<int32_t>(albedo.extent.height), 1};
    next.CmdBlitImage(buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, albedo.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    VkImageMemoryBarrier after = before[0];
    after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after.newLayout = layout;
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &after);
    // Its smaller levels: the light, smooth.
    make_light_levels(buffer, albedo.image, albedo.extent, next.CmdPipelineBarrier, next.CmdBlitImage);
}

// The interface layer of a swapchain image, made the screen's size in the
// format of the game's view of it (under depth_mutex); false when it cannot
// be (then the game draws its interface as usual).
bool make_layer(Layer& layer, VkFormat format) {
    if (layer.image) return layer.format == format && layer.extent.width == screen.width &&
        layer.extent.height == screen.height;
    if (layer_failed || !device_data || !device_data->instance || !screen.width || !screen.height) return false;
    DeviceData& d = *device_data;
    Layer made;
    made.extent = screen;
    made.format = format;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {screen.width, screen.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements requirements{};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const bool ok = d.fn.CreateImage(d.device, &info, nullptr, &made.image) == VK_SUCCESS &&
        (d.fn.GetImageMemoryRequirements(d.device, made.image, &requirements), true) &&
        (allocate.allocationSize = requirements.size,
            allocate.memoryTypeIndex = device_local_memory(requirements.memoryTypeBits),
            allocate.memoryTypeIndex != UINT32_MAX) &&
        d.fn.AllocateMemory(d.device, &allocate, nullptr, &made.memory) == VK_SUCCESS &&
        d.fn.BindImageMemory(d.device, made.image, made.memory, 0) == VK_SUCCESS &&
        (view_info.image = made.image, d.fn.CreateImageView(d.device, &view_info, nullptr, &made.view) == VK_SUCCESS);
    if (!ok) {
        if (made.image) d.fn.DestroyImage(d.device, made.image, nullptr);
        if (made.memory) d.fn.FreeMemory(d.device, made.memory, nullptr);
        layer_failed = true;
        log_line("interface layer: no image of our own, the game's interface drawn as usual");
        return false;
    }
    made.id = next_id++;
    layer = made;
    log_line("interface layer: #%d %ux%u (format %d), the game's interface drawn into it from its first pass "
        "loading the screen", layer.id, screen.width, screen.height, static_cast<int>(format));
    return true;
}

// The layer cleared to transparent black before the game's first pass draws
// into it (whatever its render area), then in that pass's layout.
void record_layer_clear(VkCommandBuffer buffer, const Layer& layer, VkImageLayout layout) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = range;
    barrier.image = layer.image;
    // Its last content (laid back by the overlay) is not needed.
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);
    const VkClearColorValue transparent{};
    next.CmdClearColorImage(buffer, layer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = layout;
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
        nullptr, 0, nullptr, 1, &barrier);
}

void destroy_layers() {
    // Called under depth_mutex, the device idle.
    DeviceData& d = *device_data;
    for (auto& [image, layer] : layers) {
        if (layer.view) d.fn.DestroyImageView(d.device, layer.view, nullptr);
        if (layer.image) d.fn.DestroyImage(d.device, layer.image, nullptr);
        if (layer.memory) d.fn.FreeMemory(d.device, layer.memory, nullptr);
    }
    layers.clear();
}

// The game's HUD blur: an eighth of the screen (under depth_mutex).
bool hud_blur_shaped(VkExtent2D extent) {
    return screen.width && screen.height && extent.width && extent.height &&
        std::abs(static_cast<int>(extent.width * 8) - static_cast<int>(screen.width)) <= 8 &&
        std::abs(static_cast<int>(extent.height * 8) - static_cast<int>(screen.height)) <= 8;
}

// Our blur, the size of the game's (under depth_mutex); false when it cannot be.
bool make_own_blur(VkExtent2D extent) {
    if (own_blur.image) return true;
    if (own_blur.failed || !device_data || !device_data->instance) return false;
    DeviceData& d = *device_data;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements requirements{};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    OwnBlur made;
    const bool ok = d.fn.CreateImage(d.device, &info, nullptr, &made.image) == VK_SUCCESS &&
        (d.fn.GetImageMemoryRequirements(d.device, made.image, &requirements), true) &&
        (allocate.allocationSize = requirements.size,
            allocate.memoryTypeIndex = device_local_memory(requirements.memoryTypeBits),
            allocate.memoryTypeIndex != UINT32_MAX) &&
        d.fn.AllocateMemory(d.device, &allocate, nullptr, &made.memory) == VK_SUCCESS &&
        d.fn.BindImageMemory(d.device, made.image, made.memory, 0) == VK_SUCCESS;
    if (!ok) {
        if (made.image) d.fn.DestroyImage(d.device, made.image, nullptr);
        if (made.memory) d.fn.FreeMemory(d.device, made.memory, nullptr);
        own_blur.failed = true;
        log_line("HUD blur: no image of our own, the game's blur left alone");
        return false;
    }
    made.extent = extent;
    own_blur = made;
    return true;
}

// Ours (SHADER_READ_ONLY, filled at the last present) copied over the game's
// blur `image` (just made SHADER_READ_ONLY by the game's barrier), each put
// back in SHADER_READ_ONLY_OPTIMAL.
void record_blur_copy(VkCommandBuffer buffer, VkImage image) {
    VkImageMemoryBarrier before[2]{};
    before[0].sType = before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[0].image = image;
    before[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[1].image = own_blur.image;
    for (auto& barrier : before) {
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 2, before);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {own_blur.extent.width, own_blur.extent.height, 1};
    next.CmdCopyImage(buffer, own_blur.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier after[2] = {before[0], before[1]};
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
        nullptr, 2, after);
}

// The scene's depth, as the game's pass ends: worth a copy?
bool scene_shaped(const Followed& image) {
    if (!screen.width || !screen.height || !image.extent.height) return false;
    const double aspect = static_cast<double>(screen.width) / screen.height;
    const double shape = static_cast<double>(image.extent.width) / image.extent.height;
    const double area = static_cast<double>(image.extent.width) * image.extent.height;
    return std::abs(shape - aspect) <= 0.02 * aspect && area >= 0.3 * screen.width * screen.height;
}

void record_copy(VkCommandBuffer buffer, VkImage source, VkImageLayout layout) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier before[2]{};
    before[0].sType = before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[0].oldLayout = layout;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].image = source;
    // Ours: its last content is not needed; after the overlay read it.
    before[1].srcAccessMask = 0;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].image = copy.image;
    for (auto& barrier : before) {
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = range;
    }
    next.CmdPipelineBarrier(buffer,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {copy.extent.width, copy.extent.height, 1};
    next.CmdCopyImage(buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier after[2] = {before[0], before[1]};
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[0].newLayout = layout;
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    next.CmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
        nullptr, 2, after);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkImage* image) {
    // The light view: the game's large R8G8B8A8_UNORM color attachments made
    // copyable (its G-buffer's color among them), its normals' format noted.
    const bool attachment = (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && info->mipLevels == 1 &&
        info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT && info->imageType == VK_IMAGE_TYPE_2D &&
        info->extent.width >= 640;
    const bool color = attachment && info->format == VK_FORMAT_R8G8B8A8_UNORM;
    const bool normals = attachment && info->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    // The HUD's blur may be among the small R8G8B8A8_UNORM storage images:
    // made copyable into.
    const bool blur = info->format == VK_FORMAT_R8G8B8A8_UNORM && (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && info->mipLevels == 1 && info->arrayLayers == 1 &&
        info->samples == VK_SAMPLE_COUNT_1_BIT && info->imageType == VK_IMAGE_TYPE_2D && info->extent.width <= 1024 &&
        info->extent.height <= 1024;
    VkImageCreateInfo copyable = *info;
    if (color) copyable.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (blur) copyable.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkResult result = next.CreateImage(device, &copyable, allocator, image);
    if (result != VK_SUCCESS) return result;
    if (blur) {
        std::lock_guard lock(depth_mutex);
        blur_candidates[*image] = {info->extent.width, info->extent.height};
    }
    if (color || normals) {
        std::lock_guard lock(depth_mutex);
        colors[*image] = {info->format, {info->extent.width, info->extent.height}};
    }
    if (depth_format(info->format) && (info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && info->mipLevels == 1 &&
            info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT && info->imageType == VK_IMAGE_TYPE_2D) {
        std::lock_guard lock(depth_mutex);
        followed[*image] = {next_id++, info->format, {info->extent.width, info->extent.height}};
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard lock(depth_mutex);
        followed.erase(image);
        colors.erase(image);
        blur_candidates.erase(image);
    }
    next.DestroyImage(device, image, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkImageView* view) {
    const VkResult result = next.CreateImageView(device, info, allocator, view);
    if (result != VK_SUCCESS) return result;
    std::lock_guard lock(depth_mutex);
    if (followed.count(info->image) || colors.count(info->image) || presentable.count(info->image))
        views[*view] = info->image;
    else views.erase(*view);  // a handle given again
    if (presentable.count(info->image)) presentable_views[*view] = info->format;
    else presentable_views.erase(*view);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
    {
        std::lock_guard lock(depth_mutex);
        recording_of(buffer) = Recording{};
    }
    gpu::command_buffer_begun(buffer);
    return next.BeginCommandBuffer(buffer, info);
}

// A pass of the game given to the next layer with some of its attachments
// changed: its copy.
struct Redirected {
    VkRenderingInfo info{};
    std::vector<VkRenderingAttachmentInfo> colors;
};

// The pass to hand on: `info`, or `redirected` drawing into our interface
// layers instead of the swapchain images.
const VkRenderingInfo* begin_rendering(VkCommandBuffer buffer, const VkRenderingInfo* info, Redirected& redirected) {
    const VkRenderingInfo* given = info;
    bool scene = false;
    {
        std::lock_guard lock(depth_mutex);
        Recording& recording = recording_of(buffer);
        recording.depth = VK_NULL_HANDLE;
        recording.scene = false;
        // The interface layer: from the game's first pass of the frame
        // loading a swapchain image (its interface), its passes on that image
        // draw into our layer, cleared first, outside any pass.
        if (layer_wanted.load(std::memory_order_acquire) && !layer_failed)
            for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
                const VkRenderingAttachmentInfo& attachment = info->pColorAttachments[i];
                if (!attachment.imageView) continue;
                auto view = views.find(attachment.imageView);
                if (view == views.end() || !presentable.count(view->second)) continue;
                auto format = presentable_views.find(attachment.imageView);
                Layer& layer = layers[view->second];
                const bool first = !layer.begun;
                if (first && (attachment.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || (info->flags & VK_RENDERING_RESUMING_BIT)))
                    continue;
                if (format == presentable_views.end() || !make_layer(layer, format->second)) continue;
                if (first) {
                    record_layer_clear(buffer, layer, attachment.imageLayout);
                    layer.begun = true;
                    layer.submitted = false;
                }
                layer.layout = attachment.imageLayout;
                if (given == info) {
                    redirected.info = *info;
                    redirected.colors.assign(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount);
                    redirected.info.pColorAttachments = redirected.colors.data();
                    given = &redirected.info;
                }
                redirected.colors[i].imageView = layer.view;
                recording.layers.push_back(view->second);
            }
        // The G-buffer: four color attachments or more, the first a
        // screen-shaped R8G8B8A8_UNORM, one an A2B10G10R10 (its normals).
        recording.albedo = VK_NULL_HANDLE;
        if (info->colorAttachmentCount >= 4 && !(info->flags & VK_RENDERING_SUSPENDING_BIT)) {
            const auto image_of = [&](uint32_t i) -> const std::pair<const VkImage, FollowedColor>* {
                if (!info->pColorAttachments[i].imageView) return nullptr;
                auto view = views.find(info->pColorAttachments[i].imageView);
                if (view == views.end()) return nullptr;
                auto found = colors.find(view->second);
                return found == colors.end() ? nullptr : &*found;
            };
            const auto* first = image_of(0);
            bool normals = false;
            for (uint32_t i = 1; i < info->colorAttachmentCount; ++i) {
                const auto* other = image_of(i);
                normals = normals || (other && other->second.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
            }
            if (first && first->second.format == VK_FORMAT_R8G8B8A8_UNORM && normals &&
                    scene_shaped({0, first->second.format, first->second.extent})) {
                recording.albedo = first->first;
                recording.albedo_layout = info->pColorAttachments[0].imageLayout;
            }
        }
        if (!info->pDepthAttachment || !info->pDepthAttachment->imageView) return given;
        auto view = views.find(info->pDepthAttachment->imageView);
        if (view == views.end()) return given;
        recording.depth = view->second;
        recording.layout = info->pDepthAttachment->imageLayout;
        recording.stored = info->pDepthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
        recording.suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
        // The scene's first pass: the screen-shaped depth cleared.
        auto image = followed.find(view->second);
        scene = image != followed.end() && scene_shaped(image->second) &&
            info->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
        recording.scene = scene;
    }
    if (scene) gpu::scene_begun(buffer);
    return given;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    thread_local Redirected redirected;
    next.CmdBeginRendering(buffer, begin_rendering(buffer, info, redirected));
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    thread_local Redirected redirected;
    next.CmdBeginRenderingKHR(buffer, begin_rendering(buffer, info, redirected));
}

// After the game's pass ends: its scene depth copied, when it wrote one.
void end_rendering(VkCommandBuffer buffer) {
    std::lock_guard lock(depth_mutex);
    Recording& recording = recording_of(buffer);
    if (recording.scene) {
        recording.scene = false;
        gpu::scene_ended(buffer);
    }
    if (recording.albedo) {
        auto found = colors.find(recording.albedo);
        if (found != colors.end() && make_albedo(found->second.extent)) {
            record_albedo(buffer, recording.albedo, recording.albedo_layout, found->second.extent);
            recording.albedo_copied = true;
        }
        recording.albedo = VK_NULL_HANDLE;
    }
    const VkImage image = recording.depth;
    recording.depth = VK_NULL_HANDLE;
    if (!image || !recording.stored || recording.suspending) return;
    auto found = followed.find(image);
    if (found == followed.end() || !scene_shaped(found->second)) return;
    if (!make_copy(found->second.extent, found->second.format)) return;
    record_copy(buffer, image, recording.layout);
    recording.copied = true;
    recording.has_camera = local_camera_now(recording.camera);
}

// The game's barriers: right after the one making its HUD blur readable
// (written by compute: GENERAL to SHADER_READ_ONLY), ours copied over it.
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer buffer, VkPipelineStageFlags source,
        VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier* memory,
        uint32_t buffer_count, const VkBufferMemoryBarrier* buffers, uint32_t image_count,
        const VkImageMemoryBarrier* images) {
    next.CmdPipelineBarrier(buffer, source, destination, flags, memory_count, memory, buffer_count, buffers, image_count,
        images);
    if (!image_count || !blur_filled.load(std::memory_order_acquire)) return;
    std::lock_guard lock(depth_mutex);
    if (!own_blur.image) return;
    for (uint32_t i = 0; i < image_count; ++i) {
        const VkImageMemoryBarrier& barrier = images[i];
        if (barrier.newLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                !(barrier.srcAccessMask & VK_ACCESS_SHADER_WRITE_BIT))
            continue;
        auto found = blur_candidates.find(barrier.image);
        if (found == blur_candidates.end() || !hud_blur_shaped(found->second) ||
                found->second.width != own_blur.extent.width || found->second.height != own_blur.extent.height)
            continue;
        record_blur_copy(buffer, barrier.image);
        if (!own_blur.said) {
            own_blur.said = true;
            log_line("HUD blur: ours (the scene with the world items) copied over the game's %ux%u blur",
                own_blur.extent.width, own_blur.extent.height);
        }
        return;
    }
}

VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer buffer) {
    next.CmdEndRendering(buffer);
    end_rendering(buffer);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderingKHR(VkCommandBuffer buffer) {
    next.CmdEndRenderingKHR(buffer);
    end_rendering(buffer);
}

// game-depth.on's light_probe, read before taking depth_mutex.
std::atomic<bool> light_probe_on{false};

void submitted(VkCommandBuffer buffer) {
    // Called under depth_mutex.
    // The scene's light, in any command buffer (the deferred lighting's own).
    if (copy.has_camera && light_probe_on) gpu::light_submitted(buffer, copy.camera.position);
    auto found = recordings.find(buffer);
    if (found != recordings.end() && found->second.albedo_copied) albedo.submitted_at = presents;
    if (found != recordings.end())
        for (VkImage image : found->second.layers) {
            auto layer = layers.find(image);
            if (layer != layers.end() && layer->second.begun) layer->second.submitted = true;
        }
    if (found == recordings.end() || !found->second.copied) return;
    copy.submitted_at = presents;
    copy.camera = found->second.camera;
    copy.has_camera = found->second.has_camera;
    gpu::Camera camera;
    if (found->second.has_camera && gpu::submitted(buffer, camera, found->second.camera.position))
        copy.gpu_camera = camera;
    else
        copy.gpu_camera.valid = false;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    light_probe_on = settings().light_probe;
    {
        std::lock_guard lock(depth_mutex);
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j) submitted(submits[i].pCommandBuffers[j]);
    }
    return next.QueueSubmit(queue, count, submits, fence);
}

void submitted2(uint32_t count, const VkSubmitInfo2* submits) {
    light_probe_on = settings().light_probe;
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

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    const VkResult result = next.CreateBuffer(device, info, allocator, buffer);
    if (result == VK_SUCCESS) gpu::buffer_created(*buffer, info->size);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator) {
    gpu::buffer_destroyed(buffer);
    next.DestroyBuffer(device, buffer, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
        VkDeviceSize offset) {
    const VkResult result = next.BindBufferMemory(device, buffer, memory, offset);
    if (result == VK_SUCCESS) gpu::buffer_bound(buffer, memory, offset);
    return result;
}

void bound2(uint32_t count, const VkBindBufferMemoryInfo* infos) {
    for (uint32_t i = 0; i < count; ++i) gpu::buffer_bound(infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    const VkResult result = next.BindBufferMemory2(device, count, infos);
    if (result == VK_SUCCESS) bound2(count, infos);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    const VkResult result = next.BindBufferMemory2KHR(device, count, infos);
    if (result == VK_SUCCESS) bound2(count, infos);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
        const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    const VkResult result = next.AllocateMemory(device, info, allocator, memory);
    if (result == VK_SUCCESS) {
        std::lock_guard lock(depth_mutex);
        memory_sizes[*memory] = info->allocationSize;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
        VkMemoryMapFlags flags, void** data) {
    const VkResult result = next.MapMemory(device, memory, offset, size, flags, data);
    if (result != VK_SUCCESS || !data || !*data) return result;
    if (size == VK_WHOLE_SIZE) {
        std::lock_guard lock(depth_mutex);
        auto known = memory_sizes.find(memory);
        size = known != memory_sizes.end() && known->second > offset ? known->second - offset : 0;
    }
    if (size) gpu::memory_mapped(memory, *data, offset, size);
    return result;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice device, VkDeviceMemory memory) {
    gpu::memory_unmapped(memory);
    next.UnmapMemory(device, memory);
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    gpu::memory_unmapped(memory);
    {
        std::lock_guard lock(depth_mutex);
        memory_sizes.erase(memory);
    }
    next.FreeMemory(device, memory, allocator);
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, uint32_t write_count, const VkWriteDescriptorSet* writes,
        uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    gpu::descriptors_written(write_count, writes);
    next.UpdateDescriptorSets(device, write_count, writes, copy_count, copies);
}

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer buffer, VkPipelineBindPoint point,
        VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet* sets, uint32_t dynamic_count,
        const uint32_t* dynamic) {
    gpu::sets_bound(buffer, point, first, count, sets, dynamic_count, dynamic);
    next.CmdBindDescriptorSets(buffer, point, layout, first, count, sets, dynamic_count, dynamic);
}

VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer buffer, VkPipelineLayout layout, VkShaderStageFlags stages,
        uint32_t offset, uint32_t size, const void* values) {
    gpu::constants_pushed(buffer, offset, size, values);
    next.CmdPushConstants(buffer, layout, stages, offset, size, values);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    const VkDeviceAddress address = next.GetBufferDeviceAddress(device, info);
    gpu::buffer_address(info->buffer, address);
    return address;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    const VkDeviceAddress address = next.GetBufferDeviceAddressKHR(device, info);
    gpu::buffer_address(info->buffer, address);
    return address;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddressEXT(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    const VkDeviceAddress address = next.GetBufferDeviceAddressEXT(device, info);
    gpu::buffer_address(info->buffer, address);
    return address;
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

void start(DeviceData& d, PFN_vkGetDeviceProcAddr next_gdpa) {
    if (!enabled()) return;
    if (device_data) {
        log_line("game depth: another device %p, not followed", static_cast<void*>(d.device));
        return;
    }
    device_data = &d;
#define DEPTH_LOAD(name, type) next.name = reinterpret_cast<PFN_##type>(next_gdpa(d.device, #type));
    DEPTH_FUNCTIONS(DEPTH_LOAD)
#undef DEPTH_LOAD
    next.CmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(next_gdpa(d.device, "vkCmdCopyImage"));
    next.CmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(next_gdpa(d.device, "vkCmdBlitImage"));
    next.CmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(next_gdpa(d.device, "vkCmdClearColorImage"));
    if (!next.CmdClearColorImage) layer_failed = true;
    log_line("game depth: on (game-depth.on), device %p", static_cast<void*>(d.device));
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

void note_screen(VkExtent2D extent) {
    if (!enabled()) return;
    std::lock_guard lock(depth_mutex);
    screen = extent;
}

void note_swapchain(const std::vector<VkImage>& images, VkFormat format) {
    if (!enabled()) return;
    // The old images' layers, once the GPU is done with them (a new swapchain is rare).
    bool any = false;
    {
        std::lock_guard lock(depth_mutex);
        any = !layers.empty();
    }
    if (any && device_data) device_data->fn.DeviceWaitIdle(device_data->device);
    std::lock_guard lock(depth_mutex);
    if (any && device_data) destroy_layers();
    presentable.clear();
    presentable.insert(images.begin(), images.end());
    presentable_format = format;
}

SceneDepth scene_depth(VkExtent2D) {
    SceneDepth found;
    if (!enabled()) return found;
    std::lock_guard lock(depth_mutex);
    // A copy submitted since the last present or the one before.
    if (!copy.image || presents - copy.submitted_at > 1) return found;
    found = {copy.image, copy.format, copy.extent, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, copy.id, copy.camera,
        copy.has_camera, copy.gpu_camera};
    if (albedo.image && presents - albedo.submitted_at <= 1) {
        found.albedo = albedo.image;
        found.albedo_extent = albedo.extent;
        found.albedo_id = albedo.id;
    }
    return found;
}

void want_interface_layer(bool wanted) {
    if (!enabled()) return;
    layer_wanted.store(wanted, std::memory_order_release);
}

HudBlur hud_blur_target() {
    if (!enabled()) return {};
    std::lock_guard lock(depth_mutex);
    if (!own_blur.image)
        for (auto& [image, extent] : blur_candidates)
            if (hud_blur_shaped(extent)) {
                make_own_blur(extent);
                break;
            }
    if (!own_blur.image || !hud_blur_shaped(own_blur.extent)) return {};
    return {own_blur.image, own_blur.extent};
}

void hud_blur_filled(bool filled) {
    if (!enabled()) return;
    blur_filled.store(filled, std::memory_order_release);
}

InterfaceLayer take_interface_layer(VkImage image) {
    if (!enabled()) return {};
    std::lock_guard lock(depth_mutex);
    auto found = layers.find(image);
    if (found == layers.end() || !found->second.begun || !found->second.submitted) return {};
    return {found->second.image, found->second.view, found->second.layout, found->second.id};
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
        if (name == "light_probe") read.light_probe = value > 0.0;
        if (name == "camera") {
            const std::string mode = word.substr(equals + 1);
            read.camera = mode == "present" ? CameraMode::Present : mode == "copy" ? CameraMode::Copy : CameraMode::Gpu;
        }
    }
    current = read;
    static const char* const kModes[] = {"the scene's, from the GPU", "read with the copy", "read at the present"};
    log_line("game depth: near %.4g m, margin %.3g m, camera %s (unless a world item says), light probe %s",
        current.near_distance, current.margin, kModes[static_cast<int>(current.camera)], current.light_probe ? "on" : "off");
    return current;
}

void presented(const std::vector<VkImage>& images) {
    if (!enabled()) return;
    std::lock_guard lock(depth_mutex);
    ++presents;
    for (VkImage image : images) {
        auto layer = layers.find(image);
        if (layer == layers.end()) continue;
        layer->second.begun = false;
        layer->second.submitted = false;
    }
}

}  // namespace overlay::depth
