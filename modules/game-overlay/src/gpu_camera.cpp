// The camera the game renders its scene with, read where it hands it to the
// GPU (with game-depth.on). The camera read from the runtime is the one of the
// player's inputs: about a frame behind the image (KB, 26/10/2026), and the
// world items drawn with it slide against the game's depth. The frame
// constants hold, per pass, a camera block: a view-projection matrix (16
// float, row-major, world translation; row 2 is 0, 0, 0, near: reversed
// infinite depth; row 3 the unit gaze) then the eye (3 float, meters), in a
// mapped ring that reads at only a few MB/s from the CPU: it cannot be
// searched at every frame (render camera probe of 26/10).
//
// So the layer follows what the scene's pass is given: the descriptor sets
// bound for graphics up to its end (their uniform and storage buffers, with
// their dynamic offsets) and the push constants inside it (8-byte values
// that fall in a buffer's device address range). Now and then, until a block
// is found, it reads up to 4 KB of each of them (64 KB at most) and looks
// for a camera block near the input camera; once found, only those 76 bytes
// are read again, from the same set, binding and offset (or push constant),
// at each submit of the scene.

#include "overlay.h"
#include "gpu_camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace overlay::depth::gpu {

namespace {

struct BufferInfo {
    VkDeviceSize size = 0;
    VkDeviceMemory memory{};
    VkDeviceSize memory_offset = 0;
    VkDeviceAddress address = 0;
};

struct Mapping {
    char* data = nullptr;
    VkDeviceSize offset = 0, size = 0;
};

struct Entry {
    uint32_t binding = 0, element = 0;
    VkBuffer buffer{};
    VkDeviceSize offset = 0, range = 0;
    bool dynamic = false;
};

struct Bind {
    uint32_t first = 0;
    std::vector<VkDescriptorSet> sets;
    std::vector<uint32_t> dynamic;
};

struct Push {
    uint32_t offset = 0, size = 0;
    unsigned char data[128]{};
};

struct Recording {
    std::vector<Bind> binds;
    std::vector<Push> pushes;
    bool in_scene = false, scene = false;
    size_t binds_at_scene_end = 0;
};

// Where the camera block was found: a set index, binding and element of the
// graphics state at the scene's end, or a push constant's 8 bytes; and the
// block's offset in what they point at.
struct Place {
    bool push = false;
    uint32_t set = 0, binding = 0, element = 0;
    uint32_t push_index = 0, push_byte = 0;
    VkDeviceSize inner = 0;
};

struct Region {
    VkBuffer buffer{};
    VkDeviceSize offset = 0, size = 0;
    Place place;
};

constexpr size_t kMaxBinds = 2048;
constexpr size_t kMaxPushes = 64;
constexpr VkDeviceSize kSearchBytes = 4096;        // per region
constexpr VkDeviceSize kSearchBudget = 64 * 1024;  // per search
constexpr ULONGLONG kSearchEvery = 2000;
constexpr float kCloseMeters = 3.0f;

std::mutex mutex;
std::unordered_map<VkBuffer, BufferInfo> buffers;
std::map<VkDeviceAddress, VkBuffer> addresses;
std::unordered_map<VkDeviceMemory, Mapping> mappings;
std::unordered_map<VkDescriptorSet, std::vector<Entry>> set_entries;
std::unordered_map<VkCommandBuffer, Recording> recordings;

bool found = false;
Place place;
int misses = 0;
ULONGLONG last_search = 0;
// Over the current 5 s, for the log.
ULONGLONG last_report = 0;
int follows = 0, follow_misses = 0, searches = 0;
double search_ms = 0.0;
size_t last_regions = 0;

thread_local VkCommandBuffer cached_buffer{};
thread_local Recording* cached{};

Recording& recording_of(VkCommandBuffer buffer) {
    if (buffer == cached_buffer && cached) return *cached;
    std::lock_guard lock(mutex);
    cached_buffer = buffer;
    cached = &recordings[buffer];
    return *cached;
}

// The scene's light, searched in every region bound in a command buffer that
// holds the scene's pass (the deferred lighting comes after the scene's
// pass): a keen::RenderForwardConstants (matrix at +48, eye at +256, the
// sun's direction at +320, its color at +336, the light factors at +352) or
// a keen::DeferredLightingConstants (light factors at +0, matrix at +112,
// eye at +240, sun at +1424 and +1440), told by its reversed infinite
// matrix, its eye near the input camera's and a unit direction.
struct LightLayout {
    const char* name;
    uint32_t size, matrix, eye, direction, color, factors;
};
constexpr LightLayout kLightLayouts[2] = {
    {"RenderForwardConstants", 384, 48, 256, 320, 336, 352},
    {"DeferredLightingConstants", 1712, 112, 240, 1424, 1440, 0},
};
constexpr VkDeviceSize kLightSearchBytes = 8192;        // per region
constexpr VkDeviceSize kLightSearchBudget = 256 * 1024;  // per search
bool light_found = false;
int light_layout = 0;
Place light_place{};
uint32_t light_misses = 0;
ULONGLONG last_light_search = 0, last_light_report = 0, last_light_read = 0;
Light last_light;
int light_searches = 0;

// A camera block at `f` (matrix then eye), near the input camera's eye.
bool camera_block(const float* f, const double around[3]) {
    for (int i = 0; i < 19; ++i)
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > 1e7f) return false;
    if (std::fabs(f[8]) > 0.02f || std::fabs(f[9]) > 0.02f || std::fabs(f[10]) > 0.02f) return false;
    if (!(f[11] > 0.0f && f[11] < 1.0f)) return false;
    const float n3 = std::sqrt(f[12] * f[12] + f[13] * f[13] + f[14] * f[14]);
    if (n3 < 0.98f || n3 > 1.02f) return false;
    const float n0 = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float n1 = std::sqrt(f[4] * f[4] + f[5] * f[5] + f[6] * f[6]);
    if (n0 < 0.2f || n0 > 5.0f || n1 < 0.2f || n1 > 5.0f) return false;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(f[16 + i] - around[i]) > kCloseMeters) return false;
    // The eye is where the gaze row vanishes.
    const float w = f[15] + f[12] * f[16] + f[13] * f[17] + f[14] * f[18];
    return std::fabs(w) < 1.0f;
}

