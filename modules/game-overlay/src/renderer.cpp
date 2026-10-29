// Draws the scene with Dear ImGui into the image being presented.
//
// At a present with visible zones, the layer records one command buffer that
// runs a render pass over the swapchain image (load, draw, store), submits it
// after the game's wait semaphores and makes the real present wait on it. The
// ImGui backend is initialized on the first such present, with that queue.
// When a zone draws off screen (see Offscreen), a pass over an image as large
// as the screen runs first (clear, draw), and the zones show it as a texture.

#include "draw_scene.h"
#include "game_depth.h"
#include "world_depth_spirv.h"
#include "overlay.h"
#include "scene.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace overlay {

namespace {

constexpr uint32_t kFrames = 4;
// ImGui vertex buffers: one set per frame for the zones, one for the
// off-screen pass.
constexpr uint32_t kDrawsPerFrame = 2;
constexpr wchar_t kDefaultFont[] = L"C:\\Windows\\Fonts\\segoeui.ttf";

struct FrameSlot {
    VkCommandBuffer command_buffer{};
    VkFence fence{};
    VkSemaphore done{};
    bool submitted = false;
};

// The off-screen image, in the swapchain's format so that the ImGui pipeline
// draws into it as into the swapchain image.
struct OffscreenTarget {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFramebuffer framebuffer{};
    VkDescriptorSet texture{};
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool failed = false;  // Not created for this extent and format: views are drawn directly.
};

// The game's scene depth read back (game-depth.on): reduced on the GPU to
// kDepthWidth across (a blit, nearest), copied into a host buffer of the
// frame slot, read when the slot comes back (kFrames presents later) and
// shown in a corner as a picture, near cells bright. D32 only.
constexpr uint32_t kDepthWidth = 640;
constexpr ULONGLONG kDepthLogMilliseconds = 5000;

struct DepthReadback {
    bool failed = false;
    VkExtent2D source{};  // the game's depth it was made for
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};  // reduced
    VkImage image{};
    VkDeviceMemory image_memory{};
    VkBuffer buffers[kFrames]{};
    VkDeviceMemory buffer_memory[kFrames]{};
    const float* mapped[kFrames]{};
    bool pending[kFrames]{};
    depth::SceneDepth read_from[kFrames];
    ImTextureData* texture{};
    depth::SceneDepth shown;
    ULONGLONG last_log = 0;
    bool skip_logged = false;
};

