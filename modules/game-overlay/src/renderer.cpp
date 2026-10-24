// Draws the scene with Dear ImGui into the image being presented.
//
// At a present with visible zones, the layer records one command buffer that
// runs a render pass over the swapchain image (load, draw, store), submits it
// after the game's wait semaphores and makes the real present wait on it. The
// ImGui backend is initialized on the first such present, with that queue.

#include "draw_scene.h"
#include "overlay.h"
#include "scene.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <cstring>
#include <fstream>
#include <iterator>

namespace overlay {

namespace {

constexpr uint32_t kFrames = 4;
constexpr wchar_t kDefaultFont[] = L"C:\\Windows\\Fonts\\segoeui.ttf";

struct FrameSlot {
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    VkSemaphore done{};
    bool submitted = false;
};

}  // namespace

struct Renderer {
    bool broken = false;
    uint32_t family = UINT32_MAX;
    VkQueue queue{};
    VkCommandPool pool{};
    FrameSlot frames[kFrames];
    uint32_t next_frame = 0;
    std::unordered_map<VkFormat, VkRenderPass> passes;
    VkFormat pipeline_format = VK_FORMAT_UNDEFINED;
    ImGuiContext* context{};
    bool backend_ready = false;
    std::map<std::string, ImFont*> fonts;
    ImFont* default_font{};
    std::vector<ImTextureData*> textures;
    ULONGLONG last_tick = 0;
    bool other_queue_logged = false;
    bool first_frame_logged = false;
};

namespace {

DeviceData* imgui_device = nullptr;

// ImGui allocates its own command buffer for texture uploads; like ours, it
// needs the loader's dispatch pointer, which the layers below rely on.
VKAPI_ATTR VkResult VKAPI_CALL imgui_allocate_command_buffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
        VkCommandBuffer* buffers) {
    VkResult result = imgui_device->fn.AllocateCommandBuffers(device, info, buffers);
    if (result != VK_SUCCESS) return result;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) imgui_device->set_loader_data(device, buffers[i]);
    return VK_SUCCESS;
}

PFN_vkVoidFunction imgui_loader(const char* name, void* user_data) {
    auto* d = static_cast<DeviceData*>(user_data);
    if (std::strcmp(name, "vkAllocateCommandBuffers") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&imgui_allocate_command_buffers);
    if (PFN_vkVoidFunction function = d->next_gdpa(d->device, name)) return function;
    return d->instance->next_gipa(d->instance->instance, name);
}

void check_result(VkResult result) {
    if (result != VK_SUCCESS) log_line("imgui vulkan call failed: %d", static_cast<int>(result));
}

VkRenderPass pass_for(DeviceData& d, Renderer& r, VkFormat format) {
    auto& pass = r.passes[format];
    if (pass) return pass;
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (d.fn.CreateRenderPass(d.device, &info, nullptr, &pass) != VK_SUCCESS) pass = VK_NULL_HANDLE;
    return pass;
}

bool create_frames(DeviceData& d, Renderer& r) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = r.family;
    if (d.fn.CreateCommandPool(d.device, &pool_info, nullptr, &r.pool) != VK_SUCCESS) return false;
    for (auto& frame : r.frames) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = r.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (d.fn.AllocateCommandBuffers(d.device, &allocate, &frame.command_buffer) != VK_SUCCESS) return false;
        if (d.set_loader_data(d.device, frame.command_buffer) != VK_SUCCESS) return false;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (d.fn.CreateFence(d.device, &fence, nullptr, &frame.fence) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d.fn.CreateSemaphore(d.device, &semaphore, nullptr, &frame.done) != VK_SUCCESS) return false;
    }
    return true;
}

ImFont* load_font_file(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // TrueType, OpenType or a collection: reject anything else before stb_truetype sees it.
    if (bytes.size() < 12) return nullptr;
    const unsigned char* head = reinterpret_cast<const unsigned char*>(bytes.data());
    const bool known = (head[0] == 0 && head[1] == 1 && head[2] == 0 && head[3] == 0) ||
        std::memcmp(head, "OTTO", 4) == 0 || std::memcmp(head, "true", 4) == 0 || std::memcmp(head, "ttcf", 4) == 0;
    if (!known) return nullptr;
    void* data = IM_ALLOC(bytes.size());
    std::memcpy(data, bytes.data(), bytes.size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes.size()));
}

std::wstring widen(const std::string& text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
    if (!wide.empty()) wide.pop_back();
    return wide;
}