// Whether `f` holds a light of `layout` near the input camera's eye.
bool light_block(const char* bytes, const LightLayout& layout, const double around[3]) {
    const float* m = reinterpret_cast<const float*>(bytes + layout.matrix);
    if (std::fabs(m[8]) > 0.02f || std::fabs(m[9]) > 0.02f || std::fabs(m[10]) > 0.02f) return false;
    if (!(m[11] > 0.0f && m[11] < 1.0f)) return false;
    const float n3 = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
    if (!(n3 > 0.98f && n3 < 1.02f)) return false;
    const float* eye = reinterpret_cast<const float*>(bytes + layout.eye);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(eye[i]) || std::fabs(eye[i] - around[i]) > kCloseMeters) return false;
    const float* d = reinterpret_cast<const float*>(bytes + layout.direction);
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return n > 0.98f && n < 1.02f;
}

void read_light(const char* bytes, const LightLayout& layout, Light& out) {
    std::memcpy(out.direction, bytes + layout.direction, sizeof out.direction);
    std::memcpy(out.color, bytes + layout.color, sizeof out.color);
    const float* f = reinterpret_cast<const float*>(bytes + layout.factors);
    out.sun = f[0];
    out.point_lights = f[1];
    out.gi = f[2];
    out.emissive = f[3];
    out.exposure = f[4];
    out.exposure_correction = f[5];
    out.valid = true;
}

// The bytes [offset, offset + size) of a buffer, in its mapped memory, or null.
const char* mapped_at(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    auto info = buffers.find(buffer);
    if (info == buffers.end() || !info->second.memory) return nullptr;
    auto mapping = mappings.find(info->second.memory);
    if (mapping == mappings.end()) return nullptr;
    const VkDeviceSize at = info->second.memory_offset + offset;
    if (at < mapping->second.offset || at + size > mapping->second.offset + mapping->second.size) return nullptr;
    return mapping->second.data + (at - mapping->second.offset);
}

