// game_depth.cpp: the game's scene depth, copied as the game draws it (when a
// file `game-depth.on` sits next to the layer), for the overlay to read at
// the present.
#pragma once

#include "gpu_camera.h"
#include "local_camera.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace overlay {
struct DeviceData;
}

namespace overlay::depth {

bool enabled();
// The game's device created: the next layer's functions taken.
void start(DeviceData& d, PFN_vkGetDeviceProcAddr next_gdpa);
// The function of its own for a name, or null (off, or not one of its own).
PFN_vkVoidFunction function(const char* requested);

// The swapchain's size: the scene's depth is shaped like it.
void note_screen(VkExtent2D extent);
// The swapchain's images: the game's interface may be drawn into a layer of
// ours instead of them (want_interface_layer).
void note_swapchain(const std::vector<VkImage>& images, VkFormat format);
// The last copy of the scene's depth (our image, in SHADER_READ_ONLY_OPTIMAL),
// when one was submitted since the last present or the one before; `image`
// is null otherwise.
struct SceneDepth {
    VkImage image{};
    VkFormat format{};
    VkExtent2D extent{};
    VkImageLayout layout{};
    int id = 0;  // The image's number, for the logs.
    // The camera read as the game recorded the pass copied: the camera of
    // the scene in the depth, when `has_camera`.
    LocalCamera camera;
    bool has_camera = false;
    // The scene's camera as the game handed it to the GPU, when found.
    gpu::Camera gpu_camera;
    // The light view: the color of the game's surfaces without light (its
    // G-buffer), made smaller (R8G8B8A8_UNORM, SHADER_READ_ONLY_OPTIMAL),
    // when a copy was submitted since the last present or the one before.
    VkImage albedo{};
    VkExtent2D albedo_extent{};
    int albedo_id = 0;
};
SceneDepth scene_depth(VkExtent2D screen);

// The game's interface layer. While the overlay wants it, the game's passes
// on a swapchain image, from its first one of the frame loading it (its
// interface, 31/10 capture: the scene is put there before, by a compute
// pass), draw into an image of ours instead, cleared to transparent black
// first: the interface's colors come out premultiplied, its alpha its
// coverage (the game blends its interface by its alpha, 31/10 capture). The
// swapchain image keeps the scene alone; the overlay lays the layer back over
// its world items. `wanted` is read as the game records its passes: the
// overlay says it after each frame it drew.
void want_interface_layer(bool wanted);
struct InterfaceLayer {
    VkImage image{};
    VkImageView view{};
    VkImageLayout layout{};  // the one the game's passes left it in
    int id = 0;              // the image's number, for the logs
};
// The layer drawn for swapchain image `image` and submitted since that image
// was last presented, or none (`image` null). The overlay must lay it back
// at this present: the game's interface is not on the image.
InterfaceLayer take_interface_layer(VkImage image);

// The light view's images: this many levels, each half the one before.
constexpr uint32_t kLightLevels = 4;
// Levels 1 to kLightLevels - 1 of `image` (level 0 in TRANSFER_DST_OPTIMAL,
// written) blitted from level 0 down; all left in SHADER_READ_ONLY_OPTIMAL.
void make_light_levels(VkCommandBuffer buffer, VkImage image, VkExtent2D extent, PFN_vkCmdPipelineBarrier barrier_fn,
    PFN_vkCmdBlitImage blit_fn);

// Read from game-depth.on (`near=0.01 margin=0.15`), again when it changes:
// the game's near distance (its depth is near / distance) and how far behind
// the game's surface (meters) a world cell still shows.
enum class CameraMode { Gpu, Copy, Present };
struct Settings {
    double near_distance = 0.01;  // unless a world item asks for the GPU matrix's (depth_near)
    // How far behind the game's surface a cell still shows. 0 (30/10): the
    // cells are drawn past their cells instead (face_offset), as the game's
    // blocks; more, the build shows through what stands just in front of it.
    double margin = 0.0;
    // Which camera draws the world items: `camera=gpu` (default), the
    // scene's as handed to the GPU (else as `copy`); `camera=copy`, the one
    // read as the game recorded the scene; `camera=present`, the one read at
    // the present, as without the game's depth.
    CameraMode camera = CameraMode::Gpu;
    // `light_probe=1`: the probe of the scene's sun and light factors in every
    // command buffer (gpu::light_submitted); off by default (31/10: not found
    // in the scene's pass, a lead set aside for the game's own lighting).
    bool light_probe = false;
};
Settings settings();
// At each present, after the overlay drew: a frame ends, `images` shown
// (their interface layers free for their next frame).
void presented(const std::vector<VkImage>& images);

}  // namespace overlay::depth
