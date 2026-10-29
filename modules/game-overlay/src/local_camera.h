#pragma once

#include <cstdint>

// The game's camera and building cursor for world items, read from
// ShroudForge's native runtime (kfc-runtime.dll, KfcRuntimeReadLocalCamera)
// at most once per frame.
namespace overlay {

struct LocalCamera {
    double position[3]{};      // Meters.
    double orientation[4]{};   // x, y, z, w: takes the camera's axes to the world's.
    double fov_y = 0.0;        // Radians.
    double aspect = 0.0;
    double fov_x = 0.0;        // Radians, when known (the GPU camera); else from the screen's shape.
    // The building cursor, when the runtime gives it: the corner of the
    // placement cube (meters), its orientation, and its flags (bit 0: no
    // valid position, aiming at nothing).
    bool has_cursor = false;
    double cursor_position[3]{};
    double cursor_orientation[4]{};
    std::uint32_t cursor_flags = 0;
};

// The camera for the frame being drawn; false when the runtime cannot give it
// (no world, official runtime without the export, no layout yet). `lead`
// seconds: the camera foreseen that far ahead from its mean turn and move
// over the last 0.1 s (at most 8 degrees), to make up for a camera read
// older than the frame drawn; negative, the camera read that long before,
// for one read newer than the frame drawn. Either way, how the camera read
// changes is logged every 5 s.
bool local_camera(LocalCamera& camera, double lead = 0.0);

// The camera read now, from any thread, kept nowhere (for the game's depth
// copy: the camera as the game draws the scene).
bool local_camera_now(LocalCamera& camera);

}  // namespace overlay
