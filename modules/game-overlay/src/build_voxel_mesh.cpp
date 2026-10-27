#include "voxel_mesh.h"

#include <algorithm>
#include <stdexcept>

namespace overlay {

namespace {

bool drawn(const std::array<ImU32, 128>& colors, unsigned char ch) {
    return ((colors[ch] >> IM_COL32_A_SHIFT) & 0xFF) != 0;
}

// The faces (and marks, `with_marks`) of a decoded grid.
VoxelMesh mesh_of_grid(int sx, int sy, int sz, const std::vector<unsigned char>& grid,
    const std::array<ImU32, 128>& colors, const std::array<ImU32, 128>& marks, bool with_marks) {
    const size_t total = grid.size();
    bool is_drawn[128], is_marked[128];
    for (int ch = 0; ch < 128; ++ch) {
        is_drawn[ch] = drawn(colors, static_cast<unsigned char>(ch));
        is_marked[ch] = with_marks && ((marks[ch] >> IM_COL32_A_SHIFT) & 0xFF) != 0;
    }
    VoxelMesh mesh;
    mesh.sx = sx;
    mesh.sy = sy;
    mesh.sz = sz;
    mesh.drawn.resize(total);
    for (size_t i = 0; i < total; ++i) mesh.drawn[i] = is_drawn[grid[i]] ? 1 : 0;
    static constexpr int kSteps[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    const auto at = [&](int x, int y, int z) -> int {
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return -1;
        return grid[(static_cast<size_t>(z) * sy + y) * sx + x];
    };
    const auto drawn_at = [&](const int p[3]) {
        const int other = at(p[0], p[1], p[2]);
        return other >= 0 && is_drawn[other];
    };
    // Edge k of a face: the side of its corners k and k + 1, towards -v, +u,
    // +v, -u (u and v the two axes after the normal's).
    static constexpr int kEdges[4][2] = {{1, -1}, {0, 1}, {1, 1}, {0, -1}};
    // The surface turns at an edge unless the cell beyond it has the same
    // character and the same face open.
    const auto creases = [&](int x, int y, int z, std::uint8_t d, unsigned char ch) {
        const int axis = d / 2, sides[2] = {(axis + 1) % 3, (axis + 2) % 3};
        std::uint8_t bits = 0;
        for (int k = 0; k < 4; ++k) {
            int beyond[3] = {x, y, z};
            beyond[sides[kEdges[k][0]]] += kEdges[k][1];
            int front[3] = {beyond[0] + kSteps[d][0], beyond[1] + kSteps[d][1], beyond[2] + kSteps[d][2]};
            if (at(beyond[0], beyond[1], beyond[2]) != ch || drawn_at(front)) bits |= 1 << k;
        }
        return bits;
    };
    // Neighbours by index, -1 outside the box (27/10: the bounds of every
    // neighbour checked by `at`, 32 ms for 256 x 64 x 256).
    const size_t row = static_cast<size_t>(sx), plane = row * sy;
    size_t i = 0;
    for (int z = 0; z < sz; ++z)
        for (int y = 0; y < sy; ++y)
            for (int x = 0; x < sx; ++x, ++i) {
                const unsigned char ch = grid[i];
                if (is_marked[ch])
                    mesh.marks.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                        static_cast<std::int16_t>(z), ch});
                if (!is_drawn[ch]) continue;
                const int around[6] = {x > 0 ? grid[i - 1] : -1, x + 1 < sx ? grid[i + 1] : -1,
                    y > 0 ? grid[i - row] : -1, y + 1 < sy ? grid[i + row] : -1, z > 0 ? grid[i - plane] : -1,
                    z + 1 < sz ? grid[i + plane] : -1};
                for (std::uint8_t d = 0; d < 6; ++d) {
                    const int other = around[d];
                    const bool open = other < 0 || !is_drawn[other];
                    if (!open && other == ch) continue;
                    if (mesh.faces.size() >= kMaxVoxelFaces) throw std::runtime_error("voxels: too many faces");
                    mesh.faces.push_back({static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                        static_cast<std::int16_t>(z), d, ch, !open, open ? creases(x, y, z, d, ch) : std::uint8_t{0}});
                }
            }
    return mesh;
}

// The grid at half its size on each side: each cell takes the drawn character
// of the highest `rank` among the up to 8 it covers, or else the first one.
std::vector<unsigned char> halve(int sx, int sy, int sz, const std::vector<unsigned char>& grid, const int rank[128],
    int& hx, int& hy, int& hz) {
    hx = (sx + 1) / 2;
    hy = (sy + 1) / 2;
    hz = (sz + 1) / 2;
    std::vector<unsigned char> out(static_cast<size_t>(hx) * hy * hz);
    size_t o = 0;
    for (int z = 0; z < hz; ++z)
        for (int y = 0; y < hy; ++y)
            for (int x = 0; x < hx; ++x, ++o) {
                unsigned char best = grid[(static_cast<size_t>(2 * z) * sy + 2 * y) * sx + 2 * x];
                for (int dz = 0; dz < 2 && 2 * z + dz < sz; ++dz)
                    for (int dy = 0; dy < 2 && 2 * y + dy < sy; ++dy)
                        for (int dx = 0; dx < 2 && 2 * x + dx < sx; ++dx) {
                            const unsigned char ch =
                                grid[(static_cast<size_t>(2 * z + dz) * sy + 2 * y + dy) * sx + 2 * x + dx];
                            if (rank[ch] > rank[best]) best = ch;
                        }
                out[o] = best;
            }
    return out;
}

}  // namespace

