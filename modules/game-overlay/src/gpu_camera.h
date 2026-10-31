// gpu_camera.cpp: the camera the game renders its scene with, read where it
// hands it to the GPU (with game-depth.on), for world items to be drawn with
// the very camera of the image. Fed by game_depth.cpp's hooks.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace overlay::depth::gpu {

// The view-projection matrix of the scene (16 float, row-major, world
// translation; reversed infinite depth: row 2 is 0, 0, 0, near) and the eye.
struct Camera {
    float matrix[16]{};
    double eye[3]{};         // the projection's centre, solved from the matrix
    double block_eye[3]{};   // the 3 floats after it in the block (not quite the centre)
    bool valid = false;
};

// The scene's sun and light factors (keen::RenderForwardConstants or
// keen::DeferredLightingConstants of the game's reflection: the same
// ForwardDirectionalLightData and RenderLightFactors in both).
struct Light {
    float direction[3]{};  // toward the sun, or from it (to be seen)
    float color[3]{};
    float sun = 0.0f, point_lights = 0.0f, gi = 0.0f, emissive = 0.0f, exposure = 0.0f, exposure_correction = 0.0f;
    bool valid = false;
};
// The last one read, if found.
bool light(Light& out);

void buffer_created(VkBuffer buffer, VkDeviceSize size);
void buffer_destroyed(VkBuffer buffer);
void buffer_bound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset);
void buffer_address(VkBuffer buffer, VkDeviceAddress address);
void memory_mapped(VkDeviceMemory memory, void* data, VkDeviceSize offset, VkDeviceSize size);
void memory_unmapped(VkDeviceMemory memory);
void descriptors_written(uint32_t count, const VkWriteDescriptorSet* writes);

void command_buffer_begun(VkCommandBuffer buffer);
void sets_bound(VkCommandBuffer buffer, VkPipelineBindPoint point, uint32_t first, uint32_t count,
    const VkDescriptorSet* sets, uint32_t dynamic_count, const uint32_t* dynamic);
void constants_pushed(VkCommandBuffer buffer, uint32_t offset, uint32_t size, const void* data);
// The scene's pass (the one that clears the screen-shaped depth) in a command buffer.
void scene_begun(VkCommandBuffer buffer);
void scene_ended(VkCommandBuffer buffer);
// A command buffer submitted: the scene's camera, when it held the scene's
// pass and the camera is found (searched now and then, else read where it
// was found).
bool submitted(VkCommandBuffer buffer, Camera& camera, const double input_eye[3]);

}  // namespace overlay::depth::gpu
