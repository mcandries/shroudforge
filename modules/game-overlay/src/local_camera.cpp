#include "local_camera.h"

#include <imgui.h>

#include <windows.h>

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

// The last two different camera samples and when they came, for the lead.
struct Sample {
    double position[3]{};
    double orientation[4]{};
    double time = 0.0;
};

}  // namespace

bool local_camera(LocalCamera& camera, double lead) {
    static int frame = -1;
    static bool valid = false;
    static LocalCamera cached;
    static Sample previous, latest;
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
                bool moved = false;
                for (int i = 0; i < 3; ++i) moved = moved || cached.position[i] != latest.position[i];
                for (int i = 0; i < 4; ++i) moved = moved || cached.orientation[i] != latest.orientation[i];
                if (moved) {
                    previous = latest;
                    for (int i = 0; i < 3; ++i) latest.position[i] = cached.position[i];
                    for (int i = 0; i < 4; ++i) latest.orientation[i] = cached.orientation[i];
                    latest.time = ImGui::GetTime();
                }
            }
        }
    }
    camera = cached;
    // Foreseen `lead` seconds ahead: the turn between the last two samples,
    // scaled, applied once more (body frame), and the move likewise.
    const double span = latest.time - previous.time;
    if (valid && lead > 0.0 && previous.time > 0.0 && span > 0.0 && span <= 0.25) {
        const double scale = lead / span;
        const double inverse[4] = {-previous.orientation[0], -previous.orientation[1], -previous.orientation[2],
            previous.orientation[3]};
        double turn[4];
        multiply(inverse, latest.orientation, turn);
        if (turn[3] < 0.0)
            for (double& value : turn) value = -value;
        const double half = std::acos(std::fmin(1.0, turn[3]));
        const double sine = std::sin(half);
        if (sine > 1e-9) {
            const double scaled = half * scale;
            const double factor = std::sin(scaled) / sine;
            const double step[4] = {turn[0] * factor, turn[1] * factor, turn[2] * factor, std::cos(scaled)};
            multiply(latest.orientation, step, camera.orientation);
        }
        for (int i = 0; i < 3; ++i)
            camera.position[i] = latest.position[i] + (latest.position[i] - previous.position[i]) * scale;
    }
    return valid;
}

}  // namespace overlay
