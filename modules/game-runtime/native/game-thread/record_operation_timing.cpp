#include "game-thread/operation_timing.h"
#include "abi/runtime_api.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Counter {
    std::atomic<std::uint64_t> count{}, total{}, maximum{};
};
Counter counters[static_cast<unsigned>(OperationTiming::Kind::Count)];
constexpr const char* names[] = {"dispatch-wait", "dispatch-run", "drain-interval", "cursor-lock", "cursor-read",
    "cursor-resolve", "cursor-registry", "cursor-scan", "cursor-sample", "direct-resolve", "direct-run",
    "membership", "membership-slots(count, shown as ms/1000)", "sample-copy"};
std::atomic<int> caller_priority{-99};
}

namespace OperationTiming {
void NoteCallerPriority(int priority) { caller_priority.store(priority, std::memory_order_relaxed); }

void Record(Kind kind, std::uint64_t microseconds) {
    auto& counter = counters[static_cast<unsigned>(kind)];
    counter.count.fetch_add(1, std::memory_order_relaxed);
    counter.total.fetch_add(microseconds, std::memory_order_relaxed);
    auto maximum = counter.maximum.load(std::memory_order_relaxed);
    while (maximum < microseconds && !counter.maximum.compare_exchange_weak(maximum, microseconds, std::memory_order_relaxed)) {}
}
}

// "name count avg/max ms; ..." for every kind seen since the last call, then
// reset. Returns the text length (truncated to capacity - 1).
extern "C" std::size_t __cdecl KfcRuntimeOperationTiming(char* buffer, std::size_t capacity) {
    if (!buffer || !capacity) return 0;
    std::string text;
    for (unsigned index = 0; index < static_cast<unsigned>(OperationTiming::Kind::Count); ++index) {
        auto& counter = counters[index];
        const auto count = counter.count.exchange(0, std::memory_order_relaxed);
        const auto total = counter.total.exchange(0, std::memory_order_relaxed);
        const auto maximum = counter.maximum.exchange(0, std::memory_order_relaxed);
        if (!count) continue;
        char line[128];
        std::snprintf(line, sizeof(line), "%s%s %llux avg %.2f max %.2f ms", text.empty() ? "" : "; ", names[index],
            static_cast<unsigned long long>(count), total / 1000.0 / count, maximum / 1000.0);
        text += line;
    }
    const auto priority = caller_priority.load(std::memory_order_relaxed);
    if (priority != -99) text += "; caller thread priority " + std::to_string(priority);
    const auto length = std::min(text.size(), capacity - 1);
    std::memcpy(buffer, text.data(), length);
    buffer[length] = '\0';
    return length;
}
