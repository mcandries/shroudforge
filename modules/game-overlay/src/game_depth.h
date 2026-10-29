// game_depth.cpp: the game's scene depth image, followed through its frames
// (when a file `game-depth.on` sits next to the layer), for the overlay to
// read it at the present.
#pragma once

#include <vulkan/vulkan.h>

namespace overlay::depth {

bool enabled();
// The game's device created: the next layer's functions taken.
void start(VkDevice device, PFN_vkGetDeviceProcAddr next_gdpa);
// The function of its own for a name, or null (off, or not one of its own).
PFN_vkVoidFunction function(const char* requested);

// This frame's scene depth: the largest depth attachment of the frame shaped
// like the screen (`screen`), in the layout the game left it in. `image` is
// null when there is none.
struct SceneDepth {
    VkImage image{};
    VkFormat format{};
    VkExtent2D extent{};
    VkImageLayout layout{};
    int id = 0;  // The image's number, for the logs.
};
SceneDepth scene_depth(VkExtent2D screen);

// Read from game-depth.on (`near=0.01 margin=0.15`), again when it changes:
// the game's near distance (its depth is near / distance) and how far behind
// the game's surface (meters) a world cell still shows.
struct Settings {
    double near_distance = 0.01;
    double margin = 0.15;
    bool show_picture = true;  // `picture=0`: no picture of the depth in a corner
};
Settings settings();
// At each present, after the overlay drew: a frame ends.
void presented();

}  // namespace overlay::depth