// World cells drawn against the game's depth (game-depth.on): our pipeline
// in the zones' pass, before the zones, sampling the game's depth image (in
// a read-only layout for the pass, back to the game's after). Buffers and
// descriptor set per frame slot.
struct WorldDepthPipeline {
    bool failed = false;
    VkRenderPass pass{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkSampler sampler{};
    VkDescriptorPool pool{};
    VkDescriptorSet sets[kFrames]{};
    VkBuffer vertex[kFrames]{}, index[kFrames]{};
    VkDeviceMemory vertex_memory[kFrames]{}, index_memory[kFrames]{};
    void* vertex_mapped[kFrames]{};
    void* index_mapped[kFrames]{};
    VkDeviceSize vertex_size[kFrames]{}, index_size[kFrames]{};
    std::unordered_map<int, VkImageView> views;  // of the game's depth images, by their number
    bool drawn_logged = false;
};

struct WorldDepthConstants {
    float to_clip[2];
    float depth_scale[2];
    float margin;
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
    std::unordered_map<VkFormat, VkRenderPass> offscreen_passes;
    OffscreenTarget offscreen;
    std::unique_ptr<ImDrawList> offscreen_list;
    bool offscreen_used = false;
    VkFormat pipeline_format = VK_FORMAT_UNDEFINED;
    ImGuiContext* context{};
    bool backend_ready = false;
    std::map<std::string, ImFont*> fonts;
    ImFont* default_font{};
    std::vector<ImTextureData*> textures;
    ULONGLONG last_tick = 0;
    bool other_queue_logged = false;
    bool first_frame_logged = false;
    DepthReadback depth;
    std::vector<float> depth_values;  // the last read back
    VkExtent2D depth_values_extent{};
    WorldDepthPipeline world;
    WorldDepthDraw world_draw;
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

// Clears the off-screen image, then leaves it for the zones' pass to sample.
VkRenderPass offscreen_pass_for(DeviceData& d, Renderer& r, VkFormat format) {
    auto& pass = r.offscreen_passes[format];
    if (pass) return pass;
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    // After the previous frame sampled it; before this frame samples it.
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
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
    info.ImageCount = kFrames * kDrawsPerFrame;
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

void release_offscreen(DeviceData& d, Renderer& r) {
    OffscreenTarget& o = r.offscreen;
    if (o.image || o.texture) wait_frames(d, r);
    if (o.texture) ImGui_ImplVulkan_RemoveTexture(o.texture);
    if (o.framebuffer) d.fn.DestroyFramebuffer(d.device, o.framebuffer, nullptr);
    if (o.view) d.fn.DestroyImageView(d.device, o.view, nullptr);
    if (o.image) d.fn.DestroyImage(d.device, o.image, nullptr);
    if (o.memory) d.fn.FreeMemory(d.device, o.memory, nullptr);
    o = OffscreenTarget{};
}

bool create_offscreen(DeviceData& d, Renderer& r, VkExtent2D extent, VkFormat format) {
    OffscreenTarget& o = r.offscreen;
    o.extent = extent;
    o.format = format;
    VkRenderPass pass = offscreen_pass_for(d, r, format);
    if (!pass) return false;
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.fn.CreateImage(d.device, &image_info, nullptr, &o.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    d.fn.GetImageMemoryRequirements(d.device, o.image, &requirements);
    auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        d.instance->next_gipa(d.instance->instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!memory_properties) return false;
    VkPhysicalDeviceMemoryProperties properties{};
    memory_properties(d.physical_device, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (d.fn.AllocateMemory(d.device, &allocate, nullptr, &o.memory) != VK_SUCCESS) return false;
    if (d.fn.BindImageMemory(d.device, o.image, o.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = o.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (d.fn.CreateImageView(d.device, &view_info, nullptr, &o.view) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &o.view;
    framebuffer_info.width = extent.width;
    framebuffer_info.height = extent.height;
    framebuffer_info.layers = 1;
    if (d.fn.CreateFramebuffer(d.device, &framebuffer_info, nullptr, &o.framebuffer) != VK_SUCCESS) return false;
    o.texture = ImGui_ImplVulkan_AddTexture(o.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return o.texture != VK_NULL_HANDLE;
}

// The off-screen image for this extent and format, created or recreated as
// needed; false when it cannot be (then logged once).
bool ensure_offscreen(DeviceData& d, Renderer& r, VkExtent2D extent, VkFormat format) {
    OffscreenTarget& o = r.offscreen;
    const bool same = o.extent.width == extent.width && o.extent.height == extent.height && o.format == format;
    if (same && (o.texture || o.failed)) return !o.failed;
    release_offscreen(d, r);
    if (create_offscreen(d, r, extent, format)) return true;
    release_offscreen(d, r);
    r.offscreen.extent = extent;
    r.offscreen.format = format;
    r.offscreen.failed = true;
    log_line("off-screen target unavailable (%ux%u, format %d), views drawn directly", extent.width, extent.height,
        static_cast<int>(format));
    return false;
}

uint32_t memory_type(DeviceData& d, uint32_t bits, VkMemoryPropertyFlags flags) {
    auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        d.instance->next_gipa(d.instance->instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!memory_properties) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties properties{};
    memory_properties(d.physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

void release_depth(DeviceData& d, Renderer& r) {
    DepthReadback& b = r.depth;
    wait_frames(d, r);
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (b.buffers[i]) d.fn.DestroyBuffer(d.device, b.buffers[i], nullptr);
        if (b.buffer_memory[i]) d.fn.FreeMemory(d.device, b.buffer_memory[i], nullptr);
        b.buffers[i] = VK_NULL_HANDLE;
        b.buffer_memory[i] = VK_NULL_HANDLE;
        b.mapped[i] = nullptr;
        b.pending[i] = false;
    }
    if (b.image) d.fn.DestroyImage(d.device, b.image, nullptr);
    if (b.image_memory) d.fn.FreeMemory(d.device, b.image_memory, nullptr);
    b.image = VK_NULL_HANDLE;
    b.image_memory = VK_NULL_HANDLE;
}

bool create_depth(DeviceData& d, Renderer& r, const depth::SceneDepth& scene) {
    DepthReadback& b = r.depth;
    if (scene.format != VK_FORMAT_D32_SFLOAT) {
        log_line("game depth: format %d not read (D32 only)", static_cast<int>(scene.format));
        return false;
    }
    auto format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        d.instance->next_gipa(d.instance->instance, "vkGetPhysicalDeviceFormatProperties"));
    VkFormatProperties features{};
    if (format_properties) format_properties(d.physical_device, scene.format, &features);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((features.optimalTilingFeatures & blit) != blit) {
        log_line("game depth: format %d cannot be blitted (features 0x%x)", static_cast<int>(scene.format),
            features.optimalTilingFeatures);
        return false;
    }
    b.source = scene.extent;
    b.format = scene.format;
    const uint32_t width = std::min(kDepthWidth, scene.extent.width);
    b.extent = {width, std::max(1u, static_cast<uint32_t>(
        static_cast<uint64_t>(scene.extent.height) * width / scene.extent.width))};
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = scene.format;
    image_info.extent = {b.extent.width, b.extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.fn.CreateImage(d.device, &image_info, nullptr, &b.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    d.fn.GetImageMemoryRequirements(d.device, b.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX) return false;
    if (d.fn.AllocateMemory(d.device, &allocate, nullptr, &b.image_memory) != VK_SUCCESS) return false;
    if (d.fn.BindImageMemory(d.device, b.image, b.image_memory, 0) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < kFrames; ++i) {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = static_cast<VkDeviceSize>(b.extent.width) * b.extent.height * sizeof(float);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (d.fn.CreateBuffer(d.device, &buffer_info, nullptr, &b.buffers[i]) != VK_SUCCESS) return false;
        d.fn.GetBufferMemoryRequirements(d.device, b.buffers[i], &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX) return false;
        if (d.fn.AllocateMemory(d.device, &allocate, nullptr, &b.buffer_memory[i]) != VK_SUCCESS) return false;
        if (d.fn.BindBufferMemory(d.device, b.buffers[i], b.buffer_memory[i], 0) != VK_SUCCESS) return false;
        void* mapped = nullptr;
        if (d.fn.MapMemory(d.device, b.buffer_memory[i], 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
        b.mapped[i] = static_cast<const float*>(mapped);
    }
    log_line("game depth: read back from img#%d %ux%u to %ux%u", scene.id, scene.extent.width, scene.extent.height,
        b.extent.width, b.extent.height);
    return true;
}

// The reduced depth of this frame's scene, recorded before the zones' pass:
// the game's image goes from the layout it left it in to a transfer source
// and back, untouched otherwise.
void record_depth(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot, const depth::SceneDepth& scene) {
    DepthReadback& b = r.depth;
    if (b.failed || !scene.image) return;
    if (scene.layout == VK_IMAGE_LAYOUT_UNDEFINED || scene.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
        if (!b.skip_logged) log_line("game depth: img#%d in layout %d, not read", scene.id, static_cast<int>(scene.layout));
        b.skip_logged = true;
        return;
    }
    if (b.source.width != scene.extent.width || b.source.height != scene.extent.height || b.format != scene.format) {
        release_depth(d, r);
        if (!create_depth(d, r, scene)) {
            release_depth(d, r);
            b.failed = true;
            log_line("game depth: not read back");
            return;
        }
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier before[2]{};
    before[0].sType = before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[0].oldLayout = scene.layout;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].image = scene.image;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].image = b.image;
    for (auto& barrier : before) {
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = range;
    }
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 2, before);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(scene.extent.width), static_cast<int32_t>(scene.extent.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {static_cast<int32_t>(b.extent.width), static_cast<int32_t>(b.extent.height), 1};
    d.fn.CmdBlitImage(cb, scene.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    VkImageMemoryBarrier after[2] = {before[0], before[1]};
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[0].newLayout = scene.layout;
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, after);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.imageExtent = {b.extent.width, b.extent.height, 1};
    d.fn.CmdCopyImageToBuffer(cb, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffers[slot], 1, &copy);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = b.buffers[slot];
    host.size = VK_WHOLE_SIZE;
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0,
        nullptr);
    b.pending[slot] = true;
    b.read_from[slot] = scene;
}

// A slot's depth, back from the GPU: as a picture (log scale between its
// nearest and farthest cells, near bright, nothing drawn there dark blue),
// and a line in the log now and then.
void take_depth(Renderer& r, uint32_t slot) {
    DepthReadback& b = r.depth;
    if (!b.pending[slot] || !b.mapped[slot]) return;
    b.pending[slot] = false;
    const uint32_t width = b.extent.width, height = b.extent.height;
    const float* values = b.mapped[slot];
    float nearest = 0.0f, farthest = FLT_MAX;
    size_t empty = 0;
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        const float value = values[i];
        if (value <= 0.0f) {
            ++empty;
            continue;
        }
        nearest = std::max(nearest, value);
        farthest = std::min(farthest, value);
    }
    if (!b.texture || b.texture->Width != static_cast<int>(width) || b.texture->Height != static_cast<int>(height)) {
        b.texture = IM_NEW(ImTextureData)();
        b.texture->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
        ImGui::RegisterUserTexture(b.texture);
        r.textures.push_back(b.texture);
    }
    // Reversed depth: larger is nearer; the distance goes as 1 / value.
    const double span = nearest > farthest && farthest > 0.0f ? std::log(nearest / farthest) : 1.0;
    unsigned char* pixels = static_cast<unsigned char*>(b.texture->Pixels);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        const float value = values[i];
        unsigned char* p = pixels + i * 4;
        if (value <= 0.0f) {
            p[0] = 20, p[1] = 30, p[2] = 70, p[3] = 255;
            continue;
        }
        const double t = farthest > 0.0f ? std::clamp(std::log(value / farthest) / span, 0.0, 1.0) : 0.0;
        const auto grey = static_cast<unsigned char>(40.0 + 215.0 * t);
        p[0] = p[1] = p[2] = grey, p[3] = 255;
    }
    if (b.texture->Status == ImTextureStatus_OK) {
        b.texture->UpdateRect = {0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
        b.texture->Updates.clear();
        b.texture->Updates.push_back(b.texture->UpdateRect);
        b.texture->SetStatus(ImTextureStatus_WantUpdates);
    }
    b.shown = b.read_from[slot];
    r.depth_values.assign(values, values + static_cast<size_t>(width) * height);
    r.depth_values_extent = b.extent;
    const ULONGLONG now = GetTickCount64();
    if (now - b.last_log < kDepthLogMilliseconds) return;
    b.last_log = now;
    const float centre = values[(height / 2) * width + width / 2];
    log_line("game depth: img#%d %ux%u layout %d; centre %.6g, nearest %.6g, farthest %.6g, nothing drawn %.1f%%",
        b.shown.id, b.shown.extent.width, b.shown.extent.height, static_cast<int>(b.shown.layout), centre, nearest,
        farthest == FLT_MAX ? 0.0f : farthest, 100.0 * empty / (static_cast<double>(width) * height));
}

void release_world(DeviceData& d, Renderer& r) {
    WorldDepthPipeline& w = r.world;
    wait_frames(d, r);
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (w.vertex[i]) d.fn.DestroyBuffer(d.device, w.vertex[i], nullptr);
        if (w.vertex_memory[i]) d.fn.FreeMemory(d.device, w.vertex_memory[i], nullptr);
        if (w.index[i]) d.fn.DestroyBuffer(d.device, w.index[i], nullptr);
        if (w.index_memory[i]) d.fn.FreeMemory(d.device, w.index_memory[i], nullptr);
    }
    for (auto& [id, view] : w.views) d.fn.DestroyImageView(d.device, view, nullptr);
    if (w.pool) d.fn.DestroyDescriptorPool(d.device, w.pool, nullptr);
    if (w.sampler) d.fn.DestroySampler(d.device, w.sampler, nullptr);
    if (w.pipeline) d.fn.DestroyPipeline(d.device, w.pipeline, nullptr);
    if (w.layout) d.fn.DestroyPipelineLayout(d.device, w.layout, nullptr);
    if (w.set_layout) d.fn.DestroyDescriptorSetLayout(d.device, w.set_layout, nullptr);
    const bool failed = w.failed;
    w = WorldDepthPipeline{};
    w.failed = failed;
}

VkShaderModule shader_module(DeviceData& d, const uint32_t* words, size_t size) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = words;
    VkShaderModule module{};
    return d.fn.CreateShaderModule(d.device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

bool create_world(DeviceData& d, Renderer& r, VkRenderPass pass) {
    WorldDepthPipeline& w = r.world;
    w.pass = pass;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (d.fn.CreateDescriptorSetLayout(d.device, &set_info, nullptr, &w.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange constants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(WorldDepthConstants)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &w.set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &constants;
    if (d.fn.CreatePipelineLayout(d.device, &layout_info, nullptr, &w.layout) != VK_SUCCESS) return false;

    VkShaderModule vert = shader_module(d, kWorldDepthVert, sizeof(kWorldDepthVert));
    VkShaderModule frag = shader_module(d, kWorldDepthFrag, sizeof(kWorldDepthFrag));
    if (!vert || !frag) {
        if (vert) d.fn.DestroyShaderModule(d.device, vert, nullptr);
        if (frag) d.fn.DestroyShaderModule(d.device, frag, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkVertexInputBindingDescription vertex_binding{0, sizeof(WorldDepthVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[4] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldDepthVertex, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, depth)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, dash)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldDepthVertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = w.layout;
    pipeline_info.renderPass = pass;
    const VkResult made = d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &w.pipeline);
    d.fn.DestroyShaderModule(d.device, vert, nullptr);
    d.fn.DestroyShaderModule(d.device, frag, nullptr);
    if (made != VK_SUCCESS) return false;

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d.fn.CreateSampler(d.device, &sampler_info, nullptr, &w.sampler) != VK_SUCCESS) return false;
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = kFrames;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (d.fn.CreateDescriptorPool(d.device, &pool_info, nullptr, &w.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[kFrames];
    for (auto& layout : layouts) layout = w.set_layout;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = w.pool;
    allocate.descriptorSetCount = kFrames;
    allocate.pSetLayouts = layouts;
    if (d.fn.AllocateDescriptorSets(d.device, &allocate, w.sets) != VK_SUCCESS) return false;
    log_line("game depth: world pipeline ready");
    return true;
}

// The world pipeline for the zones' pass, created once (or again for another
// pass); false when it cannot be (then not tried again).
bool ensure_world(DeviceData& d, Renderer& r, VkRenderPass pass) {
    WorldDepthPipeline& w = r.world;
    if (w.failed) return false;
    if (w.pipeline && w.pass == pass) return true;
    release_world(d, r);
    if (create_world(d, r, pass)) return true;
    release_world(d, r);
    r.world.failed = true;
    log_line("game depth: world pipeline not created, world cells drawn as before");
    return false;
}

// A host buffer of the slot holding at least `needed` bytes.
bool ensure_buffer(DeviceData& d, VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped, VkDeviceSize& size,
        VkDeviceSize needed, VkBufferUsageFlags usage) {
    if (buffer && size >= needed) return true;
    if (buffer) d.fn.DestroyBuffer(d.device, buffer, nullptr);
    if (memory) d.fn.FreeMemory(d.device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    mapped = nullptr;
    size = std::max<VkDeviceSize>(needed + needed / 2, 64 * 1024);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    if (d.fn.CreateBuffer(d.device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    d.fn.GetBufferMemoryRequirements(d.device, buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX) return false;
    if (d.fn.AllocateMemory(d.device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
    if (d.fn.BindBufferMemory(d.device, buffer, memory, 0) != VK_SUCCESS) return false;
    return d.fn.MapMemory(d.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
}

// Before the zones' pass: the slot's buffers filled, its set pointing at the
// game's depth, which goes to a read-only layout. False: nothing to draw.
bool prepare_world(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot, const depth::SceneDepth& scene) {
    WorldDepthPipeline& w = r.world;
    const WorldDepthDraw& draw = r.world_draw;
    if (!w.pipeline || draw.indices.empty()) return false;
    const VkDeviceSize vertex_bytes = draw.vertices.size() * sizeof(WorldDepthVertex);
    const VkDeviceSize index_bytes = draw.indices.size() * sizeof(std::uint32_t);
    if (!ensure_buffer(d, w.vertex[slot], w.vertex_memory[slot], w.vertex_mapped[slot], w.vertex_size[slot],
            vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !ensure_buffer(d, w.index[slot], w.index_memory[slot], w.index_mapped[slot], w.index_size[slot], index_bytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        log_line("game depth: world buffers unavailable");
        return false;
    }
    std::memcpy(w.vertex_mapped[slot], draw.vertices.data(), vertex_bytes);
    std::memcpy(w.index_mapped[slot], draw.indices.data(), index_bytes);
    VkImageView& view = w.views[scene.id];
    if (!view) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = scene.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = scene.format;
        info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (d.fn.CreateImageView(d.device, &info, nullptr, &view) != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            return false;
        }
    }
    VkDescriptorImageInfo image{w.sampler, view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = w.sets[slot];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    d.fn.UpdateDescriptorSets(d.device, 1, &write, 0, nullptr);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = scene.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = scene.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
        0, nullptr, 1, &barrier);
    return true;
}

// In the zones' pass, before the zones.
void draw_world_depth(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot, const depth::SceneDepth& scene,
        VkExtent2D screen, const depth::Settings& settings) {
    WorldDepthPipeline& w = r.world;
    d.fn.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w.pipeline);
    d.fn.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w.layout, 0, 1, &w.sets[slot], 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(screen.width), static_cast<float>(screen.height), 0.0f, 1.0f};
    d.fn.CmdSetViewport(cb, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, screen};
    d.fn.CmdSetScissor(cb, 0, 1, &scissor);
    WorldDepthConstants constants{};
    constants.to_clip[0] = 2.0f / screen.width;
    constants.to_clip[1] = 2.0f / screen.height;
    constants.depth_scale[0] = static_cast<float>(scene.extent.width) / screen.width;
    constants.depth_scale[1] = static_cast<float>(scene.extent.height) / screen.height;
    constants.margin = static_cast<float>(settings.margin / settings.near_distance);
    d.fn.CmdPushConstants(cb, w.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants,
        &constants);
    const VkDeviceSize offset = 0;
    d.fn.CmdBindVertexBuffers(cb, 0, 1, &w.vertex[slot], &offset);
    d.fn.CmdBindIndexBuffer(cb, w.index[slot], 0, VK_INDEX_TYPE_UINT32);
    d.fn.CmdDrawIndexed(cb, static_cast<uint32_t>(r.world_draw.indices.size()), 1, 0, 0, 0);
    if (!w.drawn_logged) {
        w.drawn_logged = true;
        log_line("game depth: first world cells drawn against img#%d (%zu triangles)", scene.id,
            r.world_draw.indices.size() / 3);
    }
}

// After the zones' pass: the game's depth back in its layout.
void finish_world(DeviceData& d, VkCommandBuffer cb, const depth::SceneDepth& scene) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.newLayout = scene.layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = scene.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
        0, nullptr, 1, &barrier);
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
    // The game's depth shown in a corner (game-depth.on): drawn without zones too.
    if (!scene_has_visible_zones() && !depth::enabled()) return VK_NULL_HANDLE;

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

    const uint32_t slot = r.next_frame++ % kFrames;
    FrameSlot& frame = r.frames[slot];
    if (frame.submitted) {
        if (d.fn.WaitForFences(d.device, 1, &frame.fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) {
            log_line("frame fence timeout, drawing disabled");
            r.broken = true;
            return VK_NULL_HANDLE;
        }
        d.fn.ResetFences(d.device, 1, &frame.fence);
        frame.submitted = false;
        take_depth(r, slot);
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
    if (!r.offscreen_list) r.offscreen_list = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
    ImDrawList& offscreen = *r.offscreen_list;
    offscreen._ResetForNewFrame();
    offscreen.PushTexture(io.Fonts->TexRef);
    offscreen.PushClipRectFullScreen();
    r.offscreen_used = false;
    // The game's depth (game-depth.on): this frame's scene depth, and the
    // world pipeline when it can be read.
    const depth::SceneDepth scene_depth = depth::enabled() ? depth::scene_depth(target->extent) : depth::SceneDepth{};
    const depth::Settings depth_settings = depth::enabled() ? depth::settings() : depth::Settings{};
    r.world_draw.vertices.clear();
    r.world_draw.indices.clear();
    r.world_draw.near_distance = depth_settings.near_distance;
    const bool world_ready = scene_depth.image && scene_depth.format == VK_FORMAT_D32_SFLOAT &&
        scene_depth.layout != VK_IMAGE_LAYOUT_UNDEFINED && scene_depth.layout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
        ensure_world(d, r, pass);
    DrawResources resources{
        [&](const std::string& path) { return font_for(r, path); },
        [&](Image& image) { return texture_for(r, image); },
        [&]() -> Offscreen {
            if (!ensure_offscreen(d, r, target->extent, target->format)) return {};
            r.offscreen_used = true;
            return {&offscreen, ImTextureRef(reinterpret_cast<ImTextureID>(r.offscreen.texture)), io.DisplaySize};
        },
        [&]() -> WorldDepthDraw* { return world_ready ? &r.world_draw : nullptr; },
        [&](ImVec2 at) -> float {
            const VkExtent2D e = r.depth_values_extent;
            if (r.depth_values.empty() || !e.width) return -1.0f;
            const int x = static_cast<int>(at.x / io.DisplaySize.x * e.width);
            const int y = static_cast<int>(at.y / io.DisplaySize.y * e.height);
            if (x < 0 || y < 0 || x >= static_cast<int>(e.width) || y >= static_cast<int>(e.height)) return -1.0f;
            return r.depth_values[static_cast<size_t>(y) * e.width + x];
        },
    };
    draw_scene(*ImGui::GetForegroundDrawList(), scene, io.DisplaySize, resources);
    if (depth_settings.show_picture && r.depth.texture && r.depth.texture->Status != ImTextureStatus_WantCreate) {
        // Bottom left, a quarter of the screen across.
        ImDrawList& list = *ImGui::GetForegroundDrawList();
        const float width = io.DisplaySize.x / 4.0f;
        const float height = width * r.depth.texture->Height / std::max(1, r.depth.texture->Width);
        const ImVec2 from(16.0f, io.DisplaySize.y - 16.0f - height), to(16.0f + width, io.DisplaySize.y - 16.0f);
        list.AddImage(r.depth.texture->GetTexRef(), from, to);
        list.AddRect(from, to, IM_COL32(255, 255, 255, 160));
        char label[96];
        snprintf(label, sizeof label, "game depth img#%d %ux%u, layout %d", r.depth.shown.id, r.depth.shown.extent.width,
            r.depth.shown.extent.height, static_cast<int>(r.depth.shown.layout));
        list.AddText(ImVec2(from.x + 6.0f, from.y + 4.0f), IM_COL32(255, 255, 160, 255), label);
    }
    ImGui::Render();
    // Textures first: the off-screen pass may use the font atlas before the
    // zones' pass would upload it.
    ImDrawData* zones = ImGui::GetDrawData();
    if (zones->Textures)
        for (ImTextureData* texture : *zones->Textures)
            if (texture->Status != ImTextureStatus_OK) ImGui_ImplVulkan_UpdateTexture(texture);

    // Record: one render pass over the presented image.
    VkCommandBuffer cb = frame.command_buffer;
    d.fn.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.fn.BeginCommandBuffer(cb, &begin);
    if (depth::enabled()) record_depth(d, r, cb, slot, scene_depth);
    const bool world_drawn = world_ready && prepare_world(d, r, cb, slot, scene_depth);
    if (r.offscreen_used) {
        ImDrawData data;
        data.Valid = true;
        data.DisplayPos = ImVec2(0.0f, 0.0f);
        data.DisplaySize = io.DisplaySize;
        data.FramebufferScale = ImVec2(1.0f, 1.0f);
        data.AddDrawList(&offscreen);
        VkClearValue clear{};
        VkRenderPassBeginInfo offscreen_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        offscreen_begin.renderPass = offscreen_pass_for(d, r, r.offscreen.format);
        offscreen_begin.framebuffer = r.offscreen.framebuffer;
        offscreen_begin.renderArea.extent = r.offscreen.extent;
        offscreen_begin.clearValueCount = 1;
        offscreen_begin.pClearValues = &clear;
        d.fn.CmdBeginRenderPass(cb, &offscreen_begin, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(&data, cb);
        d.fn.CmdEndRenderPass(cb);
    }
    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = target->framebuffers[image_index];
    pass_begin.renderArea.extent = target->extent;
    d.fn.CmdBeginRenderPass(cb, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    if (world_drawn) draw_world_depth(d, r, cb, slot, scene_depth, target->extent, depth_settings);
    ImGui_ImplVulkan_RenderDrawData(zones, cb);
    d.fn.CmdEndRenderPass(cb);
    if (world_drawn) finish_world(d, cb, scene_depth);
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
        if (r->backend_ready) release_offscreen(d, *r);
        release_depth(d, *r);
        release_world(d, *r);
        r->offscreen_list.reset();
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
    for (auto& [format, pass] : r->offscreen_passes)
        if (pass) d.fn.DestroyRenderPass(d.device, pass, nullptr);
    delete r;
    d.renderer = nullptr;
    imgui_device = nullptr;
}

}  // namespace overlay