// What the scene's pass was given, as regions to read (under mutex).
std::vector<Region> regions_of(const Recording& r, bool whole = false) {
    std::vector<Region> regions;
    // The graphics sets bound at the scene's end, with their dynamic offsets.
    struct Bound {
        VkDescriptorSet set{};
        std::vector<uint32_t> dynamic;
    };
    Bound state[32];
    const size_t binds = whole ? r.binds.size() : std::min(r.binds_at_scene_end, r.binds.size());
    for (size_t b = 0; b < binds; ++b) {
        const Bind& bind = r.binds[b];
        size_t next_dynamic = 0;
        for (size_t i = 0; i < bind.sets.size(); ++i) {
            const uint32_t index = bind.first + static_cast<uint32_t>(i);
            if (index >= 32) break;
            Bound& bound = state[index];
            bound.set = bind.sets[i];
            bound.dynamic.clear();
            auto entries = set_entries.find(bound.set);
            if (entries == set_entries.end()) continue;
            for (const Entry& entry : entries->second)
                if (entry.dynamic && next_dynamic < bind.dynamic.size()) bound.dynamic.push_back(bind.dynamic[next_dynamic++]);
        }
    }
    for (uint32_t index = 0; index < 32; ++index) {
        if (!state[index].set) continue;
        auto entries = set_entries.find(state[index].set);
        if (entries == set_entries.end()) continue;
        size_t next_dynamic = 0;
        for (const Entry& entry : entries->second) {
            VkDeviceSize offset = entry.offset;
            if (entry.dynamic) offset += next_dynamic < state[index].dynamic.size() ? state[index].dynamic[next_dynamic++] : 0;
            auto info = buffers.find(entry.buffer);
            if (info == buffers.end()) continue;
            VkDeviceSize size = entry.range == VK_WHOLE_SIZE ? info->second.size - std::min(info->second.size, offset)
                                                             : entry.range;
            Region region{entry.buffer, offset, size, {}};
            region.place.set = index;
            region.place.binding = entry.binding;
            region.place.element = entry.element;
            regions.push_back(region);
        }
    }
    // Push constants holding a device address.
    for (size_t p = 0; p < r.pushes.size(); ++p) {
        const Push& push = r.pushes[p];
        for (uint32_t at = 0; at + 8 <= push.size; at += 4) {
            uint64_t value = 0;
            std::memcpy(&value, push.data + at, 8);
            if (!value || addresses.empty()) continue;
            auto above = addresses.upper_bound(value);
            if (above == addresses.begin()) continue;
            --above;
            auto info = buffers.find(above->second);
            if (info == buffers.end() || value >= above->first + info->second.size) continue;
            Region region{above->second, value - above->first, info->second.size - (value - above->first), {}};
            region.place.push = true;
            region.place.push_index = static_cast<uint32_t>(p);
            region.place.push_byte = at;
            regions.push_back(region);
        }
    }
    return regions;
}

// The projection's centre: where rows 0, 1 and 3 all vanish.
bool solve_eye(const float* m, double eye[3]) {
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
        eye[c] = determinant(t) / det;
    }
    return true;
}

bool same_place(const Place& a, const Place& b) {
    if (a.push != b.push) return false;
    if (a.push) return a.push_index == b.push_index && a.push_byte == b.push_byte;
    return a.set == b.set && a.binding == b.binding && a.element == b.element;
}

}  // namespace

void buffer_created(VkBuffer buffer, VkDeviceSize size) {
    std::lock_guard lock(mutex);
    buffers[buffer] = BufferInfo{size};
}

void buffer_destroyed(VkBuffer buffer) {
    std::lock_guard lock(mutex);
    auto info = buffers.find(buffer);
    if (info == buffers.end()) return;
    if (info->second.address) addresses.erase(info->second.address);
    buffers.erase(info);
}

void buffer_bound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    std::lock_guard lock(mutex);
    BufferInfo& info = buffers[buffer];
    info.memory = memory;
    info.memory_offset = offset;
}

void buffer_address(VkBuffer buffer, VkDeviceAddress address) {
    if (!address) return;
    std::lock_guard lock(mutex);
    BufferInfo& info = buffers[buffer];
    if (info.address) addresses.erase(info.address);
    info.address = address;
    addresses[address] = buffer;
}

void memory_mapped(VkDeviceMemory memory, void* data, VkDeviceSize offset, VkDeviceSize size) {
    std::lock_guard lock(mutex);
    mappings[memory] = {static_cast<char*>(data), offset, size};
}

void memory_unmapped(VkDeviceMemory memory) {
    std::lock_guard lock(mutex);
    mappings.erase(memory);
}

void descriptors_written(uint32_t count, const VkWriteDescriptorSet* writes) {
    std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < count; ++i) {
        const VkWriteDescriptorSet& write = writes[i];
        const bool dynamic = write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        const bool plain = write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (!(dynamic || plain) || !write.pBufferInfo) continue;
        std::vector<Entry>& entries = set_entries[write.dstSet];
        for (uint32_t k = 0; k < write.descriptorCount; ++k) {
            const VkDescriptorBufferInfo& info = write.pBufferInfo[k];
            Entry entry{write.dstBinding, write.dstArrayElement + k, info.buffer, info.offset, info.range, dynamic};
            auto at = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) {
                return e.binding == entry.binding && e.element == entry.element;
            });
            if (at != entries.end()) *at = entry;
            else entries.push_back(entry);
        }
        // In binding order: dynamic offsets are given in that order.
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.binding != b.binding ? a.binding < b.binding : a.element < b.element;
        });
    }
}

