// Shared declarations of the ShroudForge game overlay layer.
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <windows.h>
#undef CreateSemaphore

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define OVERLAY_EXPORT extern "C" __declspec(dllexport)

namespace overlay {

// log.cpp
bool is_game_process();
void log_line(const char* format, ...);
std::string module_name(const void* address);

template <typename T>
void* dispatch_key(T handle) {
    return *reinterpret_cast<void**>(handle);
}

struct InstanceData {
    VkInstance instance{};
    uint32_t api_version = VK_API_VERSION_1_0;
    PFN_vkGetInstanceProcAddr next_gipa{};
    PFN_vkDestroyInstance destroy_instance{};
};

// Device functions the layer calls itself, resolved from the next layer.
#define OVERLAY_DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) X(QueuePresentKHR) X(QueueSubmit) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CreateFence) \
    X(DestroyFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(CreateImageView) \
    X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateRenderPass) X(DestroyRenderPass) \
    X(CmdBeginRenderPass) X(CmdEndRenderPass) X(DeviceWaitIdle) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CmdPipelineBarrier) \
    X(CmdBlitImage) X(CmdCopyImageToBuffer) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateSampler) X(DestroySampler) X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdPushConstants) X(CmdSetViewport) \
    X(CmdSetScissor) X(CmdDrawIndexed) X(CmdCopyBufferToImage)

struct DeviceFunctions {
#define OVERLAY_DECLARE(name) PFN_vk##name name{};
    OVERLAY_DEVICE_FUNCTIONS(OVERLAY_DECLARE)
#undef OVERLAY_DECLARE
};

struct SwapchainData {
    VkFormat format{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    // Created on first draw, destroyed with the swapchain.
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    bool drawable = false;
};

struct Renderer;

struct DeviceData {
    VkDevice device{};
    VkPhysicalDevice physical_device{};
    InstanceData* instance{};
    PFN_vkGetDeviceProcAddr next_gdpa{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
    DeviceFunctions fn;

    // Guarded by state_mutex.
    std::unordered_map<VkQueue, uint32_t> queue_families;
    std::unordered_map<VkSwapchainKHR, SwapchainData> swapchains;

    // Owned by the present path, guarded by render_mutex.
    std::mutex render_mutex;
    Renderer* renderer{};
};

extern std::mutex state_mutex;

// renderer.cpp: draws the scene into the presented image. Returns the
// semaphore the real present must wait on, or null to present untouched.
VkSemaphore render_frame(DeviceData& d, VkQueue queue, const VkPresentInfoKHR& present);
void release_swapchain(DeviceData& d, SwapchainData& swapchain);
void destroy_renderer(DeviceData& d);

}  // namespace overlay
