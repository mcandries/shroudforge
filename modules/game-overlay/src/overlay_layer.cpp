// ShroudForge game overlay: an implicit Vulkan layer.
//
// The Vulkan loader inserts this layer between Enshrouded and the driver
// when SHROUDFORGE_OVERLAY_ENABLE=1 is set and VK_ADD_IMPLICIT_LAYER_PATH
// names its folder (the ShroudForge bootstrap sets both in the game process).
// At each vkQueuePresentKHR the layer records a few transfer commands that
// draw into the swapchain image, then presents.
//
// First step: a solid square in the top-left corner, to validate that the
// layer loads and draws without cost. Anything unexpected (unsupported
// format, a failed call) disables drawing for that swapchain or device and
// the game keeps presenting untouched.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <windows.h>

#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#define OVERLAY_EXPORT extern "C" __declspec(dllexport)

namespace {

constexpr uint32_t kSquareSize = 300;
constexpr int32_t kSquareOffset = 100;

// The environment that enables the layer is inherited by the processes the
// game starts (the ShroudForge UI and its WebView2 helpers, which probe
// Vulkan). The layer only draws and logs in the game itself.
bool is_game_process() {
    static const bool game = [] {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* separator = wcsrchr(path, L'\\');
        return _wcsicmp(separator ? separator + 1 : path, L"enshrouded.exe") == 0;
    }();
    return game;
}

// ---------------------------------------------------------------- logging

std::mutex log_mutex;

void log_line(const char* format, ...) {
    if (!is_game_process()) return;
    std::lock_guard lock(log_mutex);
    static FILE* file = [] {
        wchar_t path[MAX_PATH]{};
        HMODULE self{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&log_line), &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        wchar_t* dot = wcsrchr(path, L'.');
        if (dot) wcscpy_s(dot, MAX_PATH - (dot - path), L".log");
        FILE* opened = nullptr;
        _wfopen_s(&opened, path, L"a");
        return opened;
    }();
    if (!file) return;
    fprintf(file, "[%lu %llu] ", GetCurrentProcessId(), GetTickCount64());
    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
    fputc('\n', file);
    fflush(file);
}

// Diagnostics: the DLL that holds an address (a caller, or the next layer).
std::string module_name(const void* address) {
    HMODULE module{};
    if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module)) return "?";
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* separator = strrchr(path, '\\');
    return separator ? separator + 1 : path;
}

// ------------------------------------------------------- dispatch tables

template <typename T>
void* dispatch_key(T handle) {
    return *reinterpret_cast<void**>(handle);
}

