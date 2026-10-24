// Manual check for the overlay layer: opens a window, presents a few blue
// frames through a real swapchain, then reads back the last presented image
// and prints the pixel inside the square and one outside it.
//
// Run with VK_ADD_LAYER_PATH pointing at the built layer and
// VK_INSTANCE_LAYERS=VK_LAYER_SHROUDFORGE_overlay.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { std::printf("%s failed: %d\n", #call, r_); return 1; } } while (0)

PFN_vkGetInstanceProcAddr gipa;
#define INSTANCE_FN(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
#define DEVICE_FN(name) auto name = reinterpret_cast<PFN_##name>(gdpa(device, #name))

int main() {
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return 1;
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    VkInstance instance = VK_NULL_HANDLE;
    INSTANCE_FN(vkCreateInstance);

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"overlay_present_test";
    RegisterClassW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"overlay present test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 640, 480, nullptr, nullptr, window_class.hInstance, nullptr);

    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "overlay present test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

    INSTANCE_FN(vkEnumeratePhysicalDevices);
    INSTANCE_FN(vkCreateWin32SurfaceKHR);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);
    INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    auto gdpa = vkGetDeviceProcAddr;

    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = window_class.hInstance;
    surface_info.hwnd = window;
    VkSurfaceKHR surface;
    CHECK(vkCreateWin32SurfaceKHR(instance, &surface_info, nullptr, &surface));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));

    DEVICE_FN(vkGetDeviceQueue);
    DEVICE_FN(vkCreateSwapchainKHR);
    DEVICE_FN(vkGetSwapchainImagesKHR);
    DEVICE_FN(vkAcquireNextImageKHR);
    DEVICE_FN(vkQueuePresentKHR);
    DEVICE_FN(vkQueueSubmit);
    DEVICE_FN(vkCreateCommandPool);
    DEVICE_FN(vkAllocateCommandBuffers);
    DEVICE_FN(vkBeginCommandBuffer);
    DEVICE_FN(vkEndCommandBuffer);
    DEVICE_FN(vkCmdPipelineBarrier);
    DEVICE_FN(vkCmdClearColorImage);
    DEVICE_FN(vkCmdCopyImageToBuffer);
    DEVICE_FN(vkCreateSemaphore);
    DEVICE_FN(vkCreateFence);
    DEVICE_FN(vkWaitForFences);
    DEVICE_FN(vkResetFences);
    DEVICE_FN(vkCreateBuffer);
    DEVICE_FN(vkGetBufferMemoryRequirements);
    DEVICE_FN(vkAllocateMemory);
    DEVICE_FN(vkBindBufferMemory);
    DEVICE_FN(vkMapMemory);
    DEVICE_FN(vkDeviceWaitIdle);

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
    VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = capabilities.minImageCount + 1;
    swapchain_info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swapchain_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchain_info.imageExtent = capabilities.currentExtent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain));
    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data());

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(device, &allocate, &cb));
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, rendered;
    vkCreateSemaphore(device, &semaphore_info, nullptr, &acquired);
    vkCreateSemaphore(device, &semaphore_info, nullptr, &rendered);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(device, &fence_info, nullptr, &fence);

    const VkExtent2D extent = capabilities.currentExtent;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = VkDeviceSize(extent.width) * extent.height * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readback;
    CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &readback));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, readback, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory_info.allocationSize = requirements.size;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            memory_info.memoryTypeIndex = i;
            break;
        }
    }
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &memory_info, nullptr, &memory));
    vkBindBufferMemory(device, readback, memory, 0);

    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr, 0, nullptr, 1, &b);
    };

    uint32_t last_index = 0;
    for (int frame = 0; frame <= 30; ++frame) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        uint32_t index;
        CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &begin);
        const bool check = frame == 30;
        if (check) {
            // Read back the image presented last frame: same index, contents kept.
            index = last_index;
        }
        if (!check) {
            barrier(images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue blue{{0.0f, 0.0f, 1.0f, 1.0f}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &range);
            barrier(images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else {
            barrier(images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
            barrier(images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        submit.signalSemaphoreCount = check ? 0 : 1;
        submit.pSignalSemaphores = &rendered;
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
        if (check) break;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        CHECK(vkQueuePresentKHR(queue, &present));
        last_index = index;
        vkDeviceWaitIdle(device);
    }

    void* mapped;
    vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    auto pixel = [&](uint32_t x, uint32_t y) {
        const auto* p = static_cast<const uint8_t*>(mapped) + (VkDeviceSize(y) * extent.width + x) * 4;
        std::printf("pixel %u,%u = B %u G %u R %u A %u\n", x, y, p[0], p[1], p[2], p[3]);
    };
    pixel(40, 40);
    pixel(200, 200);
    vkDeviceWaitIdle(device);
    DestroyWindow(window);
    return 0;
}
