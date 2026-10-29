// Diagnostic: the game's render passes, as the layer sees them recorded and
// submitted, logged for a few frames (shroudforge_overlay.log). For drawing
// the world items against the game's depth: which pass draws the scene, with
// which depth image (format, size, load and store), where the interface
// starts. Off unless a file `render-probe.on` sits next to the layer: read
// once, when the game creates its device; off, no function is intercepted
// and nothing is paid.

#include "overlay.h"
#include "game_depth.h"
#include "render_probe.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

namespace overlay::probe {

namespace {

// The functions the probe intercepts, both names when an extension had it
// first: the game may ask for either.
#define PROBE_FUNCTIONS(X) \
    X(CreateImage, vkCreateImage) X(CreateImageView, vkCreateImageView) X(CreateRenderPass, vkCreateRenderPass) \
    X(CreateRenderPass2, vkCreateRenderPass2) X(CreateRenderPass2KHR, vkCreateRenderPass2KHR) \
    X(CreateFramebuffer, vkCreateFramebuffer) X(BeginCommandBuffer, vkBeginCommandBuffer) \
    X(CmdBeginRenderPass, vkCmdBeginRenderPass) X(CmdBeginRenderPass2, vkCmdBeginRenderPass2) \
    X(CmdBeginRenderPass2KHR, vkCmdBeginRenderPass2KHR) X(CmdBeginRendering, vkCmdBeginRendering) \
    X(CmdBeginRenderingKHR, vkCmdBeginRenderingKHR) X(CmdDraw, vkCmdDraw) X(CmdDrawIndexed, vkCmdDrawIndexed) \
    X(CmdDrawIndirect, vkCmdDrawIndirect) X(CmdDrawIndexedIndirect, vkCmdDrawIndexedIndirect) \
    X(CmdDrawIndirectCount, vkCmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount, vkCmdDrawIndexedIndirectCount) \
    X(CmdDrawIndirectCountKHR, vkCmdDrawIndirectCountKHR) \
    X(CmdDrawIndexedIndirectCountKHR, vkCmdDrawIndexedIndirectCountKHR) X(CmdDispatch, vkCmdDispatch) \
    X(CmdDispatchIndirect, vkCmdDispatchIndirect) X(CmdCopyImage, vkCmdCopyImage) X(CmdBlitImage, vkCmdBlitImage) \
    X(CmdExecuteCommands, vkCmdExecuteCommands) X(QueueSubmit, vkQueueSubmit) X(QueueSubmit2, vkQueueSubmit2) \
    X(QueueSubmit2KHR, vkQueueSubmit2KHR)

struct Next {
#define PROBE_DECLARE(name, type) PFN_##type name{};
    PROBE_FUNCTIONS(PROBE_DECLARE)
#undef PROBE_DECLARE
};

Next next;
VkDevice probed_device{};

// Frames logged: one at kFirstFrame, then one every kEveryFrames, kFrames
// in all (the passes change with what the game shows: menu, world, map).
constexpr int kFirstFrame = 600;
constexpr int kEveryFrames = 1800;
constexpr int kFrames = 6;

struct Image {
    int id = 0;
    VkFormat format{};
    VkExtent3D extent{};
    VkImageUsageFlags usage{};
    VkSampleCountFlagBits samples{};
    bool swapchain = false;
};

struct Attachment {
    const Image* image = nullptr;
    VkFormat format{};
    VkAttachmentLoadOp load{};
    VkAttachmentStoreOp store{};
};

struct RenderPassInfo {
    std::vector<Attachment> attachments;  // image unknown: from the framebuffer
};

// One pass of a command buffer, or the work between two (compute, copies).
struct Pass {
    bool dynamic = false;
    VkExtent2D extent{};
    std::vector<Attachment> colors;
    Attachment depth;
    int draws = 0;
    int dispatches_before = 0;
    int copies_before = 0;
};

struct Recording {
    std::vector<Pass> passes;
    int dispatches = 0, copies = 0;  // since the last pass
    bool in_pass = false;
};

std::mutex probe_mutex;
std::unordered_map<VkImage, Image> images;
std::unordered_map<VkImageView, const Image*> views;
std::unordered_map<VkRenderPass, RenderPassInfo> render_passes;
std::unordered_map<VkFramebuffer, std::vector<const Image*>> framebuffers;
std::unordered_map<VkCommandBuffer, Recording> recordings;
int next_image_id = 1;

// The frame being submitted: its passes in order, with their queue.
struct SubmittedPass {
    Pass pass;
    uint32_t queue = 0;
};
std::vector<SubmittedPass> frame;
std::unordered_map<VkQueue, uint32_t> queue_ids;
int frame_number = 0;
int frames_logged = 0;

// The recording of a command buffer: its node, cached per thread (draws
// come by the thousands; nodes of an unordered_map stay in place).
thread_local VkCommandBuffer cached_buffer{};
thread_local Recording* cached_recording{};

Recording& recording_of(VkCommandBuffer buffer) {
    if (buffer == cached_buffer && cached_recording) return *cached_recording;
    std::lock_guard lock(probe_mutex);
    cached_buffer = buffer;
    cached_recording = &recordings[buffer];
    return *cached_recording;
}

const char* format_name(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM: return "D16";
    case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8D24";
    case VK_FORMAT_D32_SFLOAT: return "D32";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
    case VK_FORMAT_S8_UINT: return "S8";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11F";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    case VK_FORMAT_R32G32_UINT: return "RG32UI";
    case VK_FORMAT_R8_UNORM: return "R8";
    case VK_FORMAT_R8G8_UNORM: return "RG8";
    case VK_FORMAT_R32_UINT: return "R32UI";
    default: return nullptr;
    }
}

bool depth_format(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D32_SFLOAT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_S8_UINT;
}

std::string format_text(VkFormat format) {
    const char* name = format_name(format);
    return name ? name : "fmt" + std::to_string(static_cast<int>(format));
}

const char* load_name(VkAttachmentLoadOp load) {
    switch (load) {
    case VK_ATTACHMENT_LOAD_OP_LOAD: return "load";
    case VK_ATTACHMENT_LOAD_OP_CLEAR: return "clear";
    case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return "dontcare";
    default: return "none";
    }
}

const char* store_name(VkAttachmentStoreOp store) {
    switch (store) {
    case VK_ATTACHMENT_STORE_OP_STORE: return "store";
    case VK_ATTACHMENT_STORE_OP_DONT_CARE: return "dontcare";
    default: return "none";
    }
}

std::string attachment_text(const Attachment& a) {
    std::string text = format_text(a.image ? a.image->format : a.format);
    if (a.image) {
        text += a.image->swapchain ? " swapchain" : " img#" + std::to_string(a.image->id);
        text += " " + std::to_string(a.image->extent.width) + "x" + std::to_string(a.image->extent.height);
    }
    return text + " " + load_name(a.load) + "/" + store_name(a.store);
}

std::string image_text(const Image& image) {
    char text[160];
    snprintf(text, sizeof text, "img#%d %s %ux%ux%u usage 0x%x samples %d", image.id, format_text(image.format).c_str(),
        image.extent.width, image.extent.height, image.extent.depth, image.usage, static_cast<int>(image.samples));
    return text;
}

void log_frame() {
    // Called under probe_mutex.
    log_line("render probe: frame %d, %zu passes", frame_number, frame.size());
    int index = 0;
    for (const SubmittedPass& entry : frame) {
        const Pass& pass = entry.pass;
        std::string colors;
        for (const Attachment& color : pass.colors) colors += (colors.empty() ? "" : ", ") + attachment_text(color);
        const bool depth = pass.depth.image || pass.depth.format != VK_FORMAT_UNDEFINED;
        log_line("render probe:  %3d q%u %s %ux%u draws %d (before: %d dispatches, %d copies) color [%s] depth [%s]", index++,
            entry.queue, pass.dynamic ? "dyn" : "rp ", pass.extent.width, pass.extent.height, pass.draws,
            pass.dispatches_before, pass.copies_before, colors.c_str(), depth ? attachment_text(pass.depth).c_str() : "-");
    }
}

uint32_t queue_id(VkQueue queue) {
    auto found = queue_ids.find(queue);
    if (found != queue_ids.end()) return found->second;
    const uint32_t id = static_cast<uint32_t>(queue_ids.size());
    queue_ids[queue] = id;
    return id;
}

void submitted(VkQueue queue, VkCommandBuffer buffer) {
    // Called under probe_mutex.
    auto found = recordings.find(buffer);
    if (found == recordings.end()) return;
    const uint32_t id = queue_id(queue);
    for (const Pass& pass : found->second.passes) frame.push_back({pass, id});
    // The work after the last pass: as a pass of its own, without target.
    if (found->second.dispatches || found->second.copies) {
        Pass tail;
        tail.dispatches_before = found->second.dispatches;
        tail.copies_before = found->second.copies;
        frame.push_back({tail, id});
    }
}

// --- Intercepted functions -------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkImage* image) {
    const VkResult result = next.CreateImage(device, info, allocator, image);
    if (result != VK_SUCCESS) return result;
    std::lock_guard lock(probe_mutex);
    Image& entry = images[*image];
    entry = {next_image_id++, info->format, info->extent, info->usage, info->samples, false};
    // Depth images and those of the screen's size are worth knowing.
    if (depth_format(info->format)) log_line("render probe: depth %s", image_text(entry).c_str());
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkImageView* view) {
    const VkResult result = next.CreateImageView(device, info, allocator, view);
    if (result != VK_SUCCESS) return result;
    std::lock_guard lock(probe_mutex);
    auto found = images.find(info->image);
    views[*view] = found == images.end() ? nullptr : &found->second;
    return result;
}

