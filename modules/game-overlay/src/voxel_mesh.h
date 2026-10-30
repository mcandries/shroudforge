// The faces of a box of cells shown by a `voxels` item: built once when a mod
// sets its zone, then projected at every frame (draw_scene.cpp).

#pragma once

#include <imgui.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace overlay {

constexpr int kMaxVoxelSide = 256;
constexpr size_t kMaxVoxelFaces = 400000;
// A box larger than this on a side also gets coarser meshes (2 and 4 cells a
// cell), drawn when its cells would be smaller than a few pixels.
constexpr int kVoxelLevelsFrom = 64;
constexpr int kVoxelLevels = 2;
// Most faces of the mesh a voxels view draws by default (`max_faces`): a
// coarser one beyond (27/10: 256 x 64 x 256 in the large frame, 250 000
// faces, 7 ms a frame).
constexpr int kDefaultVoxelFaces = 120000;

// One side of a cell. Directions: 0 = -x, 1 = +x, 2 = -y, 3 = +y, 4 = -z, 5 = +z.
struct VoxelFace {
    std::int16_t x = 0, y = 0, z = 0;
    std::uint8_t direction = 0;
    unsigned char ch = 0;  // Character of the cell the face belongs to.
    bool inner = false;    // Against a drawn cell of another character: only seen in X-ray.
    // Edges where the surface turns, bit k for the edge from corner k to k + 1
    // (corners (0, 0), (1, 0), (1, 1), (0, 1) across the two axes after the
    // normal's, in order): no face of a cell of the same character goes on in
    // the same plane beyond it.
    std::uint8_t creases = 0;
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

// Faces to draw from a camera, farthest first, kept while it does not move.
struct VoxelOrder {
    bool valid = false, xray = false, see_through = false;
    float yaw = 0.0f, pitch = 0.0f, distance = 0.0f;
    std::vector<std::uint32_t> faces;
};

struct VoxelMesh {
    int sx = 0, sy = 0, sz = 0;
    int scale = 1;  // Cells of the box per cell of this mesh (coarser levels).
    std::vector<VoxelFace> faces;
    std::vector<VoxelMark> marks;
    std::vector<std::uint8_t> drawn;  // 1 for a drawn cell, x fastest, then y, then z.
    mutable VoxelVisibility visibility;
    mutable VoxelOrder order;
    // Coarser meshes of the same box, 2 then 4 cells a cell (a large box only).
    std::vector<std::shared_ptr<const VoxelMesh>> coarser;
};

// What a mesh is built from: the same source gives the same mesh.
struct VoxelSource {
    int sx = 0, sy = 0, sz = 0;
    std::shared_ptr<const std::string> cells;
    std::array<ImU32, 128> colors{}, marks{};
    bool levels = false;
    bool one_shape = false;
    bool same(const VoxelSource& other) const {
        return sx == other.sx && sy == other.sy && sz == other.sz && levels == other.levels &&
               one_shape == other.one_shape &&
               colors == other.colors && marks == other.marks &&
               (cells == other.cells || (cells && other.cells && *cells == *other.cells));
    }
};

// A mesh built on the overlay's own thread (build_voxel_mesh_later): until it
// is ready, the item shows the one it replaces (`previous`, set when its zone
// replaces another), so a mod setting its zone never waits for the faces. A
// zone setting the same source again keeps the slot of the one it replaces
// (scene.cpp): nothing is built again.
struct VoxelSlot {
    std::shared_ptr<const VoxelSource> source;
    std::atomic<std::shared_ptr<const VoxelMesh>> ready;
    std::shared_ptr<const VoxelMesh> previous;
    std::shared_ptr<const VoxelMesh> shown() const {
        auto mesh = ready.load();
        return mesh ? mesh : previous;
    }
};

// The sx * sy * sz characters of run-length `cells` (see build_voxel_mesh),
// x fastest. Throws std::runtime_error on malformed input.
std::vector<unsigned char> decode_voxel_cells(int sx, int sy, int sz, const std::string& cells);

// Checks `cells` as decode_voxel_cells reads them, without decoding them.
// Throws std::runtime_error on malformed input.
void check_voxel_cells(int sx, int sy, int sz, const std::string& cells);

// Decodes `cells`, run-length text of sx * sy * sz cells (x fastest, then y,
// then z): each character is one cell, a decimal count before it repeats it
// ("40.3#" = 40 '.' then 3 '#'). A cell is drawn when its character has a
// color. Keeps every face of a drawn cell that touches a cell not drawn (or
// the outside of the box), the faces against a drawn cell of another
// character (inner), and the cells whose character has a mark; marks the
// creases of each face. With `levels`, a box over kVoxelLevelsFrom on a side
// also gets its coarser meshes: each of their cells takes, among the cells it
// covers, the drawn character the rarest in the box (a planned block, an
// endpoint, a wall before the terrain), so that small things stay seen.
// With `one_shape`, every drawn character is one shape: no inner face, creases
// where the whole shape turns (a build whose cells say what was there).
// Throws std::runtime_error on malformed input.
VoxelMesh build_voxel_mesh(int sx, int sy, int sz, const std::string& cells, const std::array<ImU32, 128>& colors,
    const std::array<ImU32, 128>& marks, bool levels = false, bool one_shape = false);

// build_voxel_mesh of `slot->source` on the overlay's thread, then `slot`
// filled, unless its item is gone meanwhile (its zone replaced again: not
// built). Checked input only (check_voxel_cells); an error goes to the log
// (27/10: 24 to 40 ms for 256 x 64 x 256, in overlay.set, on the game's mods
// loop).
void build_voxel_mesh_later(const std::shared_ptr<VoxelSlot>& slot);

}  // namespace overlay
