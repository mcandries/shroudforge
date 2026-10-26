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
#include <thread>
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


// The render camera. The frame constants the game hands to the GPU hold, for
// each frame and pass, a view-projection matrix (16 float32, row-major, world
// translation; row 2 near zero: reversed infinite depth; row 3: the gaze)
// followed by the eye (3 float32 meters): seen on 26/10/2026 in a 4 MB mapped
// ring, about 27 blocks and 880 KB per frame. That memory reads at about
// 7 MB/s from the CPU (130 to 190 ms per frame to follow it): it is not read.
// Instead, a thread looks once through the process's ordinary memory for the
// same blocks (the copies the game prepares), and each present only reads
// the 76 bytes at the places found.
namespace {

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

// Within CLOSE_METERS of the input camera: the blocks of the player's view.
constexpr float CLOSE_METERS = 3.0f;
bool close_to(const float* f, const float around[3]) {
    return std::fabs(f[16] - around[0]) <= CLOSE_METERS && std::fabs(f[17] - around[1]) <= CLOSE_METERS &&
        std::fabs(f[18] - around[2]) <= CLOSE_METERS;
}

// Camera blocks in [data, data + size), 4-byte steps, at most `room`;
// returns how many, or -1 when the memory went away. Plain code.
int scan_linear(const char* data, size_t size, const float around[3], const char** out, int room) {
    int count = 0;
    __try {
        for (size_t at = 0; at + 76 <= size && count < room; at += 4) {
            const float* f = reinterpret_cast<const float*>(data + at);
            if (std::fabs(f[8]) > 0.02f || !(f[11] > 0.0f && f[11] < 1.0f)) continue;
            if (!close_to(f, around) || !camera_block(f)) continue;
            out[count++] = data + at;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return count;
}

// A place found, and what it held at the last present.
struct Place {
    const char* address = nullptr;
    float values[19]{};
    bool valid = false;
    uint64_t changed = 0;  // Present count when its content last changed.
    uint64_t changes = 0;  // Over the current 5 s.
};

// In the DLL's own data (image memory, not searched): the search must not
// find the overlay's copies of the blocks.
constexpr int MAX_PLACES = 64;
struct Search {
    Place places[MAX_PLACES];
    int count = 0;
    float scratch[19]{};
    uint64_t frame = 0;
    double wanted_until = 0.0, next_search = 0.0, last_valid = 0.0, longest = 0.0;
    bool running = false;
    uint64_t frames = 0;  // Over the current 5 s.
} search;

// The search, on its own thread: every committed, private, writable region
// of the process but the GPU memory the game maps.
void search_thread(float around0, float around1, float around2,
    std::vector<std::pair<const char*, size_t>> gpu) {
    const double started = seconds();
    const float around[3] = {around0, around1, around2};
    size_t scanned = 0, queried = 0, regions = 0, uncached = 0, slow = 0;
    constexpr size_t CHUNK = 1u << 20;
    constexpr double SLOW_MB_PER_S = 200.0;
    double query_time = 0.0, query_longest = 0.0, scan_time = 0.0, next_report = started + 5.0;
    const char* at = nullptr;
    MEMORY_BASIC_INFORMATION info{};
    int found = 0;
    while (found < MAX_PLACES) {
        const double asked = seconds();
        const bool answered = VirtualQuery(at, &info, sizeof(info)) == sizeof(info);
        const double took = seconds() - asked;
        query_time += took;
        query_longest = std::max(query_longest, took);
        ++queried;
        if (!answered) break;
        const char* base = static_cast<const char*>(info.BaseAddress);
        const size_t size = info.RegionSize;
        at = base + size;
        if (seconds() >= next_report) {
            next_report = seconds() + 5.0;
            log_line("render camera: searching for %.0f s: %zu regions asked (%.0f ms in VirtualQuery, longest %.1f ms), "
                     "%zu read (%.0f MB, %.0f ms), %zu uncached and %zu slow left, %d blocks found, at %p",
                seconds() - started, queried, query_time * 1000.0, query_longest * 1000.0, regions,
                scanned / 1048576.0, scan_time * 1000.0, uncached, slow, found, static_cast<const void*>(base));
        }
        if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE) continue;
        if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) continue;
        if ((info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0) continue;
        // Uncached or write-combined: memory shared with the GPU, read at
        // about 19 MB/s (128 MB regions of the driver, 26/10/2026).
        if ((info.Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) != 0) {
            ++uncached;
            continue;
        }
        if (size > (1ull << 30)) continue;
        bool is_gpu = false;
        for (const auto& [data, length] : gpu) is_gpu = is_gpu || (base < data + length && data < base + size);
        if (is_gpu) continue;
        // Read 1 MB at a time (the chunks overlap by a block less 4 bytes);
        // a region read slower than SLOW_MB_PER_S is left: GPU memory the
        // flags do not tell.
        const char* hits[MAX_PLACES];
        int count = 0;
        for (size_t offset = 0; offset < size && found + count < MAX_PLACES; offset += CHUNK) {
            const size_t length = std::min(CHUNK + 72, size - offset);
            const double read_at = seconds();
            const int got = scan_linear(base + offset, length, around, hits + count, MAX_PLACES - found - count);
            const double took = seconds() - read_at;
            scan_time += took;
            scanned += std::min(CHUNK, size - offset);
            if (got > 0) count += got;
            if (got < 0 || took * SLOW_MB_PER_S > static_cast<double>(length) / 1048576.0) {
                if (got >= 0) ++slow;
                break;
            }
        }
        ++regions;
        if (count <= 0) continue;
        // Followed at once, while the search goes on.
        std::lock_guard lock(probe_mutex);
        for (int i = 0; i < count && search.count < MAX_PLACES; ++i) {
            search.places[search.count] = Place{};
            search.places[search.count].address = hits[i];
            ++search.count;
            log_line("render camera:   block at %p (region %p, %.1f MB)", static_cast<const void*>(hits[i]),
                static_cast<const void*>(base), size / 1048576.0);
        }
        found += count;
        search.last_valid = seconds();
    }
    std::lock_guard lock(probe_mutex);
    search.running = false;
    log_line("render camera: search done in %.1f s: %zu regions asked (%.0f ms in VirtualQuery, longest %.1f ms), %zu "
             "read (%.0f MB, %.0f ms), %zu uncached and %zu slow left, %d blocks near the camera",
        seconds() - started, queried, query_time * 1000.0, query_longest * 1000.0, regions, scanned / 1048576.0,
        scan_time * 1000.0, uncached, slow, found);
}

void start_search(double now, const float around[3]) {
    if (search.running || now < search.next_search) return;
    search.running = true;
    search.next_search = now + 10.0;
    search.count = 0;
    std::vector<std::pair<const char*, size_t>> gpu;
    for (auto& [memory, region] : mapped) gpu.emplace_back(region.data, static_cast<size_t>(region.size));
    std::thread(search_thread, around[0], around[1], around[2], std::move(gpu)).detach();
}

// Reads the places found (76 bytes each) and notes which changed.
void read_places(const float around[3]) {
    ++search.frame;
    ++search.frames;
    float* values = search.scratch;
    for (int i = 0; i < search.count; ++i) {
        Place& place = search.places[i];
        if (!guarded_copy(values, place.address, sizeof(search.scratch))) {
            place.valid = false;
            continue;
        }
        if (std::memcmp(values, place.values, sizeof(place.values)) != 0) {
            std::memcpy(place.values, values, sizeof(place.values));
            place.changed = search.frame;
            ++place.changes;
        }
        place.valid = camera_block(values) && close_to(values, around);
    }
}

}  // namespace

bool render_camera(int age, RenderCamera& out) {
    std::lock_guard lock(probe_mutex);
    search.wanted_until = seconds() + 1.0;
    // Valid places that changed in the last 3 presents, most recent first;
    // `age` 0 is the most recent change, 1 the one before.
    std::vector<const Place*> live;
    for (int i = 0; i < search.count; ++i)
        if (search.places[i].valid && search.frame - search.places[i].changed <= 3) live.push_back(&search.places[i]);
    std::sort(live.begin(), live.end(), [](const Place* a, const Place* b) { return a->changed > b->changed; });
    if (age < 0 || age >= static_cast<int>(live.size())) return false;
    const Place& chosen = *live[age];
    for (int i = 0; i < 16; ++i) out.matrix[i] = chosen.values[i];
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
    LocalCamera input;
    const bool known = last_local_camera(input);
    std::lock_guard lock(probe_mutex);
    // Only while a world item asks for it, near the input camera.
    if (now > search.wanted_until || !known) return;
    const float around[3] = {static_cast<float>(input.position[0]), static_cast<float>(input.position[1]),
        static_cast<float>(input.position[2])};
    read_places(around);
    bool any_valid = false;
    for (int i = 0; i < search.count; ++i) any_valid = any_valid || search.places[i].valid;
    if (any_valid) search.last_valid = now;
    if (!search.running && (search.count == 0 || now - search.last_valid > 2.0)) start_search(now, around);
    search.longest = std::max(search.longest, seconds() - now);
    static double next_report = 0.0;
    if (now < next_report) return;
    next_report = now + 5.0;
    int valid = 0, fresh = 0;
    for (int i = 0; i < search.count; ++i) {
        const Place& place = search.places[i];
        valid += place.valid ? 1 : 0;
        fresh += place.valid && search.frame - place.changed <= 3 ? 1 : 0;
    }
    log_line("render camera: %s; %zu places, %d valid, %d changed in the last 3 frames; longest %.3f ms per present",
        search.running ? "searching" : "following", static_cast<size_t>(search.count), valid, fresh,
        search.longest * 1000.0);
    // Per place: how often it changed over the 5 s, and how it differs from
    // the input camera (eye distance, gaze angle).
    const double frames = search.frames ? static_cast<double>(search.frames) : 1.0;
    int shown = 0;
    for (int i = 0; i < search.count; ++i) {
        Place& place = search.places[i];
        if (shown < 12 && place.valid) {
            const float* f = place.values;
            const double d[3] = {f[16] - input.position[0], f[17] - input.position[1], f[18] - input.position[2]};
            log_line("render camera:   %p: changed %.2f per frame, eye %.3f m from the input camera's, gaze %.3f %.3f %.3f",
                static_cast<const void*>(place.address), place.changes / frames,
                std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), f[12], f[13], f[14]);
            ++shown;
        }
        place.changes = 0;
    }
    search.frames = 0;
    search.longest = 0.0;
}

}  // namespace overlay
