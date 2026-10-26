#pragma once

// The game's camera for world items, read from ShroudForge's native runtime
// (kfc-runtime.dll, KfcRuntimeReadLocalCamera) at most once per frame.
namespace overlay {

struct LocalCamera {
    double position[3]{};      // Meters.
    double orientation[4]{};   // x, y, z, w: takes the camera's axes to the world's.
    double fov_y = 0.0;        // Radians.
    double aspect = 0.0;
};

// The camera for the frame being drawn; false when the runtime cannot give it
// (no world, official runtime without the export, no layout yet).
bool local_camera(LocalCamera& camera);

}  // namespace overlay
