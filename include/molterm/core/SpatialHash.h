#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace molterm {

// 3D spatial hash for fast neighbor lookup in Cartesian space.
// Shared between CifLoader (bond detection) and ContactMap (interface detection).
class SpatialHash {
public:
    SpatialHash(float cellSize, int nAtoms)
        : invCell_(1.0f / cellSize) {
        buckets_.reserve(nAtoms);
    }

    void insert(int idx, float x, float y, float z) {
        auto key = makeKey(x, y, z);
        buckets_[key].push_back(idx);
    }

    // Call func(int neighborIdx) for all atoms within radius of (x,y,z)
    template<typename Func>
    void forEachNeighbor(float x, float y, float z, float radius, const Func& func) const {
        int cx = toCell(x), cy = toCell(y), cz = toCell(z);
        int span = static_cast<int>(std::ceil(radius * invCell_));
        for (int dz = -span; dz <= span; ++dz) {
            for (int dy = -span; dy <= span; ++dy) {
                for (int dx = -span; dx <= span; ++dx) {
                    auto key = packKey(cx + dx, cy + dy, cz + dz);
                    auto it = buckets_.find(key);
                    if (it != buckets_.end()) {
                        for (int idx : it->second) {
                            func(idx);
                        }
                    }
                }
            }
        }
    }

private:
    float invCell_;
    std::unordered_map<uint64_t, std::vector<int>> buckets_;

    int toCell(float v) const { return static_cast<int>(std::floor(v * invCell_)); }

    uint64_t makeKey(float x, float y, float z) const {
        return packKey(toCell(x), toCell(y), toCell(z));
    }

    static uint64_t packKey(int x, int y, int z) {
        auto u = [](int v) -> uint64_t { return static_cast<uint64_t>(v + 10000); };
        return (u(x) * 20001ULL + u(y)) * 20001ULL + u(z);
    }
};

} // namespace molterm
