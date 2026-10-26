#include "game-thread/run_game_thread_operations.h"
#include "game-thread/game_thread_state.h"
#include "engine-context/observe_entity_manager.h"
#include "game-thread/operation_timing.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <deque>

namespace GameThreadImplementation {
struct Job {
    GameThreadDispatcher::Operation operation{};
    std::shared_ptr<void> context;
    HANDLE complete{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<bool> executed{};
    std::atomic<unsigned> state{}; // 0 queued, 1 executing, 2 complete, 3 cancelled
    ~Job() { if (complete) CloseHandle(complete); }
};

std::mutex queue_mutex;
std::deque<std::shared_ptr<Job>> queue;
std::atomic<bool> accepting{};
std::string initialization_error;
std::atomic_flag draining = ATOMIC_FLAG_INIT;
thread_local unsigned dispatch_depth{};
std::atomic<bool> entity_context_hooks_ready{};
std::atomic<std::uint32_t> engine_thread{};

std::atomic<bool> command_observed{};
std::atomic<std::uint64_t> last_drain_ms{}, drain_count{}, completed_count{}, timeout_count{}, rejected_count{};
std::atomic<std::uint64_t> last_operation_us{}, maximum_operation_us{};

void __cdecl drain(void*, void*) {
    if (!accepting.load(std::memory_order_acquire)) return;
    // Several engine contexts can enter the same system. A command batch must
    // execute in one live callback, never concurrently on two worker threads.
    if (draining.test_and_set(std::memory_order_acquire)) return;
    struct CallbackScope {
        CallbackScope() { ++dispatch_depth; }
        ~CallbackScope() { --dispatch_depth; draining.clear(std::memory_order_release); }
    } scope;
    engine_thread.store(GetCurrentThreadId(), std::memory_order_release);
    static std::chrono::steady_clock::time_point previous_drain;
    const auto drain_started = std::chrono::steady_clock::now();
    if (previous_drain.time_since_epoch().count())
        OperationTiming::Record(OperationTiming::Kind::DrainInterval, OperationTiming::MicrosecondsSince(previous_drain));
    previous_drain = drain_started;
    last_drain_ms.store(GetTickCount64(), std::memory_order_relaxed);
    drain_count.fetch_add(1, std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    for (unsigned index = 0; index < 8; ++index) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(queue_mutex, std::try_to_lock);
            if (!lock || queue.empty() || !accepting.load(std::memory_order_acquire)) return;
            job = queue.front();
            queue.pop_front();
        }
        unsigned queued = 0;
        if (job->state.compare_exchange_strong(queued, 1, std::memory_order_acq_rel) && job->operation) {
            const auto operation_started = std::chrono::steady_clock::now();
            try {
                job->operation(job->context.get());
                job->executed.store(true, std::memory_order_release);
                command_observed.store(true, std::memory_order_release);
                completed_count.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                // Never unwind a C++ exception through the engine trampoline.
            }
            const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - operation_started).count());
            last_operation_us.store(duration, std::memory_order_relaxed);
            OperationTiming::Record(OperationTiming::Kind::DispatchRun, duration);
            auto maximum = maximum_operation_us.load(std::memory_order_relaxed);
            while (maximum < duration && !maximum_operation_us.compare_exchange_weak(maximum, duration, std::memory_order_relaxed)) {}
            job->state.store(2, std::memory_order_release);
        }
        SetEvent(job->complete);
        if (std::chrono::steady_clock::now() >= deadline) return;
    }
}

}

using namespace GameThreadImplementation;