void command_buffer_begun(VkCommandBuffer buffer) {
    Recording& r = recording_of(buffer);
    r.binds.clear();
    r.pushes.clear();
    r.in_scene = r.scene = false;
    r.binds_at_scene_end = 0;
}

void sets_bound(VkCommandBuffer buffer, VkPipelineBindPoint point, uint32_t first, uint32_t count,
        const VkDescriptorSet* sets, uint32_t dynamic_count, const uint32_t* dynamic) {
    if (point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    Recording& r = recording_of(buffer);
    // After the scene's pass too (the deferred lighting's constants); the
    // camera's search stops at binds_at_scene_end.
    if (r.binds.size() >= kMaxBinds) return;
    Bind bind;
    bind.first = first;
    bind.sets.assign(sets, sets + count);
    if (dynamic_count) bind.dynamic.assign(dynamic, dynamic + dynamic_count);
    r.binds.push_back(std::move(bind));
}

void constants_pushed(VkCommandBuffer buffer, uint32_t offset, uint32_t size, const void* data) {
    Recording& r = recording_of(buffer);
    if (!r.in_scene || r.pushes.size() >= kMaxPushes) return;
    Push push;
    push.offset = offset;
    push.size = std::min<uint32_t>(size, sizeof push.data);
    std::memcpy(push.data, data, push.size);
    r.pushes.push_back(push);
}

void scene_begun(VkCommandBuffer buffer) {
    Recording& r = recording_of(buffer);
    r.in_scene = r.scene = true;
}

void scene_ended(VkCommandBuffer buffer) {
    Recording& r = recording_of(buffer);
    if (!r.in_scene) return;
    r.in_scene = false;
    r.binds_at_scene_end = r.binds.size();
}

// The scene's light in the command buffer's regions: read where it was
// found, else searched now and then; said every 5 s.
void follow_light(const Recording& recording, const double input_eye[3], ULONGLONG now) {
    // The light changes slowly: read 5 times a second, searched every 2 s
    // (the regions of a whole command buffer are many, on the game's thread).
    if (light_found ? now - last_light_read < 200 : now - last_light_search < kSearchEvery) return;
    last_light_read = now;
    const std::vector<Region> all = regions_of(recording, true);
    bool got = false;
    if (light_found) {
        const LightLayout& layout = kLightLayouts[light_layout];
        for (const Region& region : all) {
            if (!same_place(region.place, light_place) || light_place.inner + layout.size > region.size) continue;
            if (const char* data = mapped_at(region.buffer, region.offset + light_place.inner, layout.size)) {
                if (light_block(data, layout, input_eye)) {
                    read_light(data, layout, last_light);
                    got = true;
                }
            }
            break;
        }
        if (got) light_misses = 0;
        else if (++light_misses > 60) {
            light_found = false;
            log_line("scene light: lost where it was found, searched again");
        }
    }
    if (!light_found && now - last_light_search >= kSearchEvery) {
        last_light_search = now;
        ++light_searches;
        VkDeviceSize budget = kLightSearchBudget;
        std::vector<char> bytes;
        for (const Region& region : all) {
            const VkDeviceSize size = std::min({region.size, kLightSearchBytes, budget});
            if (size < kLightLayouts[0].size) continue;
            const char* data = mapped_at(region.buffer, region.offset, size);
            if (!data) continue;
            budget -= size;
            bytes.assign(data, data + size);
            for (int k = 0; k < 2 && !light_found; ++k) {
                const LightLayout& layout = kLightLayouts[k];
                for (size_t at = 0; at + layout.size <= bytes.size(); at += 16) {
                    if (!light_block(bytes.data() + at, layout, input_eye)) continue;
                    light_found = true;
                    light_layout = k;
                    light_place = region.place;
                    light_place.inner = at;
                    read_light(bytes.data() + at, layout, last_light);
                    log_line("scene light: a %s found in set %u, binding %u[%u], at +%llu", layout.name,
                        light_place.set, light_place.binding, light_place.element, static_cast<unsigned long long>(at));
                    break;
                }
            }
            if (light_found || !budget) break;
        }
    }
    if (now - last_light_report >= 5000) {
        last_light_report = now;
        if (light_found && last_light.valid)
            log_line("scene light: sun direction %.3f %.3f %.3f, color %.3f %.3f %.3f; factors sun %.3f, point lights "
                "%.3f, gi %.3f, emissive %.3f, exposure %.4g, exposure correction %.4g", last_light.direction[0],
                last_light.direction[1], last_light.direction[2], last_light.color[0], last_light.color[1],
                last_light.color[2], last_light.sun, last_light.point_lights, last_light.gi, last_light.emissive,
                last_light.exposure, last_light.exposure_correction);
        else
            log_line("scene light: not found yet (%d searches, %zu regions in the command buffer)", light_searches,
                all.size());
    }
}

bool submitted(VkCommandBuffer buffer, Camera& camera, const double input_eye[3]) {
    std::lock_guard lock(mutex);
    auto recording = recordings.find(buffer);
    if (recording == recordings.end() || !recording->second.scene) return false;
    if (recording->second.in_scene) recording->second.binds_at_scene_end = recording->second.binds.size();
    const std::vector<Region> regions = regions_of(recording->second);
    last_regions = regions.size();
    const ULONGLONG now = GetTickCount64();
    follow_light(recording->second, input_eye, now);
    bool got = false;
    float block[19];
    if (found) {
        for (const Region& region : regions) {
            if (!same_place(region.place, place) || place.inner + 76 > region.size) continue;
            if (const char* data = mapped_at(region.buffer, region.offset + place.inner, 76)) {
                std::memcpy(block, data, sizeof block);
                got = camera_block(block, input_eye);
            }
            break;
        }
        ++follows;
        if (got) {
            misses = 0;
        } else if (++misses > 30) {
            ++follow_misses;
            found = false;
            log_line("gpu camera: lost where it was found, searched again");
        }
    }
    if (!found && now - last_search >= kSearchEvery) {
        last_search = now;
        ++searches;
        LARGE_INTEGER start{}, end{}, frequency{};
        QueryPerformanceCounter(&start);
        std::vector<char> bytes;
        VkDeviceSize budget = kSearchBudget;
        for (const Region& region : regions) {
            const VkDeviceSize size = std::min({region.size, kSearchBytes, budget});
            if (size < 76) continue;
            const char* data = mapped_at(region.buffer, region.offset, size);
            if (!data) continue;
            budget -= size;
            bytes.resize(static_cast<size_t>(size));
            std::memcpy(bytes.data(), data, bytes.size());
            for (size_t at = 0; at + 76 <= bytes.size(); at += 4) {
                const float* f = reinterpret_cast<const float*>(bytes.data() + at);
                if (std::fabs(f[8]) > 0.02f || !(f[11] > 0.0f && f[11] < 1.0f) || !camera_block(f, input_eye)) continue;
                found = true;
                place = region.place;
                place.inner = at;
                misses = 0;
                std::memcpy(block, f, sizeof block);
                got = true;
                if (place.push)
                    log_line("gpu camera: found through push constant %u, byte %u, at +%llu of its buffer", place.push_index,
                        place.push_byte, static_cast<unsigned long long>(at));
                else
                    log_line("gpu camera: found in set %u, binding %u[%u], at +%llu (near %.4g m)", place.set, place.binding,
                        place.element, static_cast<unsigned long long>(at), f[11]);
                break;
            }
            if (found || !budget) break;
        }
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&frequency);
        search_ms += 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart;
    }
    if (got) {
        std::memcpy(camera.matrix, block, sizeof camera.matrix);
        for (int i = 0; i < 3; ++i) camera.block_eye[i] = block[16 + i];
        // 30/10: the block's eye made world items slide as the view turned.
        if (!solve_eye(block, camera.eye))
            for (int i = 0; i < 3; ++i) camera.eye[i] = camera.block_eye[i];
        camera.valid = true;
    }
    if (now - last_report >= 5000) {
        last_report = now;
        double gap = -1.0, block_gap = -1.0;
        if (got) {
            const double d[3] = {camera.eye[0] - input_eye[0], camera.eye[1] - input_eye[1], camera.eye[2] - input_eye[2]};
            gap = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const double e[3] = {camera.eye[0] - camera.block_eye[0], camera.eye[1] - camera.block_eye[1],
                camera.eye[2] - camera.block_eye[2]};
            block_gap = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        }
        log_line("gpu camera: %s; %zu regions given to the scene, %zu sets and %zu mappings known, %d searches (%.1f ms), "
            "%d follows, %d lost; eye %.3f m from the input camera's, %.3f m from the block's", found ? "found" : "searching",
            last_regions, set_entries.size(), mappings.size(), searches, search_ms, follows, follow_misses, gap, block_gap);
        searches = follows = follow_misses = 0;
        search_ms = 0.0;
    }
    return got;
}

bool light(Light& out) {
    std::lock_guard lock(mutex);
    if (!light_found || !last_light.valid) return false;
    out = last_light;
    return true;
}

}  // namespace overlay::depth::gpu
