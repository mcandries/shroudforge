// render_probe.cpp: the game's render passes logged for a few frames, when a
// file `render-probe.on` sits next to the layer (a diagnostic).
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace overlay::probe {

bool enabled();
// The game's device created: the next layer's functions taken.
void start(VkDevice device, PFN_vkGetDeviceProcAddr next_gdpa);
// The probe's own function for a name, or null (off, or not one of its own).
PFN_vkVoidFunction function(const char* requested);
void note_swapchain(const std::vector<VkImage>& images, VkFormat format, VkExtent2D extent);
// At each present: a frame ends.
void presented();

}  // namespace overlay::probe