void note_render_pass(VkRenderPass pass, uint32_t count, const VkAttachmentDescription* attachments) {
    std::lock_guard lock(probe_mutex);
    RenderPassInfo& info = render_passes[pass];
    info.attachments.clear();
    for (uint32_t i = 0; i < count; ++i)
        info.attachments.push_back({nullptr, attachments[i].format, attachments[i].loadOp, attachments[i].storeOp});
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    const VkResult result = next.CreateRenderPass(device, info, allocator, pass);
    if (result == VK_SUCCESS) note_render_pass(*pass, info->attachmentCount, info->pAttachments);
    return result;
}

VkResult create_render_pass2(PFN_vkCreateRenderPass2 call, VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    const VkResult result = call(device, info, allocator, pass);
    if (result != VK_SUCCESS) return result;
    std::vector<VkAttachmentDescription> plain;
    for (uint32_t i = 0; i < info->attachmentCount; ++i) {
        const auto& a = info->pAttachments[i];
        plain.push_back({a.flags, a.format, a.samples, a.loadOp, a.storeOp, a.stencilLoadOp, a.stencilStoreOp,
            a.initialLayout, a.finalLayout});
    }
    note_render_pass(*pass, info->attachmentCount, plain.data());
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    return create_render_pass2(next.CreateRenderPass2, device, info, allocator, pass);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    return create_render_pass2(next.CreateRenderPass2KHR, device, info, allocator, pass);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    const VkResult result = next.CreateFramebuffer(device, info, allocator, framebuffer);
    if (result != VK_SUCCESS) return result;
    std::lock_guard lock(probe_mutex);
    std::vector<const Image*>& list = framebuffers[*framebuffer];
    list.clear();
    if (!(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
        for (uint32_t i = 0; i < info->attachmentCount; ++i) {
            auto found = views.find(info->pAttachments[i]);
            list.push_back(found == views.end() ? nullptr : found->second);
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
    Recording& recording = recording_of(buffer);
    recording.passes.clear();
    recording.dispatches = recording.copies = 0;
    recording.in_pass = false;
    return next.BeginCommandBuffer(buffer, info);
}

void begin_render_pass(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info) {
    Pass pass;
    pass.extent = info->renderArea.extent;
    std::lock_guard lock(probe_mutex);
    auto described = render_passes.find(info->renderPass);
    auto attached = framebuffers.find(info->framebuffer);
    // Imageless framebuffers: the views come with the begin.
    std::vector<const Image*> imageless;
    for (auto* chain = static_cast<const VkBaseInStructure*>(info->pNext); chain; chain = chain->pNext) {
        if (chain->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) continue;
        const auto* begin = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(chain);
        for (uint32_t i = 0; i < begin->attachmentCount; ++i) {
            auto found = views.find(begin->pAttachments[i]);
            imageless.push_back(found == views.end() ? nullptr : found->second);
        }
    }
    if (described != render_passes.end()) {
        const auto& list = !imageless.empty() ? imageless
            : attached != framebuffers.end() ? attached->second : std::vector<const Image*>{};
        for (size_t i = 0; i < described->second.attachments.size(); ++i) {
            Attachment a = described->second.attachments[i];
            if (i < list.size()) a.image = list[i];
            if (depth_format(a.image ? a.image->format : a.format)) pass.depth = a;
            else pass.colors.push_back(a);
        }
    }
    Recording& recording = buffer == cached_buffer && cached_recording ? *cached_recording : recordings[buffer];
    pass.dispatches_before = recording.dispatches;
    pass.copies_before = recording.copies;
    recording.dispatches = recording.copies = 0;
    recording.passes.push_back(std::move(pass));
    recording.in_pass = true;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
        VkSubpassContents contents) {
    begin_render_pass(buffer, info);
    next.CmdBeginRenderPass(buffer, info, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
        const VkSubpassBeginInfo* subpass) {
    begin_render_pass(buffer, info);
    next.CmdBeginRenderPass2(buffer, info, subpass);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2KHR(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info,
        const VkSubpassBeginInfo* subpass) {
    begin_render_pass(buffer, info);
    next.CmdBeginRenderPass2KHR(buffer, info, subpass);
}

Attachment rendering_attachment(const VkRenderingAttachmentInfo& info) {
    auto found = views.find(info.imageView);
    return {found == views.end() ? nullptr : found->second, VK_FORMAT_UNDEFINED, info.loadOp, info.storeOp};
}

void begin_rendering(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    Pass pass;
    pass.dynamic = true;
    pass.extent = info->renderArea.extent;
    std::lock_guard lock(probe_mutex);
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i)
        if (info->pColorAttachments[i].imageView) pass.colors.push_back(rendering_attachment(info->pColorAttachments[i]));
    if (info->pDepthAttachment && info->pDepthAttachment->imageView)
        pass.depth = rendering_attachment(*info->pDepthAttachment);
    else if (info->pStencilAttachment && info->pStencilAttachment->imageView)
        pass.depth = rendering_attachment(*info->pStencilAttachment);
    Recording& recording = buffer == cached_buffer && cached_recording ? *cached_recording : recordings[buffer];
    pass.dispatches_before = recording.dispatches;
    pass.copies_before = recording.copies;
    recording.dispatches = recording.copies = 0;
    recording.passes.push_back(std::move(pass));
    recording.in_pass = true;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    begin_rendering(buffer, info);
    next.CmdBeginRendering(buffer, info);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer buffer, const VkRenderingInfo* info) {
    begin_rendering(buffer, info);
    next.CmdBeginRenderingKHR(buffer, info);
}

void count_draw(VkCommandBuffer buffer) {
    Recording& recording = recording_of(buffer);
    if (!recording.passes.empty()) ++recording.passes.back().draws;
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer buffer, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
        uint32_t first_instance) {
    count_draw(buffer);
    next.CmdDraw(buffer, vertices, instances, first_vertex, first_instance);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer buffer, uint32_t indices, uint32_t instances,
        uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    count_draw(buffer);
    next.CmdDrawIndexed(buffer, indices, instances, first_index, vertex_offset, first_instance);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer buffer, VkBuffer source, VkDeviceSize offset, uint32_t count,
        uint32_t stride) {
    count_draw(buffer);
    next.CmdDrawIndirect(buffer, source, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer buffer, VkBuffer source, VkDeviceSize offset,
        uint32_t count, uint32_t stride) {
    count_draw(buffer);
    next.CmdDrawIndexedIndirect(buffer, source, offset, count, stride);
}

#define PROBE_DRAW_COUNT(name)                                                                                        \
    VKAPI_ATTR void VKAPI_CALL name(VkCommandBuffer buffer, VkBuffer source, VkDeviceSize offset, VkBuffer counts,   \
            VkDeviceSize count_offset, uint32_t max_count, uint32_t stride) {                                        \
        count_draw(buffer);                                                                                           \
        next.name(buffer, source, offset, counts, count_offset, max_count, stride);                                  \
    }
PROBE_DRAW_COUNT(CmdDrawIndirectCount)
PROBE_DRAW_COUNT(CmdDrawIndexedIndirectCount)
PROBE_DRAW_COUNT(CmdDrawIndirectCountKHR)
PROBE_DRAW_COUNT(CmdDrawIndexedIndirectCountKHR)
#undef PROBE_DRAW_COUNT

VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer buffer, uint32_t x, uint32_t y, uint32_t z) {
    ++recording_of(buffer).dispatches;
    next.CmdDispatch(buffer, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer buffer, VkBuffer source, VkDeviceSize offset) {
    ++recording_of(buffer).dispatches;
    next.CmdDispatchIndirect(buffer, source, offset);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer buffer, VkImage source, VkImageLayout source_layout,
        VkImage target, VkImageLayout target_layout, uint32_t count, const VkImageCopy* regions) {
    ++recording_of(buffer).copies;
    next.CmdCopyImage(buffer, source, source_layout, target, target_layout, count, regions);
}

VKAPI_ATTR void VKAPI_CALL CmdBlitImage(VkCommandBuffer buffer, VkImage source, VkImageLayout source_layout,
        VkImage target, VkImageLayout target_layout, uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    ++recording_of(buffer).copies;
    next.CmdBlitImage(buffer, source, source_layout, target, target_layout, count, regions, filter);
}

VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer buffer, uint32_t count, const VkCommandBuffer* secondaries) {
    {
        // The secondaries' draws count in the primary's pass.
        std::lock_guard lock(probe_mutex);
        Recording& recording = recordings[buffer];
        for (uint32_t i = 0; i < count; ++i) {
            auto found = recordings.find(secondaries[i]);
            if (found == recordings.end() || recording.passes.empty()) continue;
            for (const Pass& pass : found->second.passes) recording.passes.back().draws += pass.draws;
        }
    }
    next.CmdExecuteCommands(buffer, count, secondaries);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    {
        std::lock_guard lock(probe_mutex);
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j) submitted(queue, submits[i].pCommandBuffers[j]);
    }
    return next.QueueSubmit(queue, count, submits, fence);
}

void submitted2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits) {
    std::lock_guard lock(probe_mutex);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j)
            submitted(queue, submits[i].pCommandBufferInfos[j].commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {
    submitted2(queue, count, submits);
    return next.QueueSubmit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
        VkFence fence) {
    submitted2(queue, count, submits);
    return next.QueueSubmit2KHR(queue, count, submits, fence);
}

bool read_enabled() {
    wchar_t path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&read_enabled), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring file(path);
    file.erase(file.find_last_of(L'\\') + 1);
    file += L"render-probe.on";
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

bool enabled() {
    // Not with the game depth: both would take the same functions.
    static const bool on = is_game_process() && read_enabled() && !depth::enabled();
    return on;
}

void start(VkDevice device, PFN_vkGetDeviceProcAddr next_gdpa) {
    if (!enabled()) return;
    if (probed_device) {
        log_line("render probe: another device %p, not probed", static_cast<void*>(device));
        return;
    }
    probed_device = device;
#define PROBE_LOAD(name, type) next.name = reinterpret_cast<PFN_##type>(next_gdpa(device, #type));
    PROBE_FUNCTIONS(PROBE_LOAD)
#undef PROBE_LOAD
    log_line("render probe: on (render-probe.on), device %p; frames %d, then every %d, %d in all",
        static_cast<void*>(device), kFirstFrame, kEveryFrames, kFrames);
}

PFN_vkVoidFunction function(const char* requested) {
    if (!enabled()) return nullptr;
    // Only once the next layer's is known: never a function calling nothing.
#define PROBE_INTERCEPT(name, type) \
    if (std::strcmp(requested, #type) == 0) return next.name ? reinterpret_cast<PFN_vkVoidFunction>(&name) : nullptr;
    PROBE_FUNCTIONS(PROBE_INTERCEPT)
#undef PROBE_INTERCEPT
    return nullptr;
}

void note_swapchain(const std::vector<VkImage>& list, VkFormat format, VkExtent2D extent) {
    if (!enabled()) return;
    std::lock_guard lock(probe_mutex);
    for (VkImage image : list) {
        Image& entry = images[image];
        entry = {next_image_id++, format, {extent.width, extent.height, 1}, 0, VK_SAMPLE_COUNT_1_BIT, true};
    }
}

void presented() {
    if (!enabled()) return;
    std::lock_guard lock(probe_mutex);
    ++frame_number;
    const bool due = frames_logged < kFrames &&
        (frame_number == kFirstFrame || (frame_number > kFirstFrame && (frame_number - kFirstFrame) % kEveryFrames == 0));
    if (due) {
        ++frames_logged;
        log_frame();
    }
    frame.clear();
}

}  // namespace overlay::probe
