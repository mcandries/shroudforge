// ShroudForge game overlay: an implicit Vulkan layer.
//
// The Vulkan loader inserts this layer between Enshrouded and the driver
// when SHROUDFORGE_OVERLAY_ENABLE=1 is set and VK_ADD_IMPLICIT_LAYER_PATH
// names its folder (the ShroudForge bootstrap sets both in the game process).
// The layer tracks the game's swapchains and, at each vkQueuePresentKHR,
// draws the zones published by mods (scene.cpp) into the presented image.
// With no zone, or anything unexpected, the game presents untouched.

#include "overlay.h"
#include "game_depth.h"
#include "render_probe.h"

#include <intrin.h>

#include <cstring>

namespace overlay {

std::mutex state_mutex;

namespace {

std::unordered_map<void*, InstanceData*> instances;
std::unordered_map<void*, DeviceData*> devices;

InstanceData* instance_of(void* key) {
    std::lock_guard lock(state_mutex);
    auto found = instances.find(key);
    return found == instances.end() ? nullptr : found->second;
}

DeviceData* device_of(void* key) {
    std::lock_guard lock(state_mutex);
    auto found = devices.find(key);
    return found == devices.end() ? nullptr : found->second;
}

bool drawable_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult result = next_create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    auto* data = new InstanceData{};
    data->instance = *instance;
    if (info->pApplicationInfo && info->pApplicationInfo->apiVersion) data->api_version = info->pApplicationInfo->apiVersion;
    data->next_gipa = next_gipa;
    data->destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
    {
        std::lock_guard lock(state_mutex);
        instances[dispatch_key(*instance)] = data;
    }
    log_line("instance %p created, api %u.%u", static_cast<void*>(*instance), VK_API_VERSION_MAJOR(data->api_version),
        VK_API_VERSION_MINOR(data->api_version));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    InstanceData* data = instance_of(dispatch_key(instance));
    if (!data) return;
    data->destroy_instance(instance, allocator);
    std::lock_guard lock(state_mutex);
    instances.erase(dispatch_key(instance));
    delete data;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    std::string below;
    for (auto* link = chain->u.pLayerInfo->pNext; link; link = link->pNext)
        below += module_name(reinterpret_cast<const void*>(link->pfnNextGetDeviceProcAddr)) + " ";
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkSetDeviceLoaderData set_loader_data{};
    for (auto* link = static_cast<const VkLayerDeviceCreateInfo*>(info->pNext); link;
            link = static_cast<const VkLayerDeviceCreateInfo*>(link->pNext)) {
        if (link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LOADER_DATA_CALLBACK)
            set_loader_data = link->u.pfnSetDeviceLoaderData;
    }
    auto next_create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    VkResult result = next_create(physical_device, info, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto* d = new DeviceData{};
    d->device = *device;
    d->physical_device = physical_device;
    d->instance = instance_of(dispatch_key(physical_device));
    d->next_gdpa = next_gdpa;
    d->set_loader_data = set_loader_data;
#define OVERLAY_LOAD(name) d->fn.name = reinterpret_cast<PFN_vk##name>(next_gdpa(*device, "vk" #name));
    OVERLAY_DEVICE_FUNCTIONS(OVERLAY_LOAD)
#undef OVERLAY_LOAD
    {
        std::lock_guard lock(state_mutex);
        devices[dispatch_key(*device)] = d;
    }
    probe::start(*device, next_gdpa);
    depth::start(*d, next_gdpa);
    log_line("device %p created, called from %s, layers below: %s", static_cast<void*>(*device),
        module_name(_ReturnAddress()).c_str(), below.c_str());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    DeviceData* d = device_of(dispatch_key(device));
    if (!d) return;
    {
        std::lock_guard render(d->render_mutex);
        for (auto& [handle, swapchain] : d->swapchains) release_swapchain(*d, swapchain);
        destroy_renderer(*d);
    }
    PFN_vkDestroyDevice destroy = d->fn.DestroyDevice;
    {
        std::lock_guard lock(state_mutex);
        devices.erase(dispatch_key(device));
    }
    delete d;
    destroy(device, allocator);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceData* d = device_of(dispatch_key(device));
    d->fn.GetDeviceQueue(device, family, index, queue);
    if (!*queue) return;
    std::lock_guard lock(state_mutex);
    d->queue_families[*queue] = family;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceData* d = device_of(dispatch_key(device));
    d->fn.GetDeviceQueue2(device, info, queue);
    if (!*queue) return;
    std::lock_guard lock(state_mutex);
    d->queue_families[*queue] = info->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
        const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    DeviceData* d = device_of(dispatch_key(device));
    // Copyable too, with the game's depth (the light view reads the image
    // the game shows); without it if the surface refuses.
    VkSwapchainCreateInfoKHR copyable = *info;
    if (depth::enabled()) copyable.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult result = d->fn.CreateSwapchainKHR(device, &copyable, allocator, swapchain);
    if (result != VK_SUCCESS && copyable.imageUsage != info->imageUsage) {
        copyable.imageUsage = info->imageUsage;
        result = d->fn.CreateSwapchainKHR(device, info, allocator, swapchain);
    }
    if (result != VK_SUCCESS) return result;
    SwapchainData data;
    data.copyable = (copyable.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    data.format = info->imageFormat;
    data.extent = info->imageExtent;
    uint32_t count = 0;
    d->fn.GetSwapchainImagesKHR(device, *swapchain, &count, nullptr);
    data.images.resize(count);
    d->fn.GetSwapchainImagesKHR(device, *swapchain, &count, data.images.data());
    probe::note_swapchain(data.images, info->imageFormat, info->imageExtent);
    depth::note_screen(info->imageExtent);
    // The zones are drawn in a render pass, as a color attachment.
    const bool attachment = (info->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    data.drawable = attachment && drawable_format(info->imageFormat) && info->imageArrayLayers == 1;
    log_line("swapchain %ux%u format %d usage 0x%x, %u images: %s", info->imageExtent.width, info->imageExtent.height,
        static_cast<int>(info->imageFormat), info->imageUsage, count, data.drawable ? "drawable" : "not drawn");
    std::lock_guard lock(state_mutex);
    d->swapchains[*swapchain] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    DeviceData* d = device_of(dispatch_key(device));
    {
        std::lock_guard render(d->render_mutex);
        std::lock_guard lock(state_mutex);
        auto found = d->swapchains.find(swapchain);
        if (found != d->swapchains.end()) {
            release_swapchain(*d, found->second);
            d->swapchains.erase(found);
        }
    }
    d->fn.DestroySwapchainKHR(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceData* d = device_of(dispatch_key(queue));
    probe::presented();
    VkSemaphore done = VK_NULL_HANDLE;
    {
        std::lock_guard render(d->render_mutex);
        done = render_frame(*d, queue, *info);
    }
    depth::presented();
    if (!done) return d->fn.QueuePresentKHR(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &done;
    return d->fn.QueuePresentKHR(queue, &patched);
}

#define INTERCEPT(name) if (std::strcmp(requested, "vk" #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&name)

PFN_vkVoidFunction device_function(const char* requested) {
    INTERCEPT(DestroyDevice);
    if (!is_game_process()) return nullptr;
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(QueuePresentKHR);
    if (PFN_vkVoidFunction own = depth::function(requested)) return own;
    return probe::function(requested);
}

}  // namespace
}  // namespace overlay

using namespace overlay;

OVERLAY_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShroudforgeOverlay_GetDeviceProcAddr(
        VkDevice device, const char* requested) {
    // Layers above us (Steam's fossilize) look this up once and route every
    // later lookup through it: answering with the next layer's would skip us.
    if (std::strcmp(requested, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShroudforgeOverlay_GetDeviceProcAddr);
    if (PFN_vkVoidFunction own = device_function(requested)) return own;
    DeviceData* d = device_of(dispatch_key(device));
    return d ? d->next_gdpa(device, requested) : nullptr;
}

OVERLAY_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShroudforgeOverlay_GetInstanceProcAddr(
        VkInstance instance, const char* requested) {
    INTERCEPT(CreateInstance);
    INTERCEPT(DestroyInstance);
    INTERCEPT(CreateDevice);
    if (std::strcmp(requested, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShroudforgeOverlay_GetInstanceProcAddr);
    if (std::strcmp(requested, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&ShroudforgeOverlay_GetDeviceProcAddr);
    if (PFN_vkVoidFunction own = device_function(requested)) return own;
    if (!instance) return nullptr;
    InstanceData* data = instance_of(dispatch_key(instance));
    return data ? data->next_gipa(instance, requested) : nullptr;
}

OVERLAY_EXPORT VKAPI_ATTR VkResult VKAPI_CALL ShroudforgeOverlay_NegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
    if (version->loaderLayerInterfaceVersion >= 2) {
        version->pfnGetInstanceProcAddr = &ShroudforgeOverlay_GetInstanceProcAddr;
        version->pfnGetDeviceProcAddr = &ShroudforgeOverlay_GetDeviceProcAddr;
        version->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}
