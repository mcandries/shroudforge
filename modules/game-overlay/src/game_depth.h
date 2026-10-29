// game_depth.cpp: the game's scene depth, copied as the game draws it (when a
// file `game-depth.on` sits next to the layer), for the overlay to read at
// the present.
#pragma once

#include "gpu_camera.h"
#include "local_camera.h"

#include <vulkan/vulkan.h>

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
};
SceneDepth scene_depth(VkExtent2D screen);

// Read from game-depth.on (`near=0.01 margin=0.15`), again when it changes:
// the game's near distance (its depth is near / distance) and how far behind
// the game's surface (meters) a world cell still shows.
enum class CameraMode { Gpu, Copy, Present };
struct Settings {
    double near_distance = 0.01;  // the GPU camera's, when found
    double margin = 0.15;
    // Cells to remove sit in the game's blocks, whose relief juts out.
    double margin_remove = 0.35;
    // Which camera draws the world items: `camera=gpu` (default), the
    // scene's as handed to the GPU (else as `copy`); `camera=copy`, the one
    // read as the game recorded the scene; `camera=present`, the one read at
    // the present, as without the game's depth.
    CameraMode camera = CameraMode::Gpu;
};
Settings settings();
// At each present, after the overlay drew: a frame ends.
void presented();

}  // namespace overlay::depth
