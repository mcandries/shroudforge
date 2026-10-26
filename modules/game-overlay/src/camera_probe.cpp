// Diagnostic: where the game hands the camera of each frame to the GPU, to
// draw world items with the camera of the image itself rather than the one
// of the player's input (about 12 ms behind, 26/10/2026).
//
// On only when `camera_probe.txt` sits beside the layer DLL when the game
// creates its device. It then watches the buffers the game maps for the CPU
// and the data it records with vkCmdUpdateBuffer and vkCmdPushConstants, and
// every 5 s logs where the current camera position (three float32 meters,
// as in RenderForwardConstants.cameraWorldPosition) appears.

#include "camera_probe.h"

#include "local_camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace overlay {
namespace {

PFN_vkMapMemory next_map = nullptr;
PFN_vkUnmapMemory next_unmap = nullptr;
PFN_vkAllocateMemory next_allocate = nullptr;
PFN_vkFreeMemory next_free = nullptr;
PFN_vkCmdUpdateBuffer next_update = nullptr;
PFN_vkCmdPushConstants next_push = nullptr;

struct Mapped {
    char* data = nullptr;
    VkDeviceSize size = 0;
};

std::mutex probe_mutex;
std::unordered_map<VkDeviceMemory, VkDeviceSize> allocation_sizes;
std::unordered_map<VkDeviceMemory, Mapped> mapped;
// Recorded data, over the current 5 s: size -> count, and camera hits.
std::map<uint32_t, uint32_t> update_sizes, push_sizes;
uint32_t update_hits = 0, push_hits = 0;
uint32_t update_hit_size = 0, update_hit_offset = 0, push_hit_size = 0, push_hit_offset = 0;
double camera[3]{};
bool camera_known = false;

bool enabled_flag() {
    wchar_t path[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&enabled_flag), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"camera_probe.txt");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// The first offset (from `from`) of three float32 within `tolerance` of the
// camera position, or -1. Plain loop, no C++ objects: called under __try.
long long find_camera(const char* data, size_t size, size_t from, const float target[3], float tolerance) {
    for (size_t at = from; at + 12 <= size; at += 4) {
        float x;
        std::memcpy(&x, data + at, 4);
        if (std::fabs(x - target[0]) > tolerance) continue;
        float y, z;
        std::memcpy(&y, data + at + 4, 4);
        std::memcpy(&z, data + at + 8, 4);
        if (std::fabs(y - target[1]) <= tolerance && std::fabs(z - target[2]) <= tolerance)
            return static_cast<long long>(at);
    }
    return -1;
}

long long guarded_find(const char* data, size_t size, size_t from, const float target[3], float tolerance) {
    __try {
        return find_camera(data, size, from, target, tolerance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool guarded_copy(void* to, const void* from, size_t size) {
    __try {
        std::memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void target(float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(camera[i]);
}

void check_recorded(const void* data, uint32_t size, uint32_t& hits, uint32_t& hit_size, uint32_t& hit_offset) {
    if (!camera_known || !data || size < 12) return;
    float t[3];
    target(t);
    const long long at = guarded_find(static_cast<const char*>(data), size, 0, t, 0.5f);
    if (at >= 0) {
        ++hits;
        hit_size = size;
        hit_offset = static_cast<uint32_t>(at);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL ProbeAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
        const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    VkResult result = next_allocate(device, info, allocator, memory);
    if (result == VK_SUCCESS) {
        std::lock_guard lock(probe_mutex);
        allocation_sizes[*memory] = info->allocationSize;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL ProbeFreeMemory(VkDevice device, VkDeviceMemory memory,
        const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard lock(probe_mutex);
        allocation_sizes.erase(memory);
        mapped.erase(memory);
    }
    next_free(device, memory, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL ProbeMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
        VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
    VkResult result = next_map(device, memory, offset, size, flags, data);
    if (result == VK_SUCCESS && data && *data) {
        std::lock_guard lock(probe_mutex);
        VkDeviceSize length = size;
        if (size == VK_WHOLE_SIZE) {
            auto found = allocation_sizes.find(memory);
            length = found != allocation_sizes.end() && found->second > offset ? found->second - offset : 0;
        }
        mapped[memory] = Mapped{static_cast<char*>(*data), length};
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL ProbeUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    {
        std::lock_guard lock(probe_mutex);
        mapped.erase(memory);
    }
    next_unmap(device, memory);
}

VKAPI_ATTR void VKAPI_CALL ProbeCmdUpdateBuffer(VkCommandBuffer buffer, VkBuffer target_buffer, VkDeviceSize offset,
        VkDeviceSize size, const void* data) {
    {
        std::lock_guard lock(probe_mutex);
        ++update_sizes[static_cast<uint32_t>(size)];
        check_recorded(data, static_cast<uint32_t>(size), update_hits, update_hit_size, update_hit_offset);
    }
    next_update(buffer, target_buffer, offset, size, data);
}

VKAPI_ATTR void VKAPI_CALL ProbeCmdPushConstants(VkCommandBuffer buffer, VkPipelineLayout layout,
        VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void* values) {
    {
        std::lock_guard lock(probe_mutex);
        ++push_sizes[size];
        check_recorded(values, size, push_hits, push_hit_size, push_hit_offset);
    }
    next_push(buffer, layout, stages, offset, size, values);
}

std::string sizes_text(const std::map<uint32_t, uint32_t>& sizes) {
    std::vector<std::pair<uint32_t, uint32_t>> list(sizes.begin(), sizes.end());
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t i = 0; i < list.size() && i < 8; ++i)
        text += std::to_string(list[i].first) + " B x" + std::to_string(list[i].second) + " ";
    return text.empty() ? "none" : text;
}

double seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

}  // namespace

bool camera_probe_enabled() {
    static const bool enabled = enabled_flag();
    return enabled;
}

void camera_probe_attach(PFN_vkGetDeviceProcAddr next_gdpa, VkDevice device) {
    if (!camera_probe_enabled()) return;
    next_map = reinterpret_cast<PFN_vkMapMemory>(next_gdpa(device, "vkMapMemory"));
    next_unmap = reinterpret_cast<PFN_vkUnmapMemory>(next_gdpa(device, "vkUnmapMemory"));
    next_allocate = reinterpret_cast<PFN_vkAllocateMemory>(next_gdpa(device, "vkAllocateMemory"));
    next_free = reinterpret_cast<PFN_vkFreeMemory>(next_gdpa(device, "vkFreeMemory"));
    next_update = reinterpret_cast<PFN_vkCmdUpdateBuffer>(next_gdpa(device, "vkCmdUpdateBuffer"));
    next_push = reinterpret_cast<PFN_vkCmdPushConstants>(next_gdpa(device, "vkCmdPushConstants"));
    log_line("camera probe on: watching mapped memory, vkCmdUpdateBuffer and vkCmdPushConstants");
}

PFN_vkVoidFunction camera_probe_function(const char* requested) {
    if (!camera_probe_enabled() || !next_map) return nullptr;
#define PROBE(name) \
    if (std::strcmp(requested, "vk" #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Probe##name)
    PROBE(MapMemory);
    PROBE(UnmapMemory);
    PROBE(AllocateMemory);
    PROBE(FreeMemory);
    PROBE(CmdUpdateBuffer);
    PROBE(CmdPushConstants);
#undef PROBE
    return nullptr;
}

void camera_probe_frame() {
    if (!camera_probe_enabled() || !next_map) return;
    LocalCamera now;
    const bool known = last_local_camera(now);
    static double next_report = 0.0;
    const double time = seconds();
    std::lock_guard lock(probe_mutex);
    camera_known = known;
    if (known)
        for (int i = 0; i < 3; ++i) camera[i] = now.position[i];
    if (time < next_report) return;
    next_report = time + 5.0;
    if (!known) {
        log_line("camera probe: no camera read yet (world item needed, e.g. the camera diagnostic)");
        return;
    }
    VkDeviceSize total = 0;
    for (auto& [memory, region] : mapped) total += region.size;
    log_line("camera probe: camera at %.3f, %.3f, %.3f m; %zu mapped regions, %.1f MB; vkCmdUpdateBuffer %s, "
             "camera found %u times (last in %u B at %u); vkCmdPushConstants %s, camera found %u times (last in %u B at %u)",
        camera[0], camera[1], camera[2], mapped.size(), total / 1048576.0, sizes_text(update_sizes).c_str(), update_hits,
        update_hit_size, update_hit_offset, sizes_text(push_sizes).c_str(), push_hits, push_hit_size, push_hit_offset);
    update_sizes.clear();
    push_sizes.clear();
    update_hits = push_hits = 0;
    // Mapped memory: every place the camera position appears, at most 16,
    // with the 16 floats before it (a matrix, if this is a constant block).
    float t[3];
    target(t);
    const double started = seconds();
    int found = 0;
    for (auto& [memory, region] : mapped) {
        size_t from = 0;
        while (found < 16) {
            const long long at = guarded_find(region.data, static_cast<size_t>(region.size), from, t, 0.5f);
            if (at < 0) break;
            float hit[3]{}, before[16]{};
            guarded_copy(hit, region.data + at, 12);
            const bool has_before = at >= 64 && guarded_copy(before, region.data + at - 64, 64);
            log_line("camera probe: mapped %p (%.1f MB) at +%lld (mod 256: %lld): %.3f %.3f %.3f%s",
                static_cast<void*>(region.data), region.size / 1048576.0, at, at % 256, hit[0], hit[1], hit[2],
                has_before ? "" : " (no floats before)");
            if (has_before)
                log_line("camera probe:   before: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                         "%.3f %.3f %.3f %.3f",
                    before[0], before[1], before[2], before[3], before[4], before[5], before[6], before[7], before[8],
                    before[9], before[10], before[11], before[12], before[13], before[14], before[15]);
            ++found;
            from = static_cast<size_t>(at) + 4;
        }
    }
    log_line("camera probe: mapped memory scanned in %.1f ms, %d places found", (seconds() - started) * 1000.0, found);
}

}  // namespace overlay