namespace GameThreadDispatcher {

bool Ready() {
    const auto drain = last_drain_ms.load(std::memory_order_acquire);
    return accepting.load(std::memory_order_acquire) &&
        engine_thread.load(std::memory_order_acquire) && drain &&
        GetTickCount64() - drain <= 500;
}
bool EntityContextReady() {
    return entity_context_hooks_ready.load(std::memory_order_acquire) && Ready();
}
std::uint32_t ThreadId() { return engine_thread.load(std::memory_order_acquire); }

std::string Status() {
    if (!accepting.load(std::memory_order_acquire)) return initialization_error.empty()
        ? "unavailable" : "unavailable(" + initialization_error + ")";
    const auto thread = engine_thread.load(std::memory_order_acquire);
    if (!thread || !Ready()) {
        const auto drain = last_drain_ms.load(std::memory_order_acquire);
        if (drain && GetTickCount64() - drain > 500) {
            std::size_t queued{};
            { std::scoped_lock lock(queue_mutex); queued = queue.size(); }
            return "stale-game-thread(thread=" + std::to_string(thread) +
                ",drain=over-500ms,queued=" + std::to_string(queued) +
                ",completed=" + std::to_string(completed_count.load(std::memory_order_acquire)) +
                ",timeouts=" + std::to_string(timeout_count.load(std::memory_order_acquire)) +
                ",rejected=" + std::to_string(rejected_count.load(std::memory_order_acquire)) + ')';
        }
        return "installed-awaiting-world";
    }
    const auto manager = entity_manager.load(std::memory_order_acquire);
    return "ready(thread=" + std::to_string(thread) + ",manager=" +
        (manager ? "ready" : "awaiting-lookup") + ",commands=" +
        (command_observed.load(std::memory_order_acquire) ? "executed" : "waiting") + ')';
}

bool Invoke(Operation operation, std::shared_ptr<void> context, std::uint32_t timeout_ms) {
    if (!operation || !accepting.load(std::memory_order_acquire)) return false;
    if (engine_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
        // A remembered thread ID does not establish a current engine phase.
        // Waiting here would also block the thread that must drain the queue.
        if (!dispatch_depth) {
            rejected_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        operation(context.get());
        return true;
    }
    const auto job = std::make_shared<Job>();
    if (!job->complete) return false;
    job->operation = operation;
    job->context = std::move(context);
    {
        std::scoped_lock lock(queue_mutex);
        if (!accepting.load(std::memory_order_relaxed) || queue.size() >= 128) {
            rejected_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue.push_back(job);
    }
    const auto queued_at = std::chrono::steady_clock::now();
    if (WaitForSingleObject(job->complete, timeout_ms) == WAIT_OBJECT_0) {
        OperationTiming::Record(OperationTiming::Kind::DispatchWait, OperationTiming::MicrosecondsSince(queued_at));
        return job->executed.load(std::memory_order_acquire);
    }
    timeout_count.fetch_add(1, std::memory_order_relaxed);
    unsigned queued = 0;
    if (job->state.compare_exchange_strong(queued, 3, std::memory_order_acq_rel)) {
        std::scoped_lock lock(queue_mutex);
        std::erase(queue, job);
        return false;
    }
    // An executing operation retains its own buffers. The caller must not
    // inspect those buffers after a timeout or assume a write was cancelled.
    return false;
}

std::string Diagnostics() {
    const auto last = last_drain_ms.load(std::memory_order_relaxed);
    const auto manager_last = last_manager_observation_ms.load(std::memory_order_relaxed);
    std::unique_lock lock(queue_mutex, std::try_to_lock);
    return nlohmann::json({
        {"accepting", accepting.load()}, {"threadId", engine_thread.load()},
        {"ready", Ready()},
        {"entityContextHooksReady", EntityContextReady()},
        {"entityManagerObserved", entity_manager.load() != 0},
        {"entityManagerChanges", manager_changes.load()},
        {"lastManagerObservationAgeMs", manager_last ? nlohmann::json(GetTickCount64() - manager_last) : nlohmann::json(nullptr)},
        {"lastOperationUs", last_operation_us.load()}, {"maximumOperationUs", maximum_operation_us.load()},
        {"lastDrainAgeMs", last ? nlohmann::json(GetTickCount64() - last) : nlohmann::json(nullptr)},
        {"drainCount", drain_count.load()}, {"completed", completed_count.load()},
        {"timeouts", timeout_count.load()}, {"rejected", rejected_count.load()},
        {"queueDepth", lock ? nlohmann::json(queue.size()) : nlohmann::json(nullptr)},
        {"queueLimit",128}, {"batchLimit",8}, {"batchBudgetMs",2},
        {"inlineRequiresActiveCallback",true}, {"serializedCallbackBatches",true}
    }).dump();
}

}

namespace GameThreadImplementation {
void CancelPendingOperations() {
    std::deque<std::shared_ptr<Job>> cancelled;
    {
        std::scoped_lock lock(queue_mutex);
        cancelled.swap(queue);
    }
    for (const auto& job : cancelled) SetEvent(job->complete);
}
}
