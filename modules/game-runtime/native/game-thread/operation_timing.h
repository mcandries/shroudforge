#pragma once

#include <chrono>
#include <cstdint>

// Durations of runtime operations as the calling thread sees them, kept per
// kind and reported as one line by KfcRuntimeOperationTiming (then reset):
// how long a mod waits for the game thread or for the ECS operation lock.
namespace OperationTiming {
enum class Kind : unsigned {
    DispatchWait,  // A queued operation, from Invoke to its completion.
    DispatchRun,   // Its execution on the game thread.
    DrainInterval, // Between two drains of the queue by the game thread.
    CursorLock,    // ReadLocalCursorInput waiting for the ECS operation lock.
    CursorRead,    // ReadLocalCursorInput in full.
    CursorResolve, // Its owner resolution (ResolveClientEcs).
    CursorRegistry, // Its component registry check, every second.
    CursorScan,    // Its entity scan, while incomplete.
    CursorSample,  // Its sample of the input and its checks.
    DirectResolve, // A direct ECS read: the owner resolutions before and after.
    DirectRun,     // A direct ECS read: the operation itself.
    Membership,    // One entity membership check (two id lookups).
    MembershipSlots, // Slots visited by one id lookup (recorded as a count, not microseconds).
    SampleCopy,    // The copy of the local input (both copies).
    Count
};

void Record(Kind kind, std::uint64_t microseconds);
// The calling thread's Windows priority, reported with the timings.
void NoteCallerPriority(int priority);

inline std::uint64_t MicrosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}
}