bool init_backend(DeviceData& d, Renderer& r, VkFormat format) {
    VkRenderPass pass = pass_for(d, r, format);
    if (!pass || !create_frames(d, r)) return false;
    r.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(r.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "shroudforge-overlay";
    r.default_font = load_font_file(kDefaultFont);
    if (!r.default_font) r.default_font = io.Fonts->AddFontDefault();

    imgui_device = &d;
    if (!ImGui_ImplVulkan_LoadFunctions(d.instance->api_version, imgui_loader, &d)) {
        log_line("imgui: Vulkan functions unavailable");
        return false;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = d.instance->api_version;
    info.Instance = d.instance->instance;
    info.PhysicalDevice = d.physical_device;
    info.Device = d.device;
    info.QueueFamily = r.family;
    info.Queue = r.queue;
    info.DescriptorPoolSize = 128;
    info.MinImageCount = 2;
    info.ImageCount = kFrames;
    info.PipelineInfoMain.RenderPass = pass;
    info.CheckVkResultFn = check_result;
    if (!ImGui_ImplVulkan_Init(&info)) {
        log_line("imgui: backend initialization failed");
        return false;
    }
    r.backend_ready = true;
    r.pipeline_format = format;
    log_line("renderer ready on queue family %u, format %d", r.family, static_cast<int>(format));
    return true;
}

ImFont* font_for(Renderer& r, const std::string& path) {
    if (path.empty()) return r.default_font;
    auto found = r.fonts.find(path);
    if (found != r.fonts.end()) return found->second ? found->second : r.default_font;
    ImFont* font = load_font_file(widen(path));
    if (!font) log_line("font not loaded: %s", path.c_str());
    r.fonts[path] = font;
    return font ? font : r.default_font;
}

void collect_fonts(Renderer& r, const std::vector<Item>& items) {
    for (const Item& item : items) {
        if (item.kind == ItemKind::Text || item.kind == ItemKind::Chip) font_for(r, item.style.font);
        if (item.kind == ItemKind::Row || item.kind == ItemKind::Flow) collect_fonts(r, item.children);
    }
}

ImTextureRef texture_for(Renderer& r, Image& image) {
    if (!image.texture) {
        auto* texture = IM_NEW(ImTextureData)();
        texture->Create(ImTextureFormat_RGBA32, image.width, image.height);
        std::memcpy(texture->Pixels, image.rgba.data(), image.rgba.size());
        ImGui::RegisterUserTexture(texture);
        r.textures.push_back(texture);
        image.texture = texture;
    }
    return image.texture->GetTexRef();
}

bool prepare_targets(DeviceData& d, Renderer& r, SwapchainData& swapchain, VkRenderPass pass) {
    if (!swapchain.framebuffers.empty()) return true;
    for (VkImage image : swapchain.images) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{};
        if (d.fn.CreateImageView(d.device, &view_info, nullptr, &view) != VK_SUCCESS) return false;
        swapchain.views.push_back(view);
        VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer_info.renderPass = pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &view;
        framebuffer_info.width = swapchain.extent.width;
        framebuffer_info.height = swapchain.extent.height;
        framebuffer_info.layers = 1;
        VkFramebuffer framebuffer{};
        if (d.fn.CreateFramebuffer(d.device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) return false;
        swapchain.framebuffers.push_back(framebuffer);
    }
    return true;
}

void wait_frames(DeviceData& d, Renderer& r) {
    for (auto& frame : r.frames) {
        if (!frame.submitted) continue;
        d.fn.WaitForFences(d.device, 1, &frame.fence, VK_TRUE, 1'000'000'000ull);
        d.fn.ResetFences(d.device, 1, &frame.fence);
        frame.submitted = false;
    }
}

}  // namespace

VkSemaphore render_frame(DeviceData& d, VkQueue queue, const VkPresentInfoKHR& present) {
    if (!is_game_process() || !d.instance || !d.set_loader_data) return VK_NULL_HANDLE;
    update_player_options();
    SwapchainData* target = nullptr;
    uint32_t image_index = 0;
    uint32_t family = UINT32_MAX;
    {
        std::lock_guard lock(state_mutex);
        for (uint32_t i = 0; i < present.swapchainCount && !target; ++i) {
            auto found = d.swapchains.find(present.pSwapchains[i]);
            if (found == d.swapchains.end() || !found->second.drawable) continue;
            if (present.pImageIndices[i] >= found->second.images.size()) continue;
            target = &found->second;
            image_index = present.pImageIndices[i];
        }
        auto queue_family = d.queue_families.find(queue);
        if (queue_family != d.queue_families.end()) family = queue_family->second;
    }
    if (!target || family == UINT32_MAX) return VK_NULL_HANDLE;
    set_output(true, static_cast<int>(target->extent.width), static_cast<int>(target->extent.height));
    if (!scene_has_visible_zones()) return VK_NULL_HANDLE;

    if (!d.renderer) {
        d.renderer = new Renderer{};
        d.renderer->family = family;
        d.renderer->queue = queue;
        if (!init_backend(d, *d.renderer, target->format)) {
            log_line("overlay drawing disabled");
            d.renderer->broken = true;
        }
    }
    Renderer& r = *d.renderer;
    if (r.broken) return VK_NULL_HANDLE;
    if (queue != r.queue) {
        if (!r.other_queue_logged) log_line("present on another queue, not drawn");
        r.other_queue_logged = true;
        return VK_NULL_HANDLE;
    }
    ImGui::SetCurrentContext(r.context);
    VkRenderPass pass = pass_for(d, r, target->format);
    if (!pass) return VK_NULL_HANDLE;
    if (target->format != r.pipeline_format) {
        ImGui_ImplVulkan_PipelineInfo pipeline{};
        pipeline.RenderPass = pass;
        ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
        r.pipeline_format = target->format;
    }
    if (!prepare_targets(d, r, *target, pass)) {
        log_line("swapchain targets unavailable, drawing disabled");
        r.broken = true;
        return VK_NULL_HANDLE;
    }

    FrameSlot& frame = r.frames[r.next_frame++ % kFrames];
    if (frame.submitted) {
        if (d.fn.WaitForFences(d.device, 1, &frame.fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) {
            log_line("frame fence timeout, drawing disabled");
            r.broken = true;
            return VK_NULL_HANDLE;
        }
        d.fn.ResetFences(d.device, 1, &frame.fence);
        frame.submitted = false;
    }

    // Build this frame's draw lists.
    const SceneSnapshot scene = snapshot_scene();
    for (const auto& zone : scene.zones) collect_fonts(r, zone->items);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(target->extent.width), static_cast<float>(target->extent.height));
    const ULONGLONG now = GetTickCount64();
    io.DeltaTime = r.last_tick ? std::max(0.001f, (now - r.last_tick) / 1000.0f) : 1.0f / 60.0f;
    r.last_tick = now;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawResources resources{
        [&](const std::string& path) { return font_for(r, path); },
        [&](Image& image) { return texture_for(r, image); },
    };
    draw_scene(*ImGui::GetForegroundDrawList(), scene, io.DisplaySize, resources);
    ImGui::Render();

    // Record: one render pass over the presented image.
    VkCommandBuffer cb = frame.command_buffer;
    d.fn.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.fn.BeginCommandBuffer(cb, &begin);
    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = target->framebuffers[image_index];
    pass_begin.renderArea.extent = target->extent;
    d.fn.CmdBeginRenderPass(cb, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    d.fn.CmdEndRenderPass(cb);
    d.fn.EndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = present.waitSemaphoreCount;
    submit.pWaitSemaphores = present.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.done;
    if (d.fn.QueueSubmit(queue, 1, &submit, frame.fence) != VK_SUCCESS) {
        log_line("submit failed, drawing disabled");
        r.broken = true;
        return VK_NULL_HANDLE;
    }
    frame.submitted = true;
    if (!r.first_frame_logged) {
        r.first_frame_logged = true;
        log_line("first overlay frame drawn (%zu zones)", scene.zones.size());
    }
    return frame.done;
}

void release_swapchain(DeviceData& d, SwapchainData& swapchain) {
    if (swapchain.framebuffers.empty() && swapchain.views.empty()) return;
    if (d.renderer) wait_frames(d, *d.renderer);
    for (VkFramebuffer framebuffer : swapchain.framebuffers) d.fn.DestroyFramebuffer(d.device, framebuffer, nullptr);
    for (VkImageView view : swapchain.views) d.fn.DestroyImageView(d.device, view, nullptr);
    swapchain.framebuffers.clear();
    swapchain.views.clear();
}

void destroy_renderer(DeviceData& d) {
    Renderer* r = d.renderer;
    if (!r) return;
    d.fn.DeviceWaitIdle(d.device);
    if (r->context) {
        ImGui::SetCurrentContext(r->context);
        if (r->backend_ready) ImGui_ImplVulkan_Shutdown();
        for (ImTextureData* texture : r->textures) ImGui::UnregisterUserTexture(texture);
        ImGui::DestroyContext(r->context);
        for (ImTextureData* texture : r->textures) IM_DELETE(texture);
    }
    forget_textures();
    for (auto& frame : r->frames) {
        if (frame.fence) d.fn.DestroyFence(d.device, frame.fence, nullptr);
        if (frame.done) d.fn.DestroySemaphore(d.device, frame.done, nullptr);
    }
    if (r->pool) d.fn.DestroyCommandPool(d.device, r->pool, nullptr);
    for (auto& [format, pass] : r->passes)
        if (pass) d.fn.DestroyRenderPass(d.device, pass, nullptr);
    delete r;
    d.renderer = nullptr;
    imgui_device = nullptr;
}

}  // namespace overlay
