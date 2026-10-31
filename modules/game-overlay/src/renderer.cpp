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
#include <map>
#include <memory>

namespace overlay {

namespace {

constexpr uint32_t kFrames = 4;
// ImGui vertex buffers: one set per frame for the zones, one for the
// off-screen pass.
constexpr uint32_t kDrawsPerFrame = 2;
constexpr wchar_t kDefaultFont[] = L"C:\\Windows\\Fonts\\segoeui.ttf";

// The image shown made a quarter of its size, with its smaller levels down
// to depth::kLightLevels (shrink_screen).
struct ScreenCopy {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkExtent2D extent{};
};

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

// The game's scene depth read back (a diagnostic): reduced on the GPU to
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

// World cells drawn against the game's depth (game_depth.cpp): our pipeline
// in the zones' pass, before the zones, sampling the game's depth image (in
// a read-only layout for the pass, back to the game's after). Buffers and
// descriptor set per frame slot.
// A texture of the solid style on the GPU: made from its BC7 levels, copied
// in a frame's commands (its staging buffer freed frames later), kept while
// an item still holds its data (the mod's texture cache), and a few frames
// more.
struct WorldTextureGpu {
    std::weak_ptr<const std::string> data;
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    uint32_t width = 0, height = 0, levels = 0;
    VkFormat format{};
    bool copied = false;
    uint32_t copied_frame = 0, used_frame = 0;
};

struct WorldDepthPipeline {
    bool failed = false;
    // Our pass over the presented image before the zones', with a depth image
    // of our own (reversed, cleared to 0): the solid pipeline tests and writes
    // it, so that the nearest of the build's faces shows (30/10: sorted from
    // the farthest, faces fought at some junctions); the other one does not.
    VkFormat format{};
    VkRenderPass pass{};
    VkImage depth{};
    VkDeviceMemory depth_memory{};
    VkImageView depth_view{};
    VkExtent2D depth_extent{};
    std::unordered_map<VkImageView, VkFramebuffer> framebuffers;  // by the presented image's view
    // The wire view: a pass writing our faces' depth only (sampled after),
    // and a pipeline in the zones' pass.
    VkRenderPass depth_pass{};
    VkFramebuffer depth_framebuffer{};
    VkPipeline depth_pipeline{};
    VkPipeline wire_pipeline{};
    VkRenderPass zones_pass{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkPipeline solid_pipeline{};
    VkSampler sampler{};
    VkDescriptorPool pool{};
    VkDescriptorSet sets[kFrames]{};
    VkBuffer vertex[kFrames]{}, index[kFrames]{};
    VkDeviceMemory vertex_memory[kFrames]{}, index_memory[kFrames]{};
    void* vertex_mapped[kFrames]{};
    void* index_mapped[kFrames]{};
    VkDeviceSize vertex_size[kFrames]{}, index_size[kFrames]{};
    // The world's blocks of the wire view in Remove mode, a bit per cell:
    // packed once per grid, copied to each slot's buffer when it changes.
    VkBuffer blocks[kFrames]{};
    VkDeviceMemory blocks_memory[kFrames]{};
    void* blocks_mapped[kFrames]{};
    VkDeviceSize blocks_size[kFrames]{};
    std::uint64_t blocks_in[kFrames]{};  // the packing each slot holds
    std::shared_ptr<const WorldSolids> packed_of;
    std::vector<const VoxelMesh*> packed_meshes;
    std::vector<std::shared_ptr<const VoxelMesh>> packed_keep;
    std::uint64_t packing = 0;
    std::vector<std::uint32_t> packed;
    // The solid style's textures by their data, a white one when there is none.
    std::map<const std::string*, WorldTextureGpu> textures;
    WorldTextureGpu white;
    VkSampler texture_sampler{};
    // The HUD's blurred scene: the image shown with the world items, made
    // smaller after they are drawn (fill_hud_blur).
    ScreenCopy hud_small;
    bool hud_blur_said = false;
    // Its Gaussian blur (fill_hud_blur): across into `hud_temp`, then down
    // into the layer's image (game_depth.cpp), both the game's blur's size.
    VkRenderPass hud_blur_pass{};
    // The pass down mixed with our last blur (the game's keeps some of its
    // last too): our image loaded, a pipeline blending by a constant.
    VkRenderPass hud_blend_pass{};
    VkDescriptorSetLayout hud_blur_set_layout{};
    VkPipelineLayout hud_blur_layout{};
    VkPipeline hud_blur_pipeline{};
    VkPipeline hud_blend_pipeline{};
    bool hud_history = false;  // the layer's image holds our last blur
    VkDescriptorPool hud_blur_pool{};
    VkDescriptorSet hud_blur_sets[kFrames * 2]{};
    VkImage hud_temp{};
    VkDeviceMemory hud_temp_memory{};
    VkImageView hud_temp_view{};
    VkFramebuffer hud_temp_framebuffer{};
    VkExtent2D hud_temp_extent{};
    VkImage hud_target{};  // the layer's image the framebuffer below draws into
    VkImageView hud_target_view{};
    VkFramebuffer hud_target_framebuffer{};
    VkSampler light_sampler{};
    // Under the game's interface: the pass laying the interface layer
    // (game_depth.cpp) back over the world items, before the zones.
    VkDescriptorSetLayout interface_set_layout{};
    VkPipelineLayout interface_layout{};
    VkPipeline interface_pipeline{};
    VkDescriptorPool interface_pool{};
    VkDescriptorSet interface_sets[kFrames]{};
    bool interface_said = false;
    // This frame's layer, laid back from ImGui's draw callback (between the
    // world-only zones and the others), else after the zones.
    DeviceData* interface_device{};
    uint32_t interface_slot = 0;
    VkExtent2D interface_extent{};
    bool interface_ready = false;  // its set written, the layer readable
    bool interface_laid = false;
    // Our HUD blur, made there too (the zones' pass ended and begun again):
    // from the image shown with every world item on it.
    bool hud_fill = false;    // asked, and possible this frame
    bool hud_filled = false;  // made
    VkImage hud_shown{};
    depth::HudBlur hud_blur_into;
    VkRenderPassBeginInfo zones_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    std::unordered_map<int, VkImageView> views;  // of the game's depth images, by their number
    bool drawn_logged = false;
};

struct WorldDepthConstants {
    float to_clip[2];
    float depth_scale[2];
    float pad;            // (a margin once, 31/10: always 0)
    int32_t mode;         // 0 against the game's depth, 1 our faces' depth, 2 the wire view, 4 through the blocks
    float own_tolerance;  // wire: how far before our nearest face something hides an edge entirely
    float focal;          // screen pixels per unit of tan, vertically (the wire view's slopes)
    float eye[4];         // mode 4: the eye in cells of the blocks' grid; [3] the relief's tolerance, in cells
    int32_t size[4];      // mode 4: the grid's size in cells
    float dash_alpha;     // a hidden edge's dashes, this opaque
    float back_alpha;     // the wire view: those behind the build's own faces
    float relief;         // how much the normal maps tilt the faces (0 to 1)
    float normal_faces;   // which faces have a normal map: 1 sides, 2 top, 4 bottom
    // GLSL lays a vec4 on 16 bytes: tiling at 80 (31/10: once at 72, the top
    // and bottom read a tiling of 0, one texel spread over the face).
    float tiling[4];      // the solid style's textures: repeats a meter of the sides, top and bottom
};
static_assert(offsetof(WorldDepthConstants, tiling) == 80, "as the shaders' push constants");

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
    uint32_t depth_picture_until = 0;  // asked by a world item up to this frame
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
        // Read by the CPU: cached memory first (75 to 17 images a second
        // reading uncached memory, 30/10).
        allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX)
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

// A slot's depth, back from the GPU: as a picture (the distance, near /
// depth, on a log scale from 0.5 m, bright, to 2 km, dark; nothing drawn
// there dark blue), and a line in the log now and then.
void take_depth(Renderer& r, uint32_t slot, double near_distance) {
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
    // Reversed depth: larger is nearer; the distance is near / value.
    const double span = std::log(2000.0 / 0.5);
    unsigned char* pixels = static_cast<unsigned char*>(b.texture->Pixels);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        const float value = values[i];
        unsigned char* p = pixels + i * 4;
        if (value <= 0.0f) {
            p[0] = 20, p[1] = 30, p[2] = 70, p[3] = 255;
            continue;
        }
        const double t = std::clamp(std::log(near_distance / value / 0.5) / span, 0.0, 1.0);
        const auto grey = static_cast<unsigned char>(255.0 - 215.0 * t);
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

void release_texture(DeviceData& d, WorldTextureGpu& t) {
    if (t.view) d.fn.DestroyImageView(d.device, t.view, nullptr);
    if (t.image) d.fn.DestroyImage(d.device, t.image, nullptr);
    if (t.memory) d.fn.FreeMemory(d.device, t.memory, nullptr);
    if (t.staging) d.fn.DestroyBuffer(d.device, t.staging, nullptr);
    if (t.staging_memory) d.fn.FreeMemory(d.device, t.staging_memory, nullptr);
    t = WorldTextureGpu{};
}

void release_world(DeviceData& d, Renderer& r) {
    WorldDepthPipeline& w = r.world;
    wait_frames(d, r);
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (w.vertex[i]) d.fn.DestroyBuffer(d.device, w.vertex[i], nullptr);
        if (w.vertex_memory[i]) d.fn.FreeMemory(d.device, w.vertex_memory[i], nullptr);
        if (w.index[i]) d.fn.DestroyBuffer(d.device, w.index[i], nullptr);
        if (w.index_memory[i]) d.fn.FreeMemory(d.device, w.index_memory[i], nullptr);
        if (w.blocks[i]) d.fn.DestroyBuffer(d.device, w.blocks[i], nullptr);
        if (w.blocks_memory[i]) d.fn.FreeMemory(d.device, w.blocks_memory[i], nullptr);
    }
    for (auto& [id, view] : w.views) d.fn.DestroyImageView(d.device, view, nullptr);
    for (auto& [view, framebuffer] : w.framebuffers) d.fn.DestroyFramebuffer(d.device, framebuffer, nullptr);
    for (auto& [data, texture] : w.textures) release_texture(d, texture);
    release_texture(d, w.white);
    if (w.texture_sampler) d.fn.DestroySampler(d.device, w.texture_sampler, nullptr);
    if (w.hud_temp_framebuffer) d.fn.DestroyFramebuffer(d.device, w.hud_temp_framebuffer, nullptr);
    if (w.hud_temp_view) d.fn.DestroyImageView(d.device, w.hud_temp_view, nullptr);
    if (w.hud_temp) d.fn.DestroyImage(d.device, w.hud_temp, nullptr);
    if (w.hud_temp_memory) d.fn.FreeMemory(d.device, w.hud_temp_memory, nullptr);
    if (w.hud_target_framebuffer) d.fn.DestroyFramebuffer(d.device, w.hud_target_framebuffer, nullptr);
    if (w.hud_target_view) d.fn.DestroyImageView(d.device, w.hud_target_view, nullptr);
    if (w.hud_blur_pipeline) d.fn.DestroyPipeline(d.device, w.hud_blur_pipeline, nullptr);
    if (w.hud_blend_pipeline) d.fn.DestroyPipeline(d.device, w.hud_blend_pipeline, nullptr);
    if (w.hud_blend_pass) d.fn.DestroyRenderPass(d.device, w.hud_blend_pass, nullptr);
    if (w.hud_blur_layout) d.fn.DestroyPipelineLayout(d.device, w.hud_blur_layout, nullptr);
    if (w.hud_blur_set_layout) d.fn.DestroyDescriptorSetLayout(d.device, w.hud_blur_set_layout, nullptr);
    if (w.hud_blur_pool) d.fn.DestroyDescriptorPool(d.device, w.hud_blur_pool, nullptr);
    if (w.hud_blur_pass) d.fn.DestroyRenderPass(d.device, w.hud_blur_pass, nullptr);
    for (ScreenCopy* copy : {&w.hud_small}) {
        if (copy->view) d.fn.DestroyImageView(d.device, copy->view, nullptr);
        if (copy->image) d.fn.DestroyImage(d.device, copy->image, nullptr);
        if (copy->memory) d.fn.FreeMemory(d.device, copy->memory, nullptr);
    }
    if (w.light_sampler) d.fn.DestroySampler(d.device, w.light_sampler, nullptr);
    if (w.interface_pipeline) d.fn.DestroyPipeline(d.device, w.interface_pipeline, nullptr);
    if (w.interface_layout) d.fn.DestroyPipelineLayout(d.device, w.interface_layout, nullptr);
    if (w.interface_set_layout) d.fn.DestroyDescriptorSetLayout(d.device, w.interface_set_layout, nullptr);
    if (w.interface_pool) d.fn.DestroyDescriptorPool(d.device, w.interface_pool, nullptr);
    if (w.depth_framebuffer) d.fn.DestroyFramebuffer(d.device, w.depth_framebuffer, nullptr);
    if (w.depth_pass) d.fn.DestroyRenderPass(d.device, w.depth_pass, nullptr);
    if (w.depth_pipeline) d.fn.DestroyPipeline(d.device, w.depth_pipeline, nullptr);
    if (w.wire_pipeline) d.fn.DestroyPipeline(d.device, w.wire_pipeline, nullptr);
    if (w.depth_view) d.fn.DestroyImageView(d.device, w.depth_view, nullptr);
    if (w.depth) d.fn.DestroyImage(d.device, w.depth, nullptr);
    if (w.depth_memory) d.fn.FreeMemory(d.device, w.depth_memory, nullptr);
    if (w.pass) d.fn.DestroyRenderPass(d.device, w.pass, nullptr);
    if (w.pool) d.fn.DestroyDescriptorPool(d.device, w.pool, nullptr);
    if (w.sampler) d.fn.DestroySampler(d.device, w.sampler, nullptr);
    if (w.solid_pipeline) d.fn.DestroyPipeline(d.device, w.solid_pipeline, nullptr);
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

// The world's pass: the presented image loaded and kept, our depth cleared.
VkRenderPass world_pass(DeviceData& d, VkFormat format) {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    VkRenderPass pass{};
    return d.fn.CreateRenderPass(d.device, &info, nullptr, &pass) == VK_SUCCESS ? pass : VK_NULL_HANDLE;
}

// The wire view's depth pass: our depth cleared, written, left to be sampled.
VkRenderPass depth_only_pass(DeviceData& d) {
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_D32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depth{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depth;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    VkRenderPass pass{};
    return d.fn.CreateRenderPass(d.device, &info, nullptr, &pass) == VK_SUCCESS ? pass : VK_NULL_HANDLE;
}

bool create_interface_layer(DeviceData& d, Renderer& r);
bool create_hud_blur(DeviceData& d, Renderer& r);

bool create_world(DeviceData& d, Renderer& r, VkFormat format) {
    WorldDepthPipeline& w = r.world;
    w.format = format;
    w.pass = world_pass(d, format);
    if (!w.pass) return false;
    const VkRenderPass pass = w.pass;
    // The game's depth, ours (the wire view), the world's blocks (its Remove
    // mode) and the solid style's textures (sides, top, bottom, then their
    // ambient occlusion, then their normal maps).
    VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 4;
    set_info.pBindings = bindings;
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
    VkVertexInputAttributeDescription attributes[8] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldDepthVertex, x)},
        {1, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, depth)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, dash)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldDepthVertex, color)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, extra)},
        {5, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldDepthVertex, cell)},
        {6, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, fade)},
        {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(WorldDepthVertex, tex)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 8;
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
    // Translucent: our depth neither tested nor written; solid: the nearest kept.
    VkPipelineDepthStencilStateCreateInfo no_depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo own_depth = no_depth;
    own_depth.depthTestEnable = VK_TRUE;
    own_depth.depthWriteEnable = VK_TRUE;
    own_depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    pipeline_info.pDepthStencilState = &no_depth;
    VkResult made = d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &w.pipeline);
    if (made == VK_SUCCESS) {
        pipeline_info.pDepthStencilState = &own_depth;
        made = d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &w.solid_pipeline);
    }
    // The wire view: our faces' depth alone (no color), then the zones' pass.
    w.depth_pass = made == VK_SUCCESS ? depth_only_pass(d) : VK_NULL_HANDLE;
    if (w.depth_pass) {
        VkPipelineColorBlendStateCreateInfo no_color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo depth_info = pipeline_info;
        depth_info.pColorBlendState = &no_color;
        depth_info.pDepthStencilState = &own_depth;
        depth_info.renderPass = w.depth_pass;
        made = d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &depth_info, nullptr, &w.depth_pipeline);
        if (made == VK_SUCCESS) {
            VkGraphicsPipelineCreateInfo wire_info = pipeline_info;
            wire_info.pDepthStencilState = &no_depth;
            wire_info.renderPass = w.zones_pass;
            made = d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &wire_info, nullptr, &w.wire_pipeline);
        }
    } else {
        made = VK_ERROR_INITIALIZATION_FAILED;
    }
    d.fn.DestroyShaderModule(d.device, vert, nullptr);
    d.fn.DestroyShaderModule(d.device, frag, nullptr);
    if (made != VK_SUCCESS) return false;

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d.fn.CreateSampler(d.device, &sampler_info, nullptr, &w.sampler) != VK_SUCCESS) return false;
    // The textures: repeated, smooth, through their levels.
    VkSamplerCreateInfo texture_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    texture_info.magFilter = texture_info.minFilter = VK_FILTER_LINEAR;
    texture_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    texture_info.addressModeU = texture_info.addressModeV = texture_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texture_info.maxLod = 16.0f;
    if (d.fn.CreateSampler(d.device, &texture_info, nullptr, &w.texture_sampler) != VK_SUCCESS) return false;
    VkSamplerCreateInfo light_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    light_info.magFilter = light_info.minFilter = VK_FILTER_LINEAR;
    light_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    light_info.maxLod = 8.0f;
    light_info.addressModeU = light_info.addressModeV = light_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d.fn.CreateSampler(d.device, &light_info, nullptr, &w.light_sampler) != VK_SUCCESS) return false;
    VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kFrames},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrames}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = kFrames;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (d.fn.CreateDescriptorPool(d.device, &pool_info, nullptr, &w.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[kFrames];
    for (auto& layout : layouts) layout = w.set_layout;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = w.pool;
    allocate.descriptorSetCount = kFrames;
    allocate.pSetLayouts = layouts;
    if (d.fn.AllocateDescriptorSets(d.device, &allocate, w.sets) != VK_SUCCESS) return false;
    log_line("game depth: world pipeline ready");
    // Under the interface: without it, the world items are drawn over it, as before.
    if (!create_interface_layer(d, r)) log_line("interface layer: no pipeline, the world items drawn over the interface");
    if (!create_hud_blur(d, r)) log_line("HUD blur: no pipeline, the game's blur left alone");
    return true;
}

// The pass laying the game's interface layer back over the world items (in
// the zones' pass, before the zones): its colors premultiplied, its alpha
// its coverage.
bool create_interface_layer(DeviceData& d, Renderer& r) {
    WorldDepthPipeline& w = r.world;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (d.fn.CreateDescriptorSetLayout(d.device, &set_info, nullptr, &w.interface_set_layout) != VK_SUCCESS)
        return false;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &w.interface_set_layout;
    if (d.fn.CreatePipelineLayout(d.device, &layout_info, nullptr, &w.interface_layout) != VK_SUCCESS) return false;
    VkShaderModule vert = shader_module(d, kInterfaceLayerVert, sizeof(kInterfaceLayerVert));
    VkShaderModule frag = shader_module(d, kInterfaceLayerFrag, sizeof(kInterfaceLayerFrag));
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
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // The layer over what is drawn: color + (1 - alpha) x under.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
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
    VkPipelineDepthStencilStateCreateInfo no_depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
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
    pipeline_info.pDepthStencilState = &no_depth;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = w.interface_layout;
    pipeline_info.renderPass = w.zones_pass;
    const VkResult made =
        d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &w.interface_pipeline);
    d.fn.DestroyShaderModule(d.device, vert, nullptr);
    d.fn.DestroyShaderModule(d.device, frag, nullptr);
    if (made != VK_SUCCESS) {
        w.interface_pipeline = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = kFrames;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorSetLayout layouts[kFrames];
    for (auto& layout : layouts) layout = w.interface_set_layout;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorSetCount = kFrames;
    allocate.pSetLayouts = layouts;
    const bool sets = d.fn.CreateDescriptorPool(d.device, &pool_info, nullptr, &w.interface_pool) == VK_SUCCESS &&
        (allocate.descriptorPool = w.interface_pool,
            d.fn.AllocateDescriptorSets(d.device, &allocate, w.interface_sets) == VK_SUCCESS);
    if (!sets) {
        // Without its sets the layer cannot be laid: never asked for.
        d.fn.DestroyPipeline(d.device, w.interface_pipeline, nullptr);
        w.interface_pipeline = VK_NULL_HANDLE;
    }
    return sets;
}

// The HUD blur, as the game's (31/10, capture frame4945 and two videos): a
// Gaussian of sigma 1.25 texels at its size (mean squared error 0.00005), and
// each frame 80 % new, 20 % of the last (the capture's fit: 81 %; the videos:
// the game's blur moves 0.7 times as much as ours did without it, which 80 %
// gives).
constexpr float kHudBlurSigma = 1.25f;
constexpr float kHudBlurNew = 0.8f;

// The HUD blur's pass and pipeline (R8G8B8A8_UNORM, the game's blur's format):
// a triangle over the target, one direction of a Gaussian blur.
bool create_hud_blur(DeviceData& d, Renderer& r) {
    WorldDepthPipeline& w = r.world;
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
    // After what wrote its source and last read its target (the game's copy
    // in an earlier submission); before the next pass or the game's copy.
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    if (d.fn.CreateRenderPass(d.device, &pass_info, nullptr, &w.hud_blur_pass) != VK_SUCCESS) return false;
    // The same, our last blur kept (left by the game's copy in SHADER_READ_ONLY).
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (d.fn.CreateRenderPass(d.device, &pass_info, nullptr, &w.hud_blend_pass) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (d.fn.CreateDescriptorSetLayout(d.device, &set_info, nullptr, &w.hud_blur_set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 6 * sizeof(float)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &w.hud_blur_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (d.fn.CreatePipelineLayout(d.device, &layout_info, nullptr, &w.hud_blur_layout) != VK_SUCCESS) return false;
    VkShaderModule vert = shader_module(d, kInterfaceLayerVert, sizeof(kInterfaceLayerVert));
    VkShaderModule frag = shader_module(d, kHudBlurFrag, sizeof(kHudBlurFrag));
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
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;
    VkPipelineDepthStencilStateCreateInfo no_depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
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
    pipeline_info.pDepthStencilState = &no_depth;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = w.hud_blur_layout;
    pipeline_info.renderPass = w.hud_blur_pass;
    VkResult made =
        d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &w.hud_blur_pipeline);
    // Blending: kHudBlurNew of the new blur, the rest of the last.
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_state.blendConstants[3] = kHudBlurNew;
    pipeline_info.renderPass = w.hud_blend_pass;
    if (made == VK_SUCCESS)
        made = d.fn.CreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &w.hud_blend_pipeline);
    d.fn.DestroyShaderModule(d.device, vert, nullptr);
    d.fn.DestroyShaderModule(d.device, frag, nullptr);
    if (made != VK_SUCCESS) {
        if (w.hud_blur_pipeline) d.fn.DestroyPipeline(d.device, w.hud_blur_pipeline, nullptr);
        w.hud_blur_pipeline = w.hud_blend_pipeline = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames * 2};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = kFrames * 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorSetLayout layouts[kFrames * 2];
    for (auto& layout : layouts) layout = w.hud_blur_set_layout;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorSetCount = kFrames * 2;
    allocate.pSetLayouts = layouts;
    const bool sets = d.fn.CreateDescriptorPool(d.device, &pool_info, nullptr, &w.hud_blur_pool) == VK_SUCCESS &&
        (allocate.descriptorPool = w.hud_blur_pool,
            d.fn.AllocateDescriptorSets(d.device, &allocate, w.hud_blur_sets) == VK_SUCCESS);
    if (!sets) {
        d.fn.DestroyPipeline(d.device, w.hud_blur_pipeline, nullptr);
        d.fn.DestroyPipeline(d.device, w.hud_blend_pipeline, nullptr);
        w.hud_blur_pipeline = w.hud_blend_pipeline = VK_NULL_HANDLE;
    }
    return sets;
}

// The world pipelines for the presented image's format, created once (or
// again for another format); false when they cannot be (then not tried again).
bool ensure_world(DeviceData& d, Renderer& r, VkFormat format, VkRenderPass zones_pass) {
    WorldDepthPipeline& w = r.world;
    if (w.failed) return false;
    if (w.pipeline && w.format == format && w.zones_pass == zones_pass) return true;
    release_world(d, r);
    r.world.zones_pass = zones_pass;
    if (create_world(d, r, format)) return true;
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

// A texture made from `bytes` (levels from width x height down, in
// `format`: BC7, BC1, BC4, BC5 or 1 x 1 RGBA), its staging buffer filled; copied later.
bool make_texture(DeviceData& d, WorldTextureGpu& t, VkFormat format, uint32_t width, uint32_t height, uint32_t levels,
        const void* bytes, VkDeviceSize size) {
    t.format = format;
    t.width = width;
    t.height = height;
    t.levels = levels;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.fn.CreateImage(d.device, &info, nullptr, &t.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    d.fn.GetImageMemoryRequirements(d.device, t.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX ||
        d.fn.AllocateMemory(d.device, &allocate, nullptr, &t.memory) != VK_SUCCESS ||
        d.fn.BindImageMemory(d.device, t.image, t.memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = t.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (d.fn.CreateImageView(d.device, &view_info, nullptr, &t.view) != VK_SUCCESS) return false;
    void* mapped = nullptr;
    VkDeviceSize staged = 0;
    if (!ensure_buffer(d, t.staging, t.staging_memory, mapped, staged, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    std::memcpy(mapped, bytes, static_cast<size_t>(size));
    return true;
}

// Its levels copied from the staging buffer, then left for the shaders.
void copy_texture(DeviceData& d, Renderer& r, VkCommandBuffer cb, WorldTextureGpu& t) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = t.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, 1};
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize offset = 0;
    const bool rgba = t.format == VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize block =
        t.format == VK_FORMAT_BC4_UNORM_BLOCK || t.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;
    for (uint32_t level = 0; level < t.levels; ++level) {
        const uint32_t w = std::max(1u, t.width >> level), h = std::max(1u, t.height >> level);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        copy.imageExtent = {w, h, 1};
        copies.push_back(copy);
        offset += rgba ? static_cast<VkDeviceSize>(w) * h * 4 : static_cast<VkDeviceSize>((w + 3) / 4) * ((h + 3) / 4) * block;
    }
    d.fn.CmdCopyBufferToImage(cb, t.staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(copies.size()), copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);
    t.copied = true;
    t.copied_frame = r.next_frame;
}

// The view of a solid-style texture for this frame: made and copied the
// first time its data is seen (BC7, BC1, BC4 or BC5, as the game stores it: an UNORM view,
// its values as stored, as the overlay's colors are), a white one otherwise.
VkImageView texture_view(DeviceData& d, Renderer& r, VkCommandBuffer cb, const WorldTexture* texture) {
    WorldDepthPipeline& w = r.world;
    if (!w.white.image) {
        const std::uint32_t white = 0xFFFFFFFFu;
        if (!make_texture(d, w.white, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, &white, sizeof white)) {
            release_texture(d, w.white);
            return VK_NULL_HANDLE;
        }
    }
    if (!w.white.copied) copy_texture(d, r, cb, w.white);
    if (!texture || !texture->data) return w.white.view;
    WorldTextureGpu& t = w.textures[texture->data.get()];
    if (!t.image) {
        const size_t bytes =
            bc7_bytes(texture->width, texture->height, texture->levels, half_blocks(texture->format));
        t.data = texture->data;
        const VkFormat format = texture->format == TextureFormat::bc1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
            : texture->format == TextureFormat::bc4                   ? VK_FORMAT_BC4_UNORM_BLOCK
            : texture->format == TextureFormat::bc5                   ? VK_FORMAT_BC5_UNORM_BLOCK
                                                                      : VK_FORMAT_BC7_UNORM_BLOCK;
        if (!make_texture(d, t, format, static_cast<uint32_t>(texture->width),
                static_cast<uint32_t>(texture->height), static_cast<uint32_t>(texture->levels), texture->data->data(),
                bytes)) {
            log_line("world textures: a texture of %dx%d not made", texture->width, texture->height);
            release_texture(d, t);
            w.textures.erase(texture->data.get());
            return w.white.view;
        }
        static const char* kNames[4] = {"BC7", "BC1", "BC4", "BC5"};
        log_line("world textures: %dx%d %s, %d levels (%zu bytes) made", texture->width, texture->height,
            kNames[static_cast<int>(texture->format)], texture->levels, bytes);
    }
    if (!t.copied) copy_texture(d, r, cb, t);
    t.used_frame = r.next_frame;
    return t.view;
}

// The image shown (PRESENT_SRC, the swapchain made copyable) copied into
// `copy`, a quarter of its size, its smaller levels made: all left in
// SHADER_READ_ONLY_OPTIMAL; false when it cannot be. `what` names it in the log.
bool shrink_screen(DeviceData& d, VkCommandBuffer cb, VkImage shown, VkExtent2D extent, ScreenCopy& copy,
        const char* what) {
    const VkExtent2D small{std::max(1u, extent.width / 4), std::max(1u, extent.height / 4)};
    if (!copy.image || copy.extent.width != small.width || copy.extent.height != small.height) {
        if (copy.view) d.fn.DestroyImageView(d.device, copy.view, nullptr);
        if (copy.image) d.fn.DestroyImage(d.device, copy.image, nullptr);
        if (copy.memory) d.fn.FreeMemory(d.device, copy.memory, nullptr);
        copy.image = VK_NULL_HANDLE;
        copy.memory = VK_NULL_HANDLE;
        copy.view = VK_NULL_HANDLE;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {small.width, small.height, 1};
        info.mipLevels = depth::kLightLevels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (d.fn.CreateImage(d.device, &info, nullptr, &copy.image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        d.fn.GetImageMemoryRequirements(d.device, copy.image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            d.fn.AllocateMemory(d.device, &allocate, nullptr, &copy.memory) != VK_SUCCESS ||
            d.fn.BindImageMemory(d.device, copy.image, copy.memory, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = copy.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, depth::kLightLevels, 0, 1};
        if (d.fn.CreateImageView(d.device, &view_info, nullptr, &copy.view) != VK_SUCCESS) return false;
        copy.extent = small;
        log_line("%s: the image shown copied into %ux%u", what, small.width, small.height);
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier before[2]{};
    before[0].sType = before[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // The image shown, put back in its layout.
    const VkImageLayout layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    before[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[0].oldLayout = layout;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].image = shown;
    before[1].srcAccessMask = 0;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].image = copy.image;
    for (auto& barrier : before) {
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = range;
    }
    before[1].subresourceRange.levelCount = depth::kLightLevels;
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
        nullptr, 2, before);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {static_cast<int32_t>(small.width), static_cast<int32_t>(small.height), 1};
    d.fn.CmdBlitImage(cb, shown, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);
    VkImageMemoryBarrier after = before[0];
    after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after.newLayout = layout;
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        1, &after);
    // Its smaller levels: the light, smooth.
    depth::make_light_levels(cb, copy.image, small, d.fn.CmdPipelineBarrier, d.fn.CmdBlitImage);
    return true;
}

// The HUD blur's targets: our image between its two passes, and a
// framebuffer on the layer's image (made again when either changes); false
// when they cannot be.
bool hud_blur_targets(DeviceData& d, Renderer& r, const depth::HudBlur& target) {
    WorldDepthPipeline& w = r.world;
    if (!w.hud_temp || w.hud_temp_extent.width != target.extent.width ||
            w.hud_temp_extent.height != target.extent.height) {
        if (w.hud_temp_framebuffer) d.fn.DestroyFramebuffer(d.device, w.hud_temp_framebuffer, nullptr);
        if (w.hud_temp_view) d.fn.DestroyImageView(d.device, w.hud_temp_view, nullptr);
        if (w.hud_temp) d.fn.DestroyImage(d.device, w.hud_temp, nullptr);
        if (w.hud_temp_memory) d.fn.FreeMemory(d.device, w.hud_temp_memory, nullptr);
        w.hud_temp_framebuffer = VK_NULL_HANDLE;
        w.hud_temp_view = VK_NULL_HANDLE;
        w.hud_temp = VK_NULL_HANDLE;
        w.hud_temp_memory = VK_NULL_HANDLE;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (d.fn.CreateImage(d.device, &info, nullptr, &w.hud_temp) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        d.fn.GetImageMemoryRequirements(d.device, w.hud_temp, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            d.fn.AllocateMemory(d.device, &allocate, nullptr, &w.hud_temp_memory) != VK_SUCCESS ||
            d.fn.BindImageMemory(d.device, w.hud_temp, w.hud_temp_memory, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = w.hud_temp;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (d.fn.CreateImageView(d.device, &view_info, nullptr, &w.hud_temp_view) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer_info.renderPass = w.hud_blur_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &w.hud_temp_view;
        framebuffer_info.width = target.extent.width;
        framebuffer_info.height = target.extent.height;
        framebuffer_info.layers = 1;
        if (d.fn.CreateFramebuffer(d.device, &framebuffer_info, nullptr, &w.hud_temp_framebuffer) != VK_SUCCESS)
            return false;
        w.hud_temp_extent = target.extent;
    }
    if (w.hud_target != target.image || !w.hud_target_framebuffer) {
        if (w.hud_target_framebuffer) d.fn.DestroyFramebuffer(d.device, w.hud_target_framebuffer, nullptr);
        if (w.hud_target_view) d.fn.DestroyImageView(d.device, w.hud_target_view, nullptr);
        w.hud_target_framebuffer = VK_NULL_HANDLE;
        w.hud_target_view = VK_NULL_HANDLE;
        w.hud_target = target.image;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = target.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        w.hud_history = false;
        if (d.fn.CreateImageView(d.device, &view_info, nullptr, &w.hud_target_view) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer_info.renderPass = w.hud_blur_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &w.hud_target_view;
        framebuffer_info.width = target.extent.width;
        framebuffer_info.height = target.extent.height;
        framebuffer_info.layers = 1;
        if (d.fn.CreateFramebuffer(d.device, &framebuffer_info, nullptr, &w.hud_target_framebuffer) != VK_SUCCESS)
            return false;
    }
    return true;
}

// The HUD's blurred scene (game_depth.cpp, hud_blur_target): the image shown
// with the world items (the game's interface in its layer: the scene and the
// build alone) made a quarter of its size, then blurred at the game's blur's
// size as the game does (a Gaussian, sigma 1.25 of its texels, 31/10
// capture), across into our image, down into the layer's; left in
// SHADER_READ_ONLY_OPTIMAL, for the game's next frame. Down, mixed with our
// last blur when the layer's image holds it (kHudBlurNew new).

bool fill_hud_blur(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot, VkImage shown, VkExtent2D extent,
        const depth::HudBlur& target) {
    WorldDepthPipeline& w = r.world;
    if (!target.image || !w.hud_blur_pipeline || !hud_blur_targets(d, r, target) ||
            !shrink_screen(d, cb, shown, extent, w.hud_small, "HUD blur"))
        return false;
    // Across: the quarter-size image read between its texels; down: the first pass.
    const VkImageView sources[2] = {w.hud_small.view, w.hud_temp_view};
    const VkFramebuffer targets[2] = {w.hud_temp_framebuffer, w.hud_target_framebuffer};
    VkDescriptorImageInfo infos[2] = {
        {w.light_sampler, sources[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {w.light_sampler, sources[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = w.hud_blur_sets[slot * 2 + i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    d.fn.UpdateDescriptorSets(d.device, 2, writes, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(target.extent.width),
        static_cast<float>(target.extent.height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, target.extent};
    for (int i = 0; i < 2; ++i) {
        const bool blend = i == 1 && w.hud_history;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = blend ? w.hud_blend_pass : w.hud_blur_pass;
        begin.framebuffer = targets[i];
        begin.renderArea = area;
        d.fn.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
        d.fn.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blend ? w.hud_blend_pipeline : w.hud_blur_pipeline);
        d.fn.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w.hud_blur_layout, 0, 1,
            &w.hud_blur_sets[slot * 2 + i], 0, nullptr);
        const float constants[6] = {1.0f / target.extent.width, 1.0f / target.extent.height, i == 0 ? 1.0f : 0.0f,
            i == 0 ? 0.0f : 1.0f, 0.0f, kHudBlurSigma};
        d.fn.CmdPushConstants(cb, w.hud_blur_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, constants);
        d.fn.CmdSetViewport(cb, 0, 1, &viewport);
        d.fn.CmdSetScissor(cb, 0, 1, &area);
        d.fn.CmdDraw(cb, 3, 1, 0, 0);
        d.fn.CmdEndRenderPass(cb);
    }
    w.hud_history = true;
    if (!w.hud_blur_said) {
        w.hud_blur_said = true;
        log_line("HUD blur: the scene with the world items blurred into %ux%u for the game's next frame (sigma %.2f, "
            "%.0f %% new)", target.extent.width, target.extent.height, kHudBlurSigma, kHudBlurNew * 100.0f);
    }
    return true;
}

// The interface layer of the image shown (game_depth.cpp), made readable
// and named in the slot's set, for the pass laying it back; false when it
// cannot be.
bool prepare_interface_layer(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot,
        const depth::InterfaceLayer& layer) {
    WorldDepthPipeline& w = r.world;
    if (!w.interface_pipeline || !layer.image) return false;
    // The game's passes wrote it (in an earlier submission, the present's
    // semaphores waited for): readable by our fragment shader.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = layer.layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = layer.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
    VkDescriptorImageInfo info{w.sampler, layer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = w.interface_sets[slot];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    d.fn.UpdateDescriptorSets(d.device, 1, &write, 0, nullptr);
    if (!w.interface_said) {
        w.interface_said = true;
        log_line("interface layer: the game's interface (#%d) laid back over the world items, under the zones", layer.id);
    }
    return true;
}

// The game's interface layer laid back over what is drawn (in the zones' pass).
void draw_interface_layer(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot, VkExtent2D extent) {
    WorldDepthPipeline& w = r.world;
    d.fn.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w.interface_pipeline);
    d.fn.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, w.interface_layout, 0, 1, &w.interface_sets[slot], 0,
        nullptr);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    d.fn.CmdSetViewport(cb, 0, 1, &viewport);
    d.fn.CmdSetScissor(cb, 0, 1, &scissor);
    d.fn.CmdDraw(cb, 3, 1, 0, 0);
}

// ImGui's draw callback between the world-only zones and the others: our HUD
// blur made from the image shown with every world item on it (the zones'
// pass ended for it, then begun again), then the game's interface layer laid
// back there (the path's points and line under it, the panels over it).
void lay_interface(const ImDrawList*, const ImDrawCmd* command) {
    auto* r = static_cast<Renderer*>(command->UserCallbackData);
    if (!r || !r->world.interface_ready || r->world.interface_laid || !r->world.interface_device) return;
    auto* state = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (!state) return;
    DeviceData& d = *r->world.interface_device;
    WorldDepthPipeline& w = r->world;
    if (w.hud_fill) {
        d.fn.CmdEndRenderPass(state->CommandBuffer);
        w.hud_filled = fill_hud_blur(d, *r, state->CommandBuffer, w.interface_slot, w.hud_shown, w.interface_extent,
            w.hud_blur_into);
        d.fn.CmdBeginRenderPass(state->CommandBuffer, &w.zones_begin, VK_SUBPASS_CONTENTS_INLINE);
    }
    draw_interface_layer(d, *r, state->CommandBuffer, w.interface_slot, w.interface_extent);
    w.interface_laid = true;
}

// Staging buffers of textures copied frames ago freed; textures no item holds
// any more, and unused for a few frames, released.
void tidy_textures(DeviceData& d, Renderer& r) {
    WorldDepthPipeline& w = r.world;
    for (auto entry = w.textures.begin(); entry != w.textures.end();) {
        WorldTextureGpu& t = entry->second;
        if (t.staging && t.copied && r.next_frame - t.copied_frame > kFrames + 1) {
            d.fn.DestroyBuffer(d.device, t.staging, nullptr);
            d.fn.FreeMemory(d.device, t.staging_memory, nullptr);
            t.staging = VK_NULL_HANDLE;
            t.staging_memory = VK_NULL_HANDLE;
        }
        if (t.data.expired() && r.next_frame - t.used_frame > kFrames + 1) {
            release_texture(d, t);
            entry = w.textures.erase(entry);
        } else {
            ++entry;
        }
    }
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
    // The world's blocks (the wire view in Remove mode), packed once per grid;
    // a small buffer bound when there are none.
    std::vector<const VoxelMesh*> meshes;
    for (const auto& part : draw.blocks_build) meshes.push_back(part.mesh.get());
    if (draw.blocks && (w.packed_of != draw.blocks || w.packed_meshes != meshes)) {
        const WorldSolids& grid = *draw.blocks;
        // Filled cells (the solids and the build's), then the build's alone.
        const size_t words = (grid.filled.size() + 31) / 32;
        w.packed = grid.bits;
        w.packed.resize(2 * words, 0u);
        // The build's cells: the blocks it removes.
        for (const auto& part : draw.blocks_build) {
            const VoxelMesh& mesh = *part.mesh;
            size_t i = 0;
            for (int z = 0; z < mesh.sz; ++z)
                for (int y = 0; y < mesh.sy; ++y)
                    for (int x = 0; x < mesh.sx; ++x, ++i) {
                        if (i >= mesh.drawn.size() || !mesh.drawn[i]) continue;
                        const int gx = x + part.at[0], gy = y + part.at[1], gz = z + part.at[2];
                        if (gx < 0 || gy < 0 || gz < 0 || gx >= grid.sx || gy >= grid.sy || gz >= grid.sz) continue;
                        const size_t at = (static_cast<size_t>(gz) * grid.sy + gy) * grid.sx + gx;
                        w.packed[at >> 5] |= 1u << (at & 31);
                        w.packed[words + (at >> 5)] |= 1u << (at & 31);
                    }
        }
        w.packed_of = draw.blocks;
        w.packed_meshes = meshes;
        w.packed_keep.clear();
        for (const auto& part : draw.blocks_build) w.packed_keep.push_back(part.mesh);
        ++w.packing;
    }
    const VkDeviceSize blocks_bytes = draw.blocks ? w.packed.size() * sizeof(std::uint32_t) : 0;
    if (!ensure_buffer(d, w.blocks[slot], w.blocks_memory[slot], w.blocks_mapped[slot], w.blocks_size[slot],
            std::max<VkDeviceSize>(blocks_bytes, 4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        log_line("game depth: world blocks buffer unavailable");
        return false;
    }
    if (draw.blocks && w.blocks_in[slot] != w.packing) {
        std::memcpy(w.blocks_mapped[slot], w.packed.data(), blocks_bytes);
        w.blocks_in[slot] = w.packing;
    }
    VkDescriptorBufferInfo blocks_info{w.blocks[slot], 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet blocks_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    blocks_write.dstSet = w.sets[slot];
    blocks_write.dstBinding = 2;
    blocks_write.descriptorCount = 1;
    blocks_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    blocks_write.pBufferInfo = &blocks_info;
    d.fn.UpdateDescriptorSets(d.device, 1, &blocks_write, 0, nullptr);
    // The solid style's textures (or white), made and copied as needed.
    tidy_textures(d, r);
    VkDescriptorImageInfo texture_infos[3];
    for (int i = 0; i < 3; ++i) {
        const VkImageView view_of = texture_view(d, r, cb, draw.textured ? &draw.textures[i] : nullptr);
        if (!view_of) return false;
        texture_infos[i] = {w.texture_sampler, view_of, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet textures_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    textures_write.dstSet = w.sets[slot];
    textures_write.dstBinding = 3;
    textures_write.descriptorCount = 3;
    textures_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textures_write.pImageInfo = texture_infos;
    d.fn.UpdateDescriptorSets(d.device, 1, &textures_write, 0, nullptr);
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
    // Our copy is already sampled as it is; the game's own image goes read-only.
    const bool as_is = scene.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo image{w.sampler, view,
        as_is ? scene.layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    // Ours, sampled by the wire view only (the solid view's pass writes it).
    VkDescriptorImageInfo own{w.sampler, w.depth_view ? w.depth_view : view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet own_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    own_write.dstSet = w.sets[slot];
    own_write.dstBinding = 1;
    own_write.descriptorCount = 1;
    own_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    own_write.pImageInfo = &own;
    d.fn.UpdateDescriptorSets(d.device, 1, &own_write, 0, nullptr);
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = w.sets[slot];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    d.fn.UpdateDescriptorSets(d.device, 1, &write, 0, nullptr);
    if (as_is) return true;
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

// Our depth image the size of the screen, and the framebuffer of the
// presented image's view with it; false when they cannot be made.
bool world_target(DeviceData& d, Renderer& r, VkExtent2D extent, VkImageView view, VkFramebuffer& framebuffer) {
    WorldDepthPipeline& w = r.world;
    if (!w.depth || w.depth_extent.width != extent.width || w.depth_extent.height != extent.height) {
        wait_frames(d, r);
        for (auto& [old_view, old] : w.framebuffers) d.fn.DestroyFramebuffer(d.device, old, nullptr);
        w.framebuffers.clear();
        if (w.depth_framebuffer) d.fn.DestroyFramebuffer(d.device, w.depth_framebuffer, nullptr);
        w.depth_framebuffer = VK_NULL_HANDLE;
        if (w.depth_view) d.fn.DestroyImageView(d.device, w.depth_view, nullptr);
        if (w.depth) d.fn.DestroyImage(d.device, w.depth, nullptr);
        if (w.depth_memory) d.fn.FreeMemory(d.device, w.depth_memory, nullptr);
        w.depth_view = VK_NULL_HANDLE;
        w.depth = VK_NULL_HANDLE;
        w.depth_memory = VK_NULL_HANDLE;
        w.depth_extent = extent;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_D32_SFLOAT;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (d.fn.CreateImage(d.device, &info, nullptr, &w.depth) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        d.fn.GetImageMemoryRequirements(d.device, w.depth, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX) return false;
        if (d.fn.AllocateMemory(d.device, &allocate, nullptr, &w.depth_memory) != VK_SUCCESS) return false;
        if (d.fn.BindImageMemory(d.device, w.depth, w.depth_memory, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = w.depth;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_D32_SFLOAT;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (d.fn.CreateImageView(d.device, &view_info, nullptr, &w.depth_view) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo depth_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        depth_info.renderPass = w.depth_pass;
        depth_info.attachmentCount = 1;
        depth_info.pAttachments = &w.depth_view;
        depth_info.width = extent.width;
        depth_info.height = extent.height;
        depth_info.layers = 1;
        if (d.fn.CreateFramebuffer(d.device, &depth_info, nullptr, &w.depth_framebuffer) != VK_SUCCESS) return false;
    }
    VkFramebuffer& made = w.framebuffers[view];
    if (!made) {
        const VkImageView attachments[2] = {view, w.depth_view};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = w.pass;
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        if (d.fn.CreateFramebuffer(d.device, &info, nullptr, &made) != VK_SUCCESS) {
            w.framebuffers.erase(view);
            return false;
        }
    }
    framebuffer = made;
    return true;
}

// In the world's pass (mode 0), the wire view's depth pass (1, the faces
// alone) or the zones' pass (2, the wire view; 4 the world's blocks telling
// what hides an edge).
void draw_world_depth(DeviceData& d, Renderer& r, VkCommandBuffer cb, uint32_t slot, const depth::SceneDepth& scene,
        VkExtent2D screen, VkPipeline pipeline, int mode, size_t indices) {
    WorldDepthPipeline& w = r.world;
    d.fn.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
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
    constants.mode = mode;
    // Wire: an edge hidden by something 30 cm (or the largest face offset
    // and 10 cm) or more before our nearest face is left out (a tree, a
    // wall); nearer, it is the build's own block.
    constants.own_tolerance = static_cast<float>(r.world_draw.own_tolerance / r.world_draw.near_distance);
    constants.focal = static_cast<float>(r.world_draw.focal_pixels);
    constants.dash_alpha = static_cast<float>(r.world_draw.dash_alpha);
    constants.back_alpha =
        static_cast<float>(r.world_draw.back_alpha >= 0.0 ? r.world_draw.back_alpha : r.world_draw.dash_alpha);
    for (int i = 0; i < 3; ++i) constants.tiling[i] = r.world_draw.textures[i].tiling;
    constants.relief = static_cast<float>(r.world_draw.relief);
    int normal_faces = 0;
    for (int i = 0; i < 3; ++i)
        if (r.world_draw.textured && r.world_draw.textures[i].data) normal_faces |= 1 << i;
    constants.normal_faces = static_cast<float>(normal_faces);
    // The solid style textured: the eye in meters from the texture origin.
    if (r.world_draw.textured)
        for (int i = 0; i < 3; ++i) constants.eye[i] = static_cast<float>(r.world_draw.texture_eye[i]);
    if (const auto& blocks = r.world_draw.blocks) {
        for (int i = 0; i < 3; ++i) constants.eye[i] = static_cast<float>(r.world_draw.blocks_eye[i]);
        constants.eye[3] = static_cast<float>(r.world_draw.blocks_relief / blocks->cell);
        constants.size[0] = blocks->sx;
        constants.size[1] = blocks->sy;
        constants.size[2] = blocks->sz;
    }
    d.fn.CmdPushConstants(cb, w.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants,
        &constants);
    const VkDeviceSize offset = 0;
    d.fn.CmdBindVertexBuffers(cb, 0, 1, &w.vertex[slot], &offset);
    d.fn.CmdBindIndexBuffer(cb, w.index[slot], 0, VK_INDEX_TYPE_UINT32);
    d.fn.CmdDrawIndexed(cb, static_cast<uint32_t>(indices), 1, 0, 0, 0);
    if (!w.drawn_logged) {
        w.drawn_logged = true;
        log_line("game depth: first world cells drawn against img#%d (%zu triangles)", scene.id,
            r.world_draw.indices.size() / 3);
    }
}

// The camera of a GPU camera block (row-major view-projection, world
// translation; row 3 the gaze, rows 0 and 1 the screen's axes scaled, with
// the anti-aliasing's jitter along the gaze), its axes turned like
// `reference`'s (the input camera): right x up = gaze.
bool camera_of(const depth::gpu::Camera& gpu, const LocalCamera& reference, LocalCamera& out) {
    const float* m = gpu.matrix;
    double forward[3] = {m[12], m[13], m[14]};
    const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto normalize = [&](double* v) {
        const double n = std::sqrt(dot(v, v));
        if (n < 1e-9) return false;
        for (int i = 0; i < 3; ++i) v[i] /= n;
        return true;
    };
    if (!normalize(forward)) return false;
    double side[3] = {m[0], m[1], m[2]};
    const double side_along = dot(side, forward);
    for (int i = 0; i < 3; ++i) side[i] -= side_along * forward[i];
    const double row0 = std::sqrt(dot(side, side));
    double up[3] = {m[4], m[5], m[6]};
    const double along = dot(up, forward);
    for (int i = 0; i < 3; ++i) up[i] -= along * forward[i];
    const double row1 = std::sqrt(dot(up, up));
    if (!normalize(up)) return false;
    // The input camera's up: the row's sign follows the screen's y.
    const double* q = reference.orientation;
    // (0, 1, 0) turned by q: v + 2w (q x v) + 2 q x (q x v), as world_view.
    const double tx = -2.0 * q[2], ty = 0.0, tz = 2.0 * q[0];
    const double ref_up[3] = {q[3] * tx + (q[1] * tz - q[2] * ty), 1.0 + q[3] * ty + (q[2] * tx - q[0] * tz),
        q[3] * tz + (q[0] * ty - q[1] * tx)};
    if (dot(up, ref_up) < 0.0)
        for (double& v : up) v = -v;
    const double right[3] = {up[1] * forward[2] - up[2] * forward[1], up[2] * forward[0] - up[0] * forward[2],
        up[0] * forward[1] - up[1] * forward[0]};
    // The rotation with columns right, up, forward, as a quaternion.
    const double r[3][3] = {{right[0], up[0], forward[0]}, {right[1], up[1], forward[1]}, {right[2], up[2], forward[2]}};
    double x, y, z, w;
    const double trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0) {
        const double s = 0.5 / std::sqrt(trace + 1.0);
        w = 0.25 / s, x = (r[2][1] - r[1][2]) * s, y = (r[0][2] - r[2][0]) * s, z = (r[1][0] - r[0][1]) * s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]);
        w = (r[2][1] - r[1][2]) / s, x = 0.25 * s, y = (r[0][1] + r[1][0]) / s, z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]);
        w = (r[0][2] - r[2][0]) / s, x = (r[0][1] + r[1][0]) / s, y = 0.25 * s, z = (r[1][2] + r[2][1]) / s;
    } else {
        const double s = 2.0 * std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]);
        w = (r[1][0] - r[0][1]) / s, x = (r[0][2] + r[2][0]) / s, y = (r[1][2] + r[2][1]) / s, z = 0.25 * s;
    }
    out = reference;
    out.orientation[0] = x, out.orientation[1] = y, out.orientation[2] = z, out.orientation[3] = w;
    for (int i = 0; i < 3; ++i) out.position[i] = gpu.eye[i];
    out.fov_y = 2.0 * std::atan(1.0 / row1);
    out.fov_x = row0 > 1e-9 ? 2.0 * std::atan(1.0 / row0) : 0.0;
    // Projected with the matrix itself (30/10: axes and focals worked out
    // from it still made world items slide as the view turned).
    out.has_matrix = true;
    for (int i = 0; i < 16; ++i) out.matrix[i] = m[i];
    // How it compares with the input camera, now and then (for the KB).
    static ULONGLONG last_log = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last_log >= 10000) {
        last_log = now;
        const double tx2 = 2.0 * (q[1] * 1.0 - q[2] * 0.0), ty2 = 2.0 * (q[2] * 0.0 - q[0] * 1.0), tz2 = 0.0;
        const double ref_forward[3] = {q[3] * tx2 + (q[1] * tz2 - q[2] * ty2), q[3] * ty2 + (q[2] * tx2 - q[0] * tz2),
            1.0 + q[3] * tz2 + (q[0] * ty2 - q[1] * tx2)};
        double row_right[3] = {side[0] / std::max(row0, 1e-9), side[1] / std::max(row0, 1e-9), side[2] / std::max(row0, 1e-9)};
        log_line("gpu camera: vs the input camera: gaze %.3f deg apart, right %.3f (row 0 %.3f), up %.3f; fov y %.2f deg "
            "(input %.2f), fov x %.2f deg, aspect %.4f (input %.4f)",
            std::acos(std::clamp(dot(forward, ref_forward), -1.0, 1.0)) * 57.29578, dot(right, row_right),
            dot(row_right, right), dot(up, ref_up), out.fov_y * 57.29578, reference.fov_y * 57.29578,
            out.fov_x * 57.29578, row1 / std::max(row0, 1e-9), reference.aspect);
    }
    return true;
}

// After the zones' pass: the game's depth back in its layout.
void finish_world(DeviceData& d, VkCommandBuffer cb, const depth::SceneDepth& scene) {
    if (scene.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
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
    // The game's interface drawn into its layer in the next frames: only
    // after a frame drawn here with the world items under it (whatever
    // returns early says no: the overlay could not lay it back).
    bool keep_layer = false;
    // Our HUD blur, filled at this present for the game's next frame.
    bool hud_blur = false;
    struct LayerWish {
        bool& keep;
        bool& blur;
        ~LayerWish() {
            depth::want_interface_layer(keep);
            depth::hud_blur_filled(blur);
        }
    } layer_wish{keep_layer, hud_blur};
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
    // Nothing to draw, unless the game's interface went to its layer for
    // this image: it must be laid back.
    if (!scene_has_visible_zones() && !depth::take_interface_layer(target->images[image_index]).image)
        return VK_NULL_HANDLE;

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
        take_depth(r, slot, r.world_draw.near_distance);
    }

    // Build this frame's draw lists.
    const SceneSnapshot scene = snapshot_scene();
    // The interface layer and the HUD blur, while a world item asks for them.
    bool layer_asked = false, blur_asked = false;
    for (const auto& zone : scene.zones)
        for (const Item& item : zone->items)
            if (item.kind == ItemKind::World) {
                layer_asked = layer_asked || item.world_under_interface;
                blur_asked = blur_asked || (item.world_under_interface && item.world_hud_blur);
            }
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
    // The game's depth: this frame's scene depth, and the world pipeline when
    // it can be read.
    const depth::SceneDepth scene_depth = depth::scene_depth(target->extent);
    r.world_draw.vertices.clear();
    r.world_draw.indices.clear();
    r.world_draw.solid = false;
    r.world_draw.wire = false;
    r.world_draw.own_tolerance = 0.3;
    r.world_draw.focal_pixels = 0.0;
    r.world_draw.blocks.reset();
    r.world_draw.blocks_build.clear();
    r.world_draw.blocks_relief = 0.0;
    r.world_draw.dash_alpha = 220.0 / 255.0;
    r.world_draw.back_alpha = -1.0;
    r.world_draw.textured = false;
    r.world_draw.relief = 1.0f;
    r.world_draw.textures = {};
    // The game's interface layer of the image shown, if it went there: it
    // must be laid back at this present.
    const VkImage shown_image = target->images[image_index];
    const depth::InterfaceLayer layer = depth::take_interface_layer(shown_image);
    r.world.interface_ready = false;
    r.world.interface_laid = false;
    r.world.interface_device = &d;
    r.world.interface_slot = slot;
    r.world.interface_extent = target->extent;
    r.world_draw.face_indices = 0;
    // The scene's camera from the GPU, when found.
    LocalCamera gpu_camera;
    const bool gpu_known = scene_depth.gpu_camera.valid && scene_depth.has_camera &&
        camera_of(scene_depth.gpu_camera, scene_depth.camera, gpu_camera);
    r.world_draw.near_distance = depth::kNearDistance;
    const bool world_ready = scene_depth.image && scene_depth.format == VK_FORMAT_D32_SFLOAT &&
        scene_depth.layout != VK_IMAGE_LAYOUT_UNDEFINED && scene_depth.layout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
        ensure_world(d, r, target->format, pass);
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
        // The scene's camera from the GPU; until it is found, the one read as
        // the depth's copy was recorded; else (false) the one read now.
        [&](LocalCamera& camera) {
            if (gpu_known) {
                camera = gpu_camera;
                return true;
            }
            if (!scene_depth.has_camera) return false;
            camera = scene_depth.camera;
            return true;
        },
        [&]() { r.depth_picture_until = r.next_frame + 30; },
        [&](ImDrawList& list) {
            if (!layer.image) return;
            list.AddCallback(lay_interface, &r);
            list.AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        },
    };
    draw_scene(*ImGui::GetForegroundDrawList(), scene, io.DisplaySize, resources);
    // The picture of the game's depth, while a world item asks for it (a
    // diagnostic): read back at most every 6th frame.
    const bool picture = r.next_frame <= r.depth_picture_until;
    if (picture && r.depth.texture && r.depth.texture->Status != ImTextureStatus_WantCreate) {
        // Top right, a quarter of the screen across.
        ImDrawList& list = *ImGui::GetForegroundDrawList();
        const float width = io.DisplaySize.x / 4.0f;
        const float height = width * r.depth.texture->Height / std::max(1, r.depth.texture->Width);
        const ImVec2 from(io.DisplaySize.x - 16.0f - width, 16.0f), to(io.DisplaySize.x - 16.0f, 16.0f + height);
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
    if (picture && r.next_frame % 6 == 0) record_depth(d, r, cb, slot, scene_depth);
    VkFramebuffer world_framebuffer{};
    r.world.interface_ready = layer.image && prepare_interface_layer(d, r, cb, slot, layer);
    const bool world_drawn = world_ready && world_target(d, r, target->extent, target->views[image_index], world_framebuffer) &&
        prepare_world(d, r, cb, slot, scene_depth);
    const bool wire = world_drawn && r.world_draw.wire;
    if (wire) {
        // Our faces' nearest depth, sampled by the wire view in the zones' pass.
        VkClearValue clear{};
        clear.depthStencil = {0.0f, 0};
        VkRenderPassBeginInfo depth_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        depth_begin.renderPass = r.world.depth_pass;
        depth_begin.framebuffer = r.world.depth_framebuffer;
        depth_begin.renderArea.extent = target->extent;
        depth_begin.clearValueCount = 1;
        depth_begin.pClearValues = &clear;
        d.fn.CmdBeginRenderPass(cb, &depth_begin, VK_SUBPASS_CONTENTS_INLINE);
        if (r.world_draw.face_indices)
            draw_world_depth(d, r, cb, slot, scene_depth, target->extent, r.world.depth_pipeline, 1,
                r.world_draw.face_indices);
        d.fn.CmdEndRenderPass(cb);
    } else if (world_drawn) {
        VkClearValue clears[2]{};
        clears[1].depthStencil = {0.0f, 0};  // reversed: 0 is the farthest
        VkRenderPassBeginInfo world_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        world_begin.renderPass = r.world.pass;
        world_begin.framebuffer = world_framebuffer;
        world_begin.renderArea.extent = target->extent;
        world_begin.clearValueCount = 2;
        world_begin.pClearValues = clears;
        d.fn.CmdBeginRenderPass(cb, &world_begin, VK_SUBPASS_CONTENTS_INLINE);
        draw_world_depth(d, r, cb, slot, scene_depth, target->extent,
            r.world_draw.solid ? r.world.solid_pipeline : r.world.pipeline, 0, r.world_draw.indices.size());
        d.fn.CmdEndRenderPass(cb);
        // Its colors written before the zones' pass loads the image.
        VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        written.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d.fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &written, 0, nullptr, 0, nullptr);
    }
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
    // Our HUD blur, made in ImGui's callback: only from the scene alone (the
    // game's interface in its layer), in a format the game's blur shares.
    r.world.zones_begin = pass_begin;
    r.world.hud_shown = shown_image;
    r.world.hud_filled = false;
    r.world.hud_fill = blur_asked && r.world.interface_ready && target->copyable &&
        (target->format == VK_FORMAT_R8G8B8A8_UNORM || target->format == VK_FORMAT_B8G8R8A8_UNORM);
    if (r.world.hud_fill) {
        r.world.hud_blur_into = depth::hud_blur_target();
        r.world.hud_fill = r.world.hud_blur_into.image != VK_NULL_HANDLE;
    }
    d.fn.CmdBeginRenderPass(cb, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    if (wire)
        draw_world_depth(d, r, cb, slot, scene_depth, target->extent, r.world.wire_pipeline,
            r.world_draw.blocks ? 4 : 2, r.world_draw.indices.size());
    ImGui_ImplVulkan_RenderDrawData(zones, cb);
    // The game's interface layer, if ImGui's callback did not lay it (it
    // must be: the interface is not on the image).
    if (r.world.interface_ready && !r.world.interface_laid) {
        draw_interface_layer(d, r, cb, slot, target->extent);
        r.world.interface_laid = true;
    }
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
    // The game's interface under its layer from now on, while a world item
    // asks for it (Architect Line: while it is active).
    keep_layer = layer_asked && r.world.interface_pipeline;
    hud_blur = r.world.hud_filled && keep_layer;
    if (!hud_blur) r.world.hud_history = false;
    if (!r.first_frame_logged) {
        r.first_frame_logged = true;
        log_line("first overlay frame drawn (%zu zones)", scene.zones.size());
    }
    return frame.done;
}

void release_swapchain(DeviceData& d, SwapchainData& swapchain) {
    if (swapchain.framebuffers.empty() && swapchain.views.empty()) return;
    if (d.renderer) wait_frames(d, *d.renderer);
    if (d.renderer)
        for (VkImageView view : swapchain.views) {
            auto found = d.renderer->world.framebuffers.find(view);
            if (found == d.renderer->world.framebuffers.end()) continue;
            d.fn.DestroyFramebuffer(d.device, found->second, nullptr);
            d.renderer->world.framebuffers.erase(found);
        }
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