struct InstanceData {
    VkInstance instance{};
    PFN_vkGetInstanceProcAddr next_gipa{};
    PFN_vkDestroyInstance destroy_instance{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities{};
};

struct SwapchainData {
    VkFormat format{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    bool drawable = false;
};

struct Frame {
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    VkSemaphore done{};
    bool submitted = false;
};

struct DeviceData {
    VkDevice device{};
    VkPhysicalDevice physical_device{};
    InstanceData* instance{};
    PFN_vkGetDeviceProcAddr next_gdpa{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
    bool broken = false;
    bool drawn_once = false;
    bool unknown_present_logged = false;

    PFN_vkDestroyDevice destroy_device{};
    PFN_vkGetDeviceQueue get_device_queue{};
    PFN_vkGetDeviceQueue2 get_device_queue2{};
    PFN_vkCreateSwapchainKHR create_swapchain{};
    PFN_vkDestroySwapchainKHR destroy_swapchain{};
    PFN_vkGetSwapchainImagesKHR get_swapchain_images{};
    PFN_vkQueuePresentKHR queue_present{};
    PFN_vkQueueSubmit queue_submit{};
    PFN_vkCreateCommandPool create_command_pool{};
    PFN_vkDestroyCommandPool destroy_command_pool{};
    PFN_vkAllocateCommandBuffers allocate_command_buffers{};
    PFN_vkBeginCommandBuffer begin_command_buffer{};
    PFN_vkEndCommandBuffer end_command_buffer{};
    PFN_vkResetCommandBuffer reset_command_buffer{};
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image{};
    PFN_vkCreateFence create_fence{};
    PFN_vkDestroyFence destroy_fence{};
    PFN_vkWaitForFences wait_for_fences{};
    PFN_vkResetFences reset_fences{};
    PFN_vkCreateSemaphore create_semaphore{};
    PFN_vkDestroySemaphore destroy_semaphore{};
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkBindBufferMemory bind_buffer_memory{};
    PFN_vkMapMemory map_memory{};
    PFN_vkUnmapMemory unmap_memory{};
    PFN_vkDeviceWaitIdle device_wait_idle{};

    std::unordered_map<VkQueue, uint32_t> queue_families;
    std::unordered_map<VkSwapchainKHR, SwapchainData> swapchains;
    // Per queue family: one command pool and a small ring of frames.
    std::unordered_map<uint32_t, VkCommandPool> pools;
    std::unordered_map<uint32_t, std::vector<Frame>> frames;
    std::unordered_map<uint32_t, size_t> next_frame;
    // Square pixels, one buffer per byte order.
    VkBuffer rgba_buffer{};
    VkBuffer bgra_buffer{};
    VkDeviceMemory pixel_memory{};
};

std::mutex state_mutex;
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

// ------------------------------------------------------------- drawing

bool is_rgba(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
}

bool is_bgra(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool create_pixels(DeviceData& d) {
    const VkDeviceSize size = kSquareSize * kSquareSize * 4;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.create_buffer(d.device, &buffer_info, nullptr, &d.rgba_buffer) != VK_SUCCESS) return false;
    if (d.create_buffer(d.device, &buffer_info, nullptr, &d.bgra_buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements requirements{};
    d.get_buffer_memory_requirements(d.device, d.rgba_buffer, &requirements);
    const VkDeviceSize stride = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;
    VkPhysicalDeviceMemoryProperties properties{};
    d.instance->get_memory_properties(d.physical_device, &properties);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = stride * 2;
    allocate.memoryTypeIndex = type;
    if (d.allocate_memory(d.device, &allocate, nullptr, &d.pixel_memory) != VK_SUCCESS) return false;
    if (d.bind_buffer_memory(d.device, d.rgba_buffer, d.pixel_memory, 0) != VK_SUCCESS) return false;
    if (d.bind_buffer_memory(d.device, d.bgra_buffer, d.pixel_memory, stride) != VK_SUCCESS) return false;

    void* mapped = nullptr;
    if (d.map_memory(d.device, d.pixel_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
    // ShroudForge gold, fully opaque.
    const uint8_t r = 0xE2, g = 0xB0, b = 0x40, a = 0xFF;
    auto* rgba = static_cast<uint8_t*>(mapped);
    auto* bgra = rgba + stride;
    for (uint32_t i = 0; i < kSquareSize * kSquareSize; ++i) {
        rgba[i * 4 + 0] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b; rgba[i * 4 + 3] = a;
        bgra[i * 4 + 0] = b; bgra[i * 4 + 1] = g; bgra[i * 4 + 2] = r; bgra[i * 4 + 3] = a;
    }
    d.unmap_memory(d.device, d.pixel_memory);
    return true;
}

Frame* acquire_frame(DeviceData& d, uint32_t family) {
    auto& pool = d.pools[family];
    if (!pool) {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = family;
        if (d.create_command_pool(d.device, &info, nullptr, &pool) != VK_SUCCESS) return nullptr;
        auto& ring = d.frames[family];
        ring.resize(4);
        for (auto& frame : ring) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            if (d.allocate_command_buffers(d.device, &allocate, &frame.command_buffer) != VK_SUCCESS) return nullptr;
            // Command buffers allocated by a layer need the loader's dispatch pointer.
            if (d.set_loader_data(d.device, frame.command_buffer) != VK_SUCCESS) return nullptr;
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (d.create_fence(d.device, &fence, nullptr, &frame.fence) != VK_SUCCESS) return nullptr;
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (d.create_semaphore(d.device, &semaphore, nullptr, &frame.done) != VK_SUCCESS) return nullptr;
        }
    }
    auto& ring = d.frames[family];
    Frame& frame = ring[d.next_frame[family]++ % ring.size()];
    if (frame.submitted) {
        if (d.wait_for_fences(d.device, 1, &frame.fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) return nullptr;
        d.reset_fences(d.device, 1, &frame.fence);
        frame.submitted = false;
    }
    return &frame;
}

void image_barrier(DeviceData& d, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
        VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}

// Records and submits the drawing for one present. Returns the semaphore the
// real present must wait on instead of the game's, or null to present as is.
VkSemaphore draw(DeviceData& d, VkQueue queue, const VkPresentInfoKHR& present) {
    if (d.broken) return VK_NULL_HANDLE;
    auto family = d.queue_families.find(queue);
    const bool known_swapchain = present.swapchainCount > 0 && d.swapchains.count(present.pSwapchains[0]) > 0;
    if ((family == d.queue_families.end() || !known_swapchain) && !d.unknown_present_logged) {
        d.unknown_present_logged = true;
        log_line("overlay: present with queue known %d, swapchain known %d",
            family != d.queue_families.end() ? 1 : 0, known_swapchain ? 1 : 0);
    }
    if (family == d.queue_families.end()) return VK_NULL_HANDLE;
    if (!d.pixel_memory && !create_pixels(d)) {
        log_line("overlay: pixel buffer unavailable, drawing disabled");
        d.broken = true;
        return VK_NULL_HANDLE;
    }

    std::vector<std::pair<SwapchainData*, uint32_t>> targets;
    for (uint32_t i = 0; i < present.swapchainCount; ++i) {
        auto found = d.swapchains.find(present.pSwapchains[i]);
        if (found == d.swapchains.end() || !found->second.drawable) continue;
        if (present.pImageIndices[i] >= found->second.images.size()) continue;
        targets.emplace_back(&found->second, present.pImageIndices[i]);
    }
    if (targets.empty()) return VK_NULL_HANDLE;

    Frame* frame = acquire_frame(d, family->second);
    if (!frame) {
        log_line("overlay: no command buffer, drawing disabled");
        d.broken = true;
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer cb = frame->command_buffer;
    d.reset_command_buffer(cb, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.begin_command_buffer(cb, &begin);
    for (auto& [swapchain, index] : targets) {
        VkImage image = swapchain->images[index];
        image_barrier(d, cb, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {kSquareOffset, kSquareOffset, 0};
        region.imageExtent = {kSquareSize, kSquareSize, 1};
        d.cmd_copy_buffer_to_image(cb, is_bgra(swapchain->format) ? d.bgra_buffer : d.rgba_buffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(d, cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
    }
    d.end_command_buffer(cb);

    std::vector<VkPipelineStageFlags> stages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = present.waitSemaphoreCount;
    submit.pWaitSemaphores = present.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame->done;
    if (d.queue_submit(queue, 1, &submit, frame->fence) != VK_SUCCESS) {
        log_line("overlay: submit failed, drawing disabled");
        d.broken = true;
        return VK_NULL_HANDLE;
    }
    frame->submitted = true;
    if (!d.drawn_once) {
        d.drawn_once = true;
        log_line("overlay: first square drawn");
    }
    return frame->done;
}

// -------------------------------------------------------- intercepted calls

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkInstance* instance) {
    auto* chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    std::string below;
    for (auto* link = chain->u.pLayerInfo; link; link = link->pNext)
        below += module_name(reinterpret_cast<const void*>(link->pfnNextGetInstanceProcAddr)) + " ";
    log_line("overlay: CreateInstance called from %s; layers below: %s",
        module_name(_ReturnAddress()).c_str(), below.c_str());
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult result = next_create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    auto* data = new InstanceData{};
    data->instance = *instance;
    data->next_gipa = next_gipa;
    data->destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
    data->get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        next_gipa(*instance, "vkGetPhysicalDeviceMemoryProperties"));
    data->get_surface_capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        next_gipa(*instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    {
        std::lock_guard lock(state_mutex);
        instances[dispatch_key(*instance)] = data;
    }
    log_line("overlay: instance %p created for '%s', %u layers, %u extensions", static_cast<void*>(*instance),
        info->pApplicationInfo && info->pApplicationInfo->pApplicationName ? info->pApplicationInfo->pApplicationName : "",
        info->enabledLayerCount, info->enabledExtensionCount);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    InstanceData* data = instance_of(dispatch_key(instance));
    if (!data) return;
    data->destroy_instance(instance, allocator);
    log_line("overlay: instance %p destroyed", static_cast<void*>(instance));
    std::lock_guard lock(state_mutex);
    instances.erase(dispatch_key(instance));
    delete data;
}

template <typename T>
void load(DeviceData& d, T& slot, const char* name) {
    slot = reinterpret_cast<T>(d.next_gdpa(d.device, name));
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
    for (auto* link = chain->u.pLayerInfo; link; link = link->pNext)
        below += module_name(reinterpret_cast<const void*>(link->pfnNextGetDeviceProcAddr)) + " ";
    log_line("overlay: CreateDevice called from %s; layers below: %s",
        module_name(_ReturnAddress()).c_str(), below.c_str());
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
    load(*d, d->destroy_device, "vkDestroyDevice");
    load(*d, d->get_device_queue, "vkGetDeviceQueue");
    load(*d, d->get_device_queue2, "vkGetDeviceQueue2");
    load(*d, d->create_swapchain, "vkCreateSwapchainKHR");
    load(*d, d->destroy_swapchain, "vkDestroySwapchainKHR");
    load(*d, d->get_swapchain_images, "vkGetSwapchainImagesKHR");
    load(*d, d->queue_present, "vkQueuePresentKHR");
    load(*d, d->queue_submit, "vkQueueSubmit");
    load(*d, d->create_command_pool, "vkCreateCommandPool");
    load(*d, d->destroy_command_pool, "vkDestroyCommandPool");
    load(*d, d->allocate_command_buffers, "vkAllocateCommandBuffers");
    load(*d, d->begin_command_buffer, "vkBeginCommandBuffer");
    load(*d, d->end_command_buffer, "vkEndCommandBuffer");
    load(*d, d->reset_command_buffer, "vkResetCommandBuffer");
    load(*d, d->cmd_pipeline_barrier, "vkCmdPipelineBarrier");
    load(*d, d->cmd_copy_buffer_to_image, "vkCmdCopyBufferToImage");
    load(*d, d->create_fence, "vkCreateFence");
    load(*d, d->destroy_fence, "vkDestroyFence");
    load(*d, d->wait_for_fences, "vkWaitForFences");
    load(*d, d->reset_fences, "vkResetFences");
    load(*d, d->create_semaphore, "vkCreateSemaphore");
    load(*d, d->destroy_semaphore, "vkDestroySemaphore");
    load(*d, d->create_buffer, "vkCreateBuffer");
    load(*d, d->destroy_buffer, "vkDestroyBuffer");
    load(*d, d->get_buffer_memory_requirements, "vkGetBufferMemoryRequirements");
    load(*d, d->allocate_memory, "vkAllocateMemory");
    load(*d, d->free_memory, "vkFreeMemory");
    load(*d, d->bind_buffer_memory, "vkBindBufferMemory");
    load(*d, d->map_memory, "vkMapMemory");
    load(*d, d->unmap_memory, "vkUnmapMemory");
    load(*d, d->device_wait_idle, "vkDeviceWaitIdle");
    d->broken = !d->instance || !d->set_loader_data || !d->queue_present || !d->create_swapchain;
    {
        std::lock_guard lock(state_mutex);
        devices[dispatch_key(*device)] = d;
    }
    bool swapchain_extension = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        swapchain_extension |= std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    log_line("overlay: device %p created, %u extensions, swapchain extension %d", static_cast<void*>(*device),
        info->enabledExtensionCount, swapchain_extension ? 1 : 0);
    log_line("overlay: device created, loader callback %d, swapchain functions %d%s", d->set_loader_data ? 1 : 0,
        d->create_swapchain && d->queue_present ? 1 : 0, d->broken ? " (drawing disabled)" : "");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    DeviceData* d = device_of(dispatch_key(device));
    if (!d) return;
    if (d->device_wait_idle) d->device_wait_idle(device);
    for (auto& [family, ring] : d->frames) {
        for (auto& frame : ring) {
            if (frame.fence) d->destroy_fence(device, frame.fence, nullptr);
            if (frame.done) d->destroy_semaphore(device, frame.done, nullptr);
        }
    }
    for (auto& [family, pool] : d->pools) if (pool) d->destroy_command_pool(device, pool, nullptr);
    if (d->rgba_buffer) d->destroy_buffer(device, d->rgba_buffer, nullptr);
    if (d->bgra_buffer) d->destroy_buffer(device, d->bgra_buffer, nullptr);
    if (d->pixel_memory) d->free_memory(device, d->pixel_memory, nullptr);
    PFN_vkDestroyDevice destroy = d->destroy_device;
    {
        std::lock_guard lock(state_mutex);
        devices.erase(dispatch_key(device));
    }
    delete d;
    destroy(device, allocator);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceData* d = device_of(dispatch_key(device));
    d->get_device_queue(device, family, index, queue);
    if (!*queue) return;
    std::lock_guard lock(state_mutex);
    d->queue_families[*queue] = family;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceData* d = device_of(dispatch_key(device));
    d->get_device_queue2(device, info, queue);
    if (!*queue) return;
    std::lock_guard lock(state_mutex);
    d->queue_families[*queue] = info->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
        const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    DeviceData* d = device_of(dispatch_key(device));
    // The square is copied into the swapchain images, which needs the transfer
    // destination usage. Add it when the game did not ask for it and the
    // surface allows it.
    VkSwapchainCreateInfoKHR patched = *info;
    VkSurfaceCapabilitiesKHR capabilities{};
    if (!(patched.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && d->instance->get_surface_capabilities &&
            d->instance->get_surface_capabilities(d->physical_device, info->surface, &capabilities) == VK_SUCCESS &&
            (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult result = d->create_swapchain(device, &patched, allocator, swapchain);
    if (result != VK_SUCCESS && patched.imageUsage != info->imageUsage) {
        log_line("overlay: swapchain with transfer usage refused (%d), created as requested", static_cast<int>(result));
        patched.imageUsage = info->imageUsage;
        result = d->create_swapchain(device, info, allocator, swapchain);
    }
    if (result != VK_SUCCESS) return result;
    SwapchainData data;
    data.format = info->imageFormat;
    data.extent = info->imageExtent;
    uint32_t count = 0;
    d->get_swapchain_images(device, *swapchain, &count, nullptr);
    data.images.resize(count);
    d->get_swapchain_images(device, *swapchain, &count, data.images.data());
    log_line("overlay: swapchain usage requested 0x%x, used 0x%x", info->imageUsage, patched.imageUsage);
    const bool transfer = (patched.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    const bool format_ok = is_rgba(info->imageFormat) || is_bgra(info->imageFormat);
    const bool fits = info->imageExtent.width >= kSquareSize + kSquareOffset &&
        info->imageExtent.height >= kSquareSize + kSquareOffset;
    data.drawable = transfer && format_ok && fits && info->imageArrayLayers == 1;
    log_line("overlay: swapchain %ux%u format %d, %u images, transfer %d: %s", info->imageExtent.width,
        info->imageExtent.height, static_cast<int>(info->imageFormat), count, transfer ? 1 : 0,
        data.drawable ? "drawing" : "not drawn");
    std::lock_guard lock(state_mutex);
    d->swapchains[*swapchain] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    DeviceData* d = device_of(dispatch_key(device));
    {
        std::lock_guard lock(state_mutex);
        d->swapchains.erase(swapchain);
    }
    d->destroy_swapchain(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceData* d = device_of(dispatch_key(queue));
    VkSemaphore done = VK_NULL_HANDLE;
    {
        std::lock_guard lock(state_mutex);
        done = draw(*d, queue, *info);
    }
    if (!done) return d->queue_present(queue, info);
    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &done;
    return d->queue_present(queue, &patched);
}

#define INTERCEPT(name) if (std::strcmp(requested, "vk" #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&name)

void trace_request(const char* where, const void* handle, const char* requested, const void* caller) {
    if (std::strstr(requested, "Swapchain") || std::strstr(requested, "Present"))
        log_line("overlay: %s(%p, %s) from %s", where, handle, requested, module_name(caller).c_str());
}

PFN_vkVoidFunction device_function(const char* requested) {
    INTERCEPT(DestroyDevice);
    if (!is_game_process()) return nullptr;
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    INTERCEPT(CreateSwapchainKHR);
    INTERCEPT(DestroySwapchainKHR);
    INTERCEPT(QueuePresentKHR);
    return nullptr;
}

}  // namespace

OVERLAY_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShroudforgeOverlay_GetDeviceProcAddr(
        VkDevice device, const char* requested) {
    trace_request("GetDeviceProcAddr", device, requested, _ReturnAddress());
    if (PFN_vkVoidFunction own = device_function(requested)) return own;
    DeviceData* d = device_of(dispatch_key(device));
    return d ? d->next_gdpa(device, requested) : nullptr;
}

OVERLAY_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ShroudforgeOverlay_GetInstanceProcAddr(
        VkInstance instance, const char* requested) {
    trace_request("GetInstanceProcAddr", instance, requested, _ReturnAddress());
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
    log_line("overlay: loader interface version %u", version->loaderLayerInterfaceVersion);
    return VK_SUCCESS;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) log_line("overlay: layer loaded");
    if (reason == DLL_PROCESS_DETACH) log_line("overlay: layer unloaded");
    return TRUE;
}
