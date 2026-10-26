#include "local_camera.h"

#include <imgui.h>

#include <windows.h>

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

}  // namespace

bool local_camera(LocalCamera& camera) {
    static int frame = -1;
    static bool valid = false;
    static LocalCamera cached;
    if (frame == ImGui::GetFrameCount()) {
        camera = cached;
        return valid;
    }
    frame = ImGui::GetFrameCount();
    valid = false;
    if (ReadLocalCamera read = resolve()) {
        double values[9]{};
        if (read(values, 9) == 1 && values[7] > 0.0 && values[8] > 0.0) {
            for (int i = 0; i < 3; ++i) cached.position[i] = values[i];
            for (int i = 0; i < 4; ++i) cached.orientation[i] = values[3 + i];
            cached.fov_y = values[7];
            cached.aspect = values[8];
            valid = true;
        }
    }
    camera = cached;
    return valid;
}

}  // namespace overlay
