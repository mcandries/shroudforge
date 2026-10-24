#include "player-input/world_cursor_state.h"
#include "ecs/ecs_session_state.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The local input belongs to the original main-client ECS owner. The default
// ECS session can separately belong to a hosted authority world, so its type
// indices and entity handles must never be reused for the input owner.
namespace WorldRuntime::Implementation {
namespace {
using namespace EcsImplementation;
constexpr const char* InputType = "keen::ecs::ClientPlayerInput";
constexpr std::size_t MaximumInputBytes = 65536;
constexpr std::size_t MaximumCandidates = 32;
// In multiplayer the main-client ECS holds a ClientPlayerInput for every
// player: the other players' copies are replicated. The local player's entity
// also carries client-only components; the first of these markers held by
// exactly one candidate tells the local input.
constexpr const char* LocalInputMarkers[] = {
    "keen::ecs::ClientOnlyPlayerInput", "keen::ecs::ClientPlayerSettings", "keen::ecs::ScenePlayerCamera",
    "keen::ecs::ClientPlayerFocus", "keen::ecs::ClientPlayerState"};

struct LocalInputState {
    EngineContext::EcsView owner;
    ComponentType component{};
    std::uintptr_t records{};
    std::size_t registrations{}, entity_count{}, cursor{}, candidate_slot{}, published_slot{};
    EntityView candidate, published;
    std::vector<std::pair<EntityView, std::size_t>> candidates;
    std::vector<std::pair<ComponentType, std::string>> components;
    std::uint64_t session{}, registry_ms{}, completed_ms{};
    bool configured{}, complete{}, ambiguous{};
};
std::mutex local_input_mutex;
LocalInputState local_input;
std::uint64_t local_input_sequence{};
// How the last complete scan chose, or why it could not: for error messages.
std::mutex local_input_detail_mutex;
std::string local_input_detail;

ResolvedLayout client_layout(const EngineContext::EcsView& owner){
    const auto& source = owner.layout;
    ResolvedLayout layout;
    layout.count_address = owner.manager + source.manager_count;
    layout.table_address = owner.manager + source.manager_table;
    layout.occupancy_address = source.manager_occupancy ? owner.manager + source.manager_occupancy : 0;
    layout.keys_address = source.manager_keys ? owner.manager + source.manager_keys : 0;
    layout.entity_id = source.entity_id;
    layout.entity_generation = source.entity_generation;
    layout.entity_layout = source.entity_layout;
    layout.entity_storage = source.entity_storage;
    layout.entity_definition = source.entity_definition;
    layout.entity_definition_uuid = source.entity_definition_uuid;
    layout.entity_definition_uuid_validated = source.entity_definition_uuid_validated;
    layout.entity_row = source.entity_row;
    layout.component_bits = source.component_bits;
    layout.component_offsets = source.component_offsets;
    layout.component_strides = source.component_strides;
    return layout;
}

bool resolve_input_registration(const EngineContext::EcsView& owner, std::size_t size,
                                ComponentType& component, std::uintptr_t& records, std::size_t& count,
                                std::vector<std::pair<ComponentType, std::string>>& components){
    std::unordered_map<std::string, std::uint32_t> contract;
    { std::scoped_lock lock(state_mutex); contract = configured_types; }
    const auto expected = contract.find(InputType);
    if (expected == contract.end() || expected->second != size) return false;
    ComponentRegistry::Snapshot registry;
    std::string error;
    if (!ComponentRegistry::Read(owner.manager, owner.owners.image, image_size, read_bytes, registry, error) ||
        registry.owner != owner.registry_owner) return false;
    bool found{};
    components.clear();
    for (const auto& entry : registry.entries) {
        for (const auto* type : {&entry.storage, &entry.configuration}) {
            if (type->name.empty()) continue;
            const auto type_contract = contract.find(type->name);
            if (type_contract == contract.end() || type_contract->second != type->size) return false;
        }
        const auto& name = entry.storage.name.empty() ? entry.name : entry.storage.name;
        if (!name.empty()) components.emplace_back(ComponentType{entry.index, entry.size}, name);
        if (entry.storage.name != InputType) continue;
        if (found || entry.storage.size != size || entry.size != size) return false;
        component = {entry.index, entry.size};
        found = true;
    }
    if (!found) return false;
    records = registry.records;
    count = registry.entries.size();
    return true;
}

bool current_registry_header(const LocalInputState& state){
    std::uintptr_t records{}, owner{};
    std::uint64_t count{};
    return read(state.owner.manager, owner) && owner == state.owner.registry_owner &&
        read(owner + 8, records) && records == state.records &&
        read(owner + 16, count) && count == state.registrations;
}

bool current_entity_header(const LocalInputState& state, const ResolvedLayout& layout){
    std::uintptr_t table{};
    std::uint64_t count{};
    return read(layout.count_address, count) && count == state.entity_count &&
        read(layout.table_address, table) && table == state.owner.entity_table;
}

void restart_scan(LocalInputState& state, bool retain_published = false){
    state.cursor = 0;
    state.candidate = {};
    state.candidate_slot = 0;
    state.candidates.clear();
    state.complete = state.ambiguous = false;
    state.completed_ms = 0;
    if (!retain_published) { state.published = {}; state.published_slot = 0; }
}

void publish_detail(std::string detail){
    std::scoped_lock lock(local_input_detail_mutex);
    local_input_detail = std::move(detail);
}

std::string short_name(const std::string& name){
    constexpr std::string_view prefix = "keen::ecs::";
    return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
}

// Several candidates: keep the one a local-input marker singles out, else
// describe the components only some of them hold, for the error message.
bool choose_local_input(LocalInputState& state, const ResolvedLayout& layout){
    const auto holders = [&](const ComponentType& type, std::vector<std::size_t>& found){
        found.clear();
        for (std::size_t i = 0; i < state.candidates.size(); ++i) {
            bool present{};
            if (!component_presence(state.candidates[i].first, layout, type, present)) return false;
            if (present) found.push_back(i);
        }
        return true;
    };
    std::vector<std::size_t> found;
    for (const char* marker : LocalInputMarkers) {
        const auto type = std::find_if(state.components.begin(), state.components.end(),
            [&](const auto& entry){ return entry.second == marker; });
        if (type == state.components.end() || !holders(type->first, found) || found.size() != 1) continue;
        state.candidate = state.candidates[found[0]].first;
        state.candidate_slot = state.candidates[found[0]].second;
        publish_detail(std::to_string(state.candidates.size()) + " player inputs, the local one has " +
            short_name(marker));
        return true;
    }
    // Not decided: describe the candidates, the markers, the components only
    // some of them hold, and the components whose content differs (with both
    // contents for the small ones), to find what tells the local player.
    const auto count = state.candidates.size();
    std::string detail = std::to_string(count) + " player inputs:";
    for (std::size_t i = 0; i < count; ++i) {
        const auto& entity = state.candidates[i].first;
        detail += " " + std::to_string(i) + "=id" + std::to_string(entity.id) + "/gen" + std::to_string(entity.generation) +
            "/slot" + std::to_string(state.candidates[i].second);
    }
    detail += "; markers:";
    for (const char* marker : LocalInputMarkers) {
        const auto type = std::find_if(state.components.begin(), state.components.end(),
            [&](const auto& entry){ return entry.second == marker; });
        detail += " " + short_name(marker) + "=";
        if (type == state.components.end()) detail += "unregistered";
        else if (!holders(type->first, found)) detail += "unreadable";
        else detail += std::to_string(found.size()) + "/" + std::to_string(count);
    }
    std::size_t unreadable{}, checked{};
    std::string some_only, differing;
    const auto hex = [](const std::vector<std::uint8_t>& bytes){
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (const auto byte : bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
        return text;
    };
    for (const auto& [type, name] : state.components) {
        if (!holders(type, found)) { ++unreadable; continue; }
        ++checked;
        if (found.empty()) continue;
        if (found.size() != count) {
            if (some_only.size() < 400) {
                some_only += " " + short_name(name) + "[";
                for (std::size_t i = 0; i < found.size(); ++i) some_only += (i ? "," : "") + std::to_string(found[i]);
                some_only += "]";
            }
            continue;
        }
        if (!type.size || type.size > 4096 || differing.size() > 1100) continue;
        std::vector<std::vector<std::uint8_t>> contents(count, std::vector<std::uint8_t>(type.size));
        bool read_all = true;
        for (std::size_t i = 0; i < count && read_all; ++i) {
            std::uintptr_t address{};
            read_all = component_address(state.candidates[i].first, layout, type, address) &&
                read_bytes(address, contents[i].data(), contents[i].size());
        }
        if (!read_all || std::all_of(contents.begin(), contents.end(), [&](const auto& c){ return c == contents[0]; })) continue;
        differing += " " + short_name(name) + "(" + std::to_string(type.size) + ")";
        if (type.size <= 24) {
            differing += "=";
            for (std::size_t i = 0; i < count; ++i) differing += (i ? "|" : "") + hex(contents[i]);
        }
    }
    detail += "; " + std::to_string(checked) + " components checked, " + std::to_string(unreadable) + " unreadable";
    detail += "; on some only:" + (some_only.empty() ? std::string(" none") : some_only);
    detail += "; different content:" + (differing.empty() ? std::string(" none") : differing);
    publish_detail(std::move(detail));
    return false;
}

std::uint32_t scan_local_input(LocalInputState& state, const ResolvedLayout& layout){
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    std::size_t visited{};
    while (state.cursor < state.entity_count) {
        if (visited >= 8192 || (visited && std::chrono::steady_clock::now() >= deadline)) return 5;
        const auto slot = state.cursor++;
        ++visited;
        if (layout.occupancy_address) {
            std::uintptr_t occupancy{};
            std::uint64_t bits{};
            if (!read(layout.occupancy_address, occupancy) || !occupancy ||
                !read(occupancy + (slot / 64) * sizeof(bits), bits)) { restart_scan(state); return 5; }
            if (!(bits & (std::uint64_t{1} << (slot % 64)))) continue;
        }
        std::uintptr_t pointer{};
        if (!read(state.owner.entity_table + slot * sizeof(pointer), pointer)) { restart_scan(state); return 5; }
        if (!pointer) {
            if (layout.occupancy_address) { restart_scan(state); return 5; }
            continue;
        }
        EntityView entity{};
        bool present{};
        if (!entity_view(pointer, layout, entity) || !component_presence(entity, layout, state.component, present)) {
            restart_scan(state);
            return 5;
        }
        if (!present) continue;
        if (state.candidates.size() < MaximumCandidates) state.candidates.emplace_back(entity, slot);
    }
    if (state.candidates.size() == 1) {
        state.candidate = state.candidates.front().first;
        state.candidate_slot = state.candidates.front().second;
    } else if (state.candidates.size() > 1) {
        state.ambiguous = !choose_local_input(state, layout);
    }
    state.complete = true;
    state.completed_ms = GetTickCount64();
    state.published = state.ambiguous ? EntityView{} : state.candidate;
    state.published_slot = state.candidate_slot;
    return state.ambiguous ? 6u : state.candidate.pointer ? 1u : 4u;
}

bool sample_local_input(const LocalInputState& state, const ResolvedLayout& layout,
                        std::vector<std::uint8_t>& sample){
    std::uintptr_t pointer{}, address{}, again_address{};
    EntityView entity{}, again{};
    if (state.published_slot >= state.entity_count ||
        !read(state.owner.entity_table + state.published_slot * sizeof(pointer), pointer) || pointer != state.published.pointer ||
        !entity_view(pointer, layout, entity) || entity.id != state.published.id || entity.generation != state.published.generation ||
        !component_address(entity, layout, state.component, address)) return false;
    std::vector<std::uint8_t> verification(sample.size());
    return read_bytes(address, sample.data(), sample.size()) &&
        read_bytes(address, verification.data(), verification.size()) && verification == sample &&
        entity_view(pointer, layout, again) && again == entity &&
        component_address(again, layout, state.component, again_address) && again_address == address &&
        read(state.owner.entity_table + state.published_slot * sizeof(pointer), pointer) && pointer == entity.pointer;
}
}

bool LocalCursorInputAvailable(){
    EngineContext::EcsView owner;
    if (EcsImplementation::stop_requested.load(std::memory_order_acquire) ||
        !KfcRuntimeWorldSessionId() || !EngineContext::ResolveClientEcs(owner)) return false;
    std::scoped_lock lock(EcsImplementation::state_mutex);
    const auto contract = EcsImplementation::configured_types.find(InputType);
    return contract != EcsImplementation::configured_types.end() && contract->second && contract->second <= MaximumInputBytes;
}

std::uint32_t ReadLocalCursorInput(std::size_t component_size, std::uint64_t expected_session,
                                  std::uint8_t* buffer, std::size_t capacity, std::uint64_t* sequence){
    if (!component_size || component_size > MaximumInputBytes || !expected_session || !buffer ||
        capacity != component_size || !sequence) return 2;
    std::scoped_lock operation_lock(EcsImplementation::operation_mutex);
    std::unique_lock input_lock(local_input_mutex, std::try_to_lock);
    if (!input_lock.owns_lock()) return 5;
    EngineContext::EcsView owner;
    if (EcsImplementation::stop_requested.load(std::memory_order_acquire) ||
        KfcRuntimeWorldSessionId() != expected_session || !EngineContext::ResolveClientEcs(owner)) {
        local_input = {};
        return 4;
    }
    const auto now = GetTickCount64();
    const auto layout = client_layout(owner);
    std::uint64_t count{};
    std::uintptr_t table{};
    if (!read(layout.count_address, count) || !count || count > owner.layout.maximum_entities ||
        !read(layout.table_address, table) || table != owner.entity_table) return 4;
    const bool changed = !local_input.configured || local_input.owner != owner || local_input.session != expected_session ||
        local_input.component.size != component_size || local_input.entity_count != count;
    if (changed) {
        local_input = {};
        local_input.owner = owner;
        local_input.session = expected_session;
        local_input.entity_count = static_cast<std::size_t>(count);
    }
    if (changed || now - local_input.registry_ms >= 1000) {
        ComponentType component{};
        std::uintptr_t records{};
        std::size_t registrations{};
        std::vector<std::pair<ComponentType, std::string>> components;
        if (!resolve_input_registration(owner, component_size, component, records, registrations, components)) {
            local_input = {};
            return 3;
        }
        if (!local_input.configured || local_input.component != component || local_input.records != records ||
            local_input.registrations != registrations) restart_scan(local_input);
        local_input.components = std::move(components);
        local_input.component = component;
        local_input.records = records;
        local_input.registrations = registrations;
        local_input.registry_ms = now;
        local_input.configured = true;
    }
    if (!current_registry_header(local_input)) { local_input = {}; return 5; }
    // A periodic discovery refresh may span several short slices. Keep the
    // last completely validated unique input while refreshing, but revalidate
    // its original membership and payload for every published sample. Owner,
    // registry, entity and unreadable-slot failures discard it immediately.
    if (local_input.complete && now - local_input.completed_ms >= 1000) restart_scan(local_input, true);
    if (!local_input.complete) {
        const auto result = scan_local_input(local_input, layout);
        if (result != 1 && !(result == 5 && local_input.published.pointer)) return result;
    }
    if (local_input.ambiguous) return 6;
    if (!local_input.published.pointer) return 4;
    std::vector<std::uint8_t> sample(component_size);
    EngineContext::EcsView after;
    if (!sample_local_input(local_input, layout, sample) || !current_registry_header(local_input) ||
        !current_entity_header(local_input, layout) ||
        EcsImplementation::stop_requested.load(std::memory_order_acquire) ||
        KfcRuntimeWorldSessionId() != expected_session || !EngineContext::ResolveClientEcs(after) || after != owner) {
        restart_scan(local_input);
        return 5;
    }
    std::memcpy(buffer, sample.data(), sample.size());
    *sequence = ++local_input_sequence;
    return 1;
}
}

extern "C" std::uint32_t __cdecl KfcRuntimeWorldCursorReadInput(std::size_t component_size,
    std::uint64_t expected_session, std::uint8_t* buffer, std::size_t capacity, std::uint64_t* sequence){
    try { return WorldRuntime::Implementation::ReadLocalCursorInput(component_size, expected_session, buffer, capacity, sequence); }
    catch (...) { return 4; }
}

// How the last scan chose among several player inputs, or why it could not.
// Returns the text length (truncated to capacity - 1).
extern "C" std::size_t __cdecl KfcRuntimeWorldCursorInputDetail(char* buffer, std::size_t capacity){
    if (!buffer || !capacity) return 0;
    try {
        std::scoped_lock lock(WorldRuntime::Implementation::local_input_detail_mutex);
        const auto& detail = WorldRuntime::Implementation::local_input_detail;
        const auto length = std::min(detail.size(), capacity - 1);
        std::memcpy(buffer, detail.data(), length);
        buffer[length] = '\0';
        return length;
    } catch (...) { buffer[0] = '\0'; return 0; }
}
