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
#include <vector>

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
    next_map = reinterpret_cast<PFN_vkMapMemory>(next_gdpa(device, "vkMapMemory"));
    next_unmap = reinterpret_cast<PFN_vkUnmapMemory>(next_gdpa(device, "vkUnmapMemory"));
    next_allocate = reinterpret_cast<PFN_vkAllocateMemory>(next_gdpa(device, "vkAllocateMemory"));
    next_free = reinterpret_cast<PFN_vkFreeMemory>(next_gdpa(device, "vkFreeMemory"));
    next_update = reinterpret_cast<PFN_vkCmdUpdateBuffer>(next_gdpa(device, "vkCmdUpdateBuffer"));
    next_push = reinterpret_cast<PFN_vkCmdPushConstants>(next_gdpa(device, "vkCmdPushConstants"));
    if (camera_probe_enabled())
        log_line("camera probe on: watching mapped memory, vkCmdUpdateBuffer and vkCmdPushConstants");
}

PFN_vkVoidFunction camera_probe_function(const char* requested) {
    if (!next_map) return nullptr;
#define PROBE(name) \
    if (std::strcmp(requested, "vk" #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Probe##name)
    // Mapped memory, always: the render camera lives there (render_camera).
    PROBE(MapMemory);
    PROBE(UnmapMemory);
    PROBE(AllocateMemory);
    PROBE(FreeMemory);
    // Recorded data: called about 400,000 times a second, only for the probe.
    if (!camera_probe_enabled()) return nullptr;
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


// The render camera: the frame constants the game writes into a mapped ring
// buffer hold, for each frame, a view-projection matrix (16 float32,
// row-major, world translation; row 2 near zero: reversed infinite depth;
// row 3: the gaze) followed by the eye (3 float32 meters). Seen in game on
// 26/10/2026 (4 MB ring, blocks 16-byte aligned).
namespace {

constexpr int MAX_BLOCKS = 32;
struct Found {
    size_t offset = 0;
    float values[19]{};
};

// A camera block at `f` (matrix then eye)? Plain code, read under __try.
bool camera_block(const float* f) {
    for (int i = 0; i < 19; ++i)
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > 1e7f) return false;
    if (std::fabs(f[8]) > 0.02f || std::fabs(f[9]) > 0.02f || std::fabs(f[10]) > 0.02f) return false;
    if (!(f[11] > 0.0f && f[11] < 1.0f)) return false;
    const float n3 = std::sqrt(f[12] * f[12] + f[13] * f[13] + f[14] * f[14]);
    if (n3 < 0.98f || n3 > 1.02f) return false;
    const float n0 = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float n1 = std::sqrt(f[4] * f[4] + f[5] * f[5] + f[6] * f[6]);
    if (n0 < 0.2f || n0 > 5.0f || n1 < 0.2f || n1 > 5.0f) return false;
    const float* e = f + 16;
    if (e[0] == 0.0f && e[1] == 0.0f && e[2] == 0.0f) return false;
    const float w = f[15] + f[12] * e[0] + f[13] * e[1] + f[14] * e[2];
    return std::fabs(w) < 1.0f;
}

// Camera blocks in `size` bytes from `data`, starting `from` and wrapping,
// over `span` bytes. Returns how many (at most MAX_BLOCKS), -1 on a fault.
int scan_blocks(const char* data, size_t size, size_t from, size_t span, Found* out) {
    int count = 0;
    __try {
        for (size_t step = 0; step < span && count < MAX_BLOCKS; step += 16) {
            const size_t at = (from + step) % size;
            if (at + 76 > size) continue;
            const float* f = reinterpret_cast<const float*>(data + at);
            if (std::fabs(f[8]) > 0.02f || !(f[11] > 0.0f && f[11] < 1.0f)) continue;
            if (!camera_block(f)) continue;
            out[count].offset = at;
            std::memcpy(out[count].values, f, sizeof(out[count].values));
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return count;
}

struct Known {
    size_t offset = 0;
    float values[19]{};
    uint64_t born = 0;
};

struct Ring {
    const char* data = nullptr;
    size_t size = 0;
    size_t cursor = 0;
    uint64_t frame = 0;
    double last_seen = 0.0, next_search = 0.0, wanted_until = 0.0;
    std::vector<Known> known;
    // Over the current 5 s, for the probe's log.
    uint64_t frames = 0, found = 0, born = 0, advance = 0;
} ring;

// Looks for the ring among the mapped regions (1 to 64 MB), once a second.
void find_ring(double now) {
    if (now < ring.next_search) return;
    ring.next_search = now + 1.0;
    Found blocks[MAX_BLOCKS];
    for (auto& [memory, region] : mapped) {
        if (region.size < (1u << 20) || region.size > (64u << 20)) continue;
        const int count =
            scan_blocks(region.data, static_cast<size_t>(region.size), 0, static_cast<size_t>(region.size), blocks);
        if (count > 0) {
            ring.data = region.data;
            ring.size = static_cast<size_t>(region.size);
            ring.cursor = blocks[count - 1].offset;
            ring.known.clear();
            ring.last_seen = now;
            log_line("render camera: ring found, %.1f MB at %p, %d camera blocks", region.size / 1048576.0,
                static_cast<const void*>(region.data), count);
            return;
        }
    }
}

// Notes the blocks written since the last frame; the newest moves the cursor.
void track_ring(double now) {
    if (!ring.data) {
        find_ring(now);
        return;
    }
    bool still_mapped = false;
    for (auto& [memory, region] : mapped) still_mapped = still_mapped || region.data == ring.data;
    if (!still_mapped || now - ring.last_seen > 2.0) {
        log_line("render camera: ring lost");
        const double wanted = ring.wanted_until;
        ring = Ring{};
        ring.wanted_until = wanted;
        return;
    }
    Found blocks[MAX_BLOCKS];
    const size_t back = 128u << 10;
    const int count =
        scan_blocks(ring.data, ring.size, (ring.cursor + ring.size - back) % ring.size, back + (1u << 20), blocks);
    if (count < 0) return;
    ++ring.frame;
    ++ring.frames;
    ring.found += count;
    std::vector<Known> next;
    size_t newest = ring.cursor;
    uint64_t newest_born = 0;
    for (int i = 0; i < count; ++i) {
        Known block;
        block.offset = blocks[i].offset;
        std::memcpy(block.values, blocks[i].values, sizeof(block.values));
        block.born = ring.frame;
        for (const Known& old : ring.known)
            if (old.offset == block.offset && std::memcmp(old.values, block.values, sizeof(block.values)) == 0)
                block.born = old.born;
        if (block.born == ring.frame) ++ring.born;
        if (block.born >= newest_born) {
            newest_born = block.born;
            newest = block.offset;
        }
        next.push_back(block);
    }
    if (count > 0) {
        ring.advance += (newest + ring.size - ring.cursor) % ring.size;
        ring.cursor = newest;
        ring.last_seen = now;
    }
    ring.known = std::move(next);
}

}  // namespace

bool render_camera(int age, RenderCamera& out) {
    std::lock_guard lock(probe_mutex);
    ring.wanted_until = seconds() + 1.0;
    if (ring.known.empty()) return false;
    // Births, newest first; `age` 0 is the newest birth, 1 the one before.
    std::vector<uint64_t> births;
    for (const Known& block : ring.known) births.push_back(block.born);
    std::sort(births.begin(), births.end(), std::greater<>());
    births.erase(std::unique(births.begin(), births.end()), births.end());
    if (age < 0 || age >= static_cast<int>(births.size())) return false;
    const auto ahead = [](const Known& block) { return (block.offset + ring.size - ring.cursor) % ring.size; };
    const Known* chosen = nullptr;
    for (const Known& block : ring.known)
        if (block.born == births[age] && (!chosen || ahead(block) > ahead(*chosen))) chosen = &block;
    if (!chosen) return false;
    for (int i = 0; i < 16; ++i) out.matrix[i] = chosen->values[i];
    // The eye: where rows 0, 1 and 3 all vanish (the projection centre).
    const double* m = out.matrix;
    const double a[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[12], m[13], m[14]}};
    const double b[3] = {-m[3], -m[7], -m[15]};
    const auto determinant = [](const double t[3][3]) {
        return t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
            t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
    };
    const double det = determinant(a);
    if (std::fabs(det) < 1e-9) return false;
    for (int c = 0; c < 3; ++c) {
        double t[3][3];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) t[r][k] = k == c ? b[r] : a[r][k];
        out.eye[c] = determinant(t) / det;
    }
    return true;
}

void render_camera_frame() {
    const double now = seconds();
    std::lock_guard lock(probe_mutex);
    if (now > ring.wanted_until && !camera_probe_enabled()) return;
    track_ring(now);
    static double next_report = 0.0;
    if (!camera_probe_enabled() || now < next_report || !ring.data) return;
    next_report = now + 5.0;
    const double frames = ring.frames ? static_cast<double>(ring.frames) : 1.0;
    log_line("render camera: %llu frames, %.1f camera blocks seen and %.2f new per frame, cursor %.1f KB further "
             "per frame",
        static_cast<unsigned long long>(ring.frames), ring.found / frames, ring.born / frames,
        ring.advance / frames / 1024.0);
    ring.frames = ring.found = ring.born = ring.advance = 0;
}

}  // namespace overlay
