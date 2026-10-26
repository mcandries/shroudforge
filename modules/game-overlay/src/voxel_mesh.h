// The faces of a box of cells shown by a `voxels` item: built once when a mod
// sets its zone, then projected at every frame (draw_scene.cpp).

#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace overlay {

constexpr int kMaxVoxelSide = 128;
constexpr size_t kMaxVoxelFaces = 400000;

// One side of a cell. Directions: 0 = -x, 1 = +x, 2 = -y, 3 = +y, 4 = -z, 5 = +z.
struct VoxelFace {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t direction = 0;
    unsigned char ch = 0;  // Character of the cell the face belongs to.
    bool inner = false;    // Against a drawn cell of another character: only seen in X-ray.
};

struct VoxelMark {
    std::int16_t x = 0, y = 0, z = 0;
    unsigned char ch = 0;
};

// Faces seen from a camera, kept while it does not move (draw_scene.cpp).
struct VoxelVisibility {
    bool valid = false;
    float yaw = 0.0f, pitch = 0.0f, distance = 0.0f;
    std::vector<std::uint32_t> faces;
};

struct VoxelMesh {
    int sx = 0, sy = 0, sz = 0;
    std::vector<VoxelFace> faces;
    std::vector<VoxelMark> marks;
    std::vector<std::uint8_t> drawn;  // 1 for a drawn cell, x fastest, then y, then z.
    mutable VoxelVisibility visibility;
};

// Decodes `cells`, run-length text of sx * sy * sz cells (x fastest, then y,
// then z): each character is one cell, a decimal count before it repeats it
// ("40.3#" = 40 '.' then 3 '#'). A cell is drawn when its character has a
// color. Keeps every face of a drawn cell that touches a cell not drawn (or
// the outside of the box), the faces against a drawn cell of another
// character (inner), and the cells whose character has a mark. Throws
// std::runtime_error on malformed input.
VoxelMesh build_voxel_mesh(int sx, int sy, int sz, const std::string& cells, const std::array<ImU32, 128>& colors,
    const std::array<ImU32, 128>& marks);

}  // namespace overlay
