#include "overlay.h"
#include "voxel_mesh.h"

#include <deque>
#include <mutex>
#include <utility>

namespace overlay {

namespace {

struct Job {
    std::weak_ptr<VoxelSlot> slot;
    std::shared_ptr<const VoxelSource> source;
};

std::mutex jobs_mutex;
std::deque<Job> jobs;
bool working = false;

void run(const Job& job) {
    if (job.slot.expired()) return;
    try {
        const VoxelSource& s = *job.source;
        auto mesh = std::make_shared<const VoxelMesh>(build_voxel_mesh(s.sx, s.sy, s.sz, *s.cells, s.colors, s.marks, s.levels, s.one_shape));
        if (auto slot = job.slot.lock()) slot->ready.store(std::move(mesh));
    } catch (const std::exception& error) {
        log_line("voxels: %s", error.what());
    }
}

// The next job, or none: the worker then stops (`working` cleared at once,
// so that the next job starts another).
bool take(Job& job) {
    std::lock_guard lock(jobs_mutex);
    if (jobs.empty()) {
        working = false;
        return false;
    }
    job = std::move(jobs.front());
    jobs.pop_front();
    return true;
}

// Runs while there are jobs. It holds a reference to this module, given back
// as it ends (FreeLibraryAndExitThread): the layer cannot be unloaded under
// it, and no thread object is left for the end of the process (a joinable
// global std::thread ends the game with 0xC0000409, 27/10).
DWORD WINAPI work(void* module) {
    Job job;
    while (take(job)) run(job);
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

}  // namespace

void build_voxel_mesh_later(const std::shared_ptr<VoxelSlot>& slot) {
    if (!slot || !slot->source) return;
    {
        std::lock_guard lock(jobs_mutex);
        jobs.push_back({slot, slot->source});
        if (working) return;
        working = true;
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&work), &module)) {
        if (HANDLE thread = CreateThread(nullptr, 0, work, module, 0, nullptr)) {
            CloseHandle(thread);
            return;
        }
        FreeLibrary(module);
    }
    // No thread: built here.
    log_line("voxels: no thread to build the faces (%lu), built at once", GetLastError());
    Job job;
    while (take(job)) run(job);
}

}  // namespace overlay
