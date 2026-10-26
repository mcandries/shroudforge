#include "local_camera.h"

#include "overlay.h"

#include <imgui.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace overlay {
namespace {

using ReadLocalCamera = std::uint32_t(__cdecl*)(double* output, std::size_t count);

// Resolved from the loaded runtime, looked up again now and then while absent.
ReadLocalCamera resolve() {
    static ReadLocalCamera read = nullptr;
    static ULONGLONG next_try = 0;
    if (read) return read;
    const ULONGLONG now = GetTickCount64();
    if (now < next_try) return nullptr;
    next_try = now + 2000;
    if (HMODULE runtime = GetModuleHandleW(L"kfc-runtime.dll"))
        read = reinterpret_cast<ReadLocalCamera>(GetProcAddress(runtime, "KfcRuntimeReadLocalCamera"));
    return read;
}

double seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

void multiply(const double a[4], const double b[4], double out[4]) {
    const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

// The turn (body frame) from `from` to `to`, with w >= 0, and its angle.
double turn_between(const double from[4], const double to[4], double turn[4]) {
    const double inverse[4] = {-from[0], -from[1], -from[2], from[3]};
    multiply(inverse, to, turn);
    if (turn[3] < 0.0)
        for (int i = 0; i < 4; ++i) turn[i] = -turn[i];
    return 2.0 * std::acos(std::fmin(1.0, turn[3]));
}

// `turn` scaled by `scale` (same axis, angle times `scale`).
void scale_turn(const double turn[4], double scale, double out[4]) {
    const double half = std::acos(std::fmin(1.0, turn[3]));
    const double sine = std::sin(half);
    if (sine < 1e-9) {
        out[0] = out[1] = out[2] = 0.0;
        out[3] = 1.0;
        return;
    }
    const double factor = std::sin(half * scale) / sine;
    for (int i = 0; i < 3; ++i) out[i] = turn[i] * factor;
    out[3] = std::cos(half * scale);
}

// The camera read at each frame, newest last, for the lead and the delay.
struct Sample {
    double position[3]{};
    double orientation[4]{};
    double time = 0.0;
};
constexpr int HISTORY = 64;
Sample history[HISTORY];
int newest = -1;
int stored = 0;

const Sample& at(int age) { return history[(newest - age + HISTORY) % HISTORY]; }

// The camera's turn speed is measured over this window, at least MIN_SPAN
// long; foreseen turns are capped at MAX_ANGLE.
constexpr double WINDOW = 0.1, MIN_SPAN = 0.02, MAX_ANGLE = 0.14;

// Logged every LOG_EVERY seconds while a lead or delay is used: how often
// the camera read changes, and whether it goes back and forth.
constexpr double LOG_EVERY = 5.0;
struct Stats {
    double since = 0.0;
    int frames = 0, changes = 0, reversals = 0;
    double gap_min = 1e9, gap_max = 0.0, last_change = 0.0;
    double angle_max = 0.0, step_max = 0.0;
    double last_turn[3]{};
} stats;

void count(const Sample& sample, const Sample* before, double now) {
    ++stats.frames;
    if (before) {
        double turn[4];
        const double angle = turn_between(before->orientation, sample.orientation, turn);
        bool moved = angle > 1e-7;
        for (int i = 0; i < 3; ++i) moved = moved || sample.position[i] != before->position[i];
        if (moved) {
            ++stats.changes;
            if (stats.last_change > 0.0) {
                const double gap = sample.time - stats.last_change;
                stats.gap_min = std::min(stats.gap_min, gap);
                stats.gap_max = std::max(stats.gap_max, gap);
            }
            stats.last_change = sample.time;
            stats.step_max = std::max(stats.step_max, angle);
            if (angle > 1e-5) {
                const double dot = turn[0] * stats.last_turn[0] + turn[1] * stats.last_turn[1] +
                    turn[2] * stats.last_turn[2];
                if (dot < 0.0) ++stats.reversals;
                for (int i = 0; i < 3; ++i) stats.last_turn[i] = turn[i];
            }
        }
    }
    if (stats.since == 0.0) stats.since = now;
}

void report(double lead, double now) {
    if (now - stats.since < LOG_EVERY) return;
    const double span = now - stats.since;
    log_line("camera %s %.0f ms: %d frames in %.1f s, camera changed %d times (gap %.1f to %.1f ms), %d reversals, "
             "largest step %.2f deg, largest foreseen turn %.2f deg",
        lead >= 0.0 ? "lead" : "delay", std::fabs(lead) * 1000.0, stats.frames, span, stats.changes,
        stats.changes > 1 ? stats.gap_min * 1000.0 : 0.0, stats.gap_max * 1000.0, stats.reversals,
        stats.step_max * 57.29578, stats.angle_max * 57.29578);
    stats = Stats{};
    stats.since = now;
}

}  // namespace

namespace {
bool valid = false;
LocalCamera cached;
}  // namespace

bool last_local_camera(LocalCamera& camera) {
    camera = cached;
    return valid;
}

bool local_camera(LocalCamera& camera, double lead) {
    static int frame = -1;
    if (frame != ImGui::GetFrameCount()) {
        frame = ImGui::GetFrameCount();
        valid = false;
        if (ReadLocalCamera read = resolve()) {
            double values[17]{};
            if (read(values, 17) == 1 && values[7] > 0.0 && values[8] > 0.0) {
                for (int i = 0; i < 3; ++i) cached.position[i] = values[i];
                for (int i = 0; i < 4; ++i) cached.orientation[i] = values[3 + i];
                cached.fov_y = values[7];
                cached.aspect = values[8];
                cached.has_cursor = values[16] >= 0.0;
                if (cached.has_cursor) {
                    for (int i = 0; i < 3; ++i) cached.cursor_position[i] = values[9 + i];
                    for (int i = 0; i < 4; ++i) cached.cursor_orientation[i] = values[12 + i];
                    cached.cursor_flags = static_cast<std::uint32_t>(values[16]);
                }
                valid = true;
                Sample sample;
                for (int i = 0; i < 3; ++i) sample.position[i] = cached.position[i];
                for (int i = 0; i < 4; ++i) sample.orientation[i] = cached.orientation[i];
                sample.time = seconds();
                const Sample* before = stored > 0 ? &at(0) : nullptr;
                if (lead != 0.0) count(sample, before, sample.time);
                newest = (newest + 1) % HISTORY;
                history[newest] = sample;
                stored = std::min(stored + 1, HISTORY);
            }
        }
    }
    camera = cached;
    if (!valid || lead == 0.0) return valid;
    const Sample& latest = at(0);
    if (lead < 0.0) {
        // Delayed: the camera read `-lead` seconds before, or the oldest kept.
        int age = 0;
        while (age + 1 < stored && latest.time - at(age).time < -lead) ++age;
        const Sample& old = at(age);
        for (int i = 0; i < 3; ++i) camera.position[i] = old.position[i];
        for (int i = 0; i < 4; ++i) camera.orientation[i] = old.orientation[i];
    } else {
        // Foreseen: the mean turn and move over the last WINDOW seconds,
        // continued for `lead` seconds; nothing when the camera is still or
        // the window too short, and at most MAX_ANGLE.
        int age = 0;
        while (age + 1 < stored && latest.time - at(age + 1).time <= WINDOW) ++age;
        const Sample& old = at(age);
        const double span = latest.time - old.time;
        if (span >= MIN_SPAN) {
            double turn[4];
            const double angle = turn_between(old.orientation, latest.orientation, turn);
            double scale = lead / span;
            if (angle * scale > MAX_ANGLE) scale = MAX_ANGLE / angle;
            double step[4];
            scale_turn(turn, scale, step);
            multiply(latest.orientation, step, camera.orientation);
            for (int i = 0; i < 3; ++i)
                camera.position[i] = latest.position[i] + (latest.position[i] - old.position[i]) * scale;
            stats.angle_max = std::max(stats.angle_max, angle * scale);
        }
    }
    report(lead, latest.time);
    return valid;
}

}  // namespace overlay
