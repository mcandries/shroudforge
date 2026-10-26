#pragma once

#include "overlay.h"

// Diagnostic: finds where the game hands the camera of each frame to the GPU
// (camera_probe.cpp). Off unless camera_probe.txt sits beside the layer DLL.
namespace overlay {

bool camera_probe_enabled();
// At device creation: resolves the next layer's functions the probe wraps.
void camera_probe_attach(PFN_vkGetDeviceProcAddr next_gdpa, VkDevice device);
// The probe's wrapper for a device function, or null.
PFN_vkVoidFunction camera_probe_function(const char* requested);
// At each present: notes the camera, and logs what was found every 5 s.
void camera_probe_frame();

// The camera the game rendered a recent frame with, from its frame constants
// in mapped memory: a view-projection matrix (row-major, world translation)
// and its projection centre. `age` 0: the newest frame's, 1: the one before.
// False until found (the ring is looked for while someone asks).
struct RenderCamera {
    double matrix[16]{};
    double eye[3]{};
};
bool render_camera(int age, RenderCamera& out);
// At each present, before drawing: follows the frames' camera blocks.
void render_camera_frame();

}  // namespace overlay