std::vector<unsigned char> decode_voxel_cells(int sx, int sy, int sz, const std::string& cells) {
    if (sx < 1 || sy < 1 || sz < 1 || sx > kMaxVoxelSide || sy > kMaxVoxelSide || sz > kMaxVoxelSide)
        throw std::runtime_error("voxels: box sides from 1 to 256");
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
    return grid;
}

void check_voxel_cells(int sx, int sy, int sz, const std::string& cells) {
    if (sx < 1 || sy < 1 || sz < 1 || sx > kMaxVoxelSide || sy > kMaxVoxelSide || sz > kMaxVoxelSide)
        throw std::runtime_error("voxels: box sides from 1 to 256");
    const size_t total = static_cast<size_t>(sx) * sy * sz;
    size_t filled = 0, count = 0;
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
        filled += counting ? count : 1;
        if (filled > total) throw std::runtime_error("voxels: more cells than the box holds");
        count = 0;
        counting = false;
    }
    if (counting || filled != total) throw std::runtime_error("voxels: cells do not fill the box");
}

VoxelMesh build_voxel_mesh(int sx, int sy, int sz, const std::string& cells, const std::array<ImU32, 128>& colors,
    const std::array<ImU32, 128>& marks, bool levels) {
    std::vector<unsigned char> grid = decode_voxel_cells(sx, sy, sz, cells);
    VoxelMesh mesh = mesh_of_grid(sx, sy, sz, grid, colors, marks, true);
    if (!levels || std::max({sx, sy, sz}) <= kVoxelLevelsFrom) return mesh;
    // Rank: a drawn character above every other, the rarer the higher.
    size_t counts[128] = {};
    for (const unsigned char ch : grid) ++counts[ch];
    int rank[128];
    for (int ch = 0; ch < 128; ++ch) {
        rank[ch] = 0;
        if (!drawn(colors, static_cast<unsigned char>(ch)) || counts[ch] == 0) continue;
        int rarer = 0;
        for (int other = 0; other < 128; ++other)
            if (other != ch && counts[other] > 0 && drawn(colors, static_cast<unsigned char>(other)) &&
                (counts[other] < counts[ch] || (counts[other] == counts[ch] && other < ch)))
                ++rarer;
        rank[ch] = 128 - rarer;
    }
    int x = sx, y = sy, z = sz;
    for (int level = 1; level <= kVoxelLevels; ++level) {
        int hx, hy, hz;
        grid = halve(x, y, z, grid, rank, hx, hy, hz);
        x = hx;
        y = hy;
        z = hz;
        auto coarse = std::make_shared<VoxelMesh>(mesh_of_grid(x, y, z, grid, colors, marks, false));
        coarse->scale = 1 << level;
        mesh.coarser.push_back(std::move(coarse));
    }
    return mesh;
}

}  // namespace overlay
