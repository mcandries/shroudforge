#include "voxel_mesh.h"

#include <stdexcept>

namespace overlay {

namespace {

bool drawn(const std::array<ImU32, 128>& colors, unsigned char ch) {
    return ((colors[ch] >> IM_COL32_A_SHIFT) & 0xFF) != 0;
}

}  // namespace

VoxelMesh build_voxel_mesh(int sx, int sy, int sz, const std::string& cells, const std::array<ImU32, 128>& colors,
    const std::array<ImU32, 128>& marks) {
    if (sx < 1 || sy < 1 || sz < 1 || sx > kMaxVoxelSide || sy > kMaxVoxelSide || sz > kMaxVoxelSide)
        throw std::runtime_error("voxels: box sides from 1 to 128");
    const size_t total = static_cast<size_t>(sx) * sy * sz;
    std::vector<unsigned char> grid;
    grid.reserve(total);
    size_t count = 0;
    bool counting = false;
    for (const char raw : cells) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch >= 128) throw std::runtime_error("voxels: cells use ASCII characters");
        if (ch >= '0' && ch <= '9') {
            count = count * 10 + (ch - '0');
            counting = true;
            if (count > total) throw std::runtime_error("voxels: more cells than the box holds");
            continue;
        }
        const size_t repeat = counting ? count : 1;
        if (grid.size() + repeat > total) throw std::runtime_error("voxels: more cells than the box holds");
        grid.insert(grid.end(), repeat, ch);
        count = 0;
        counting = false;
    }
    if (counting || grid.size() != total) throw std::runtime_error("voxels: cells do not fill the box");

    VoxelMesh mesh;
    mesh.sx = sx;
    mesh.sy = sy;
    mesh.sz = sz;
    static constexpr int kSteps[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    const auto at = [&](int x, int y, int z) -> int {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return -1;
        return grid[(static_cast<size_t>(z) * sy + y) * sx + x];
    };
    for (int z = 0; z < sz; ++z)
        for (int y = 0; y < sy; ++y)
            for (int x = 0; x < sx; ++x) {
                const auto ch = static_cast<unsigned char>(at(x, y, z));
                if (((marks[ch] >> IM_COL32_A_SHIFT) & 0xFF) != 0)
                    mesh.marks.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                        static_cast<std::int16_t>(z), ch});
                if (!drawn(colors, ch)) continue;
                for (std::uint8_t d = 0; d < 6; ++d) {
                    const int other = at(x + kSteps[d][0], y + kSteps[d][1], z + kSteps[d][2]);
                    const bool open = other < 0 || !drawn(colors, static_cast<unsigned char>(other));
                    if (!open && other == ch) continue;
                    if (mesh.faces.size() >= kMaxVoxelFaces) throw std::runtime_error("voxels: too many faces");
                    mesh.faces.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                        static_cast<std::int16_t>(z), d, ch, !open});
                }
            }
    return mesh;
}

}  // namespace overlay
