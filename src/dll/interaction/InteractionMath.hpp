#pragma once

// Interaction (W3-F): the pure parts of the neighbor picker. No game calls, unit tested in
// tests/InteractionTests.cpp.
//
// Coordinates are world metres of the current city: x and z along the ground, y up, the
// city covers [0, sizeX] x [0, sizeZ]. Edge names follow the camera module: north is z = 0,
// south is z = sizeZ, west is x = 0, east is x = sizeX.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cS3DVector3.h"
#include "region/RegionLayout.hpp"

namespace InteractionMath {
// The ground height outside the city: the terrain height of the nearest point on the
// city edge. Sampled once per city load at every edge vertex (one sample per `step`
// metres), linear between samples.
struct EdgeProfile {
    float sizeX = 0.0f;
    float sizeZ = 0.0f;
    float step = 16.0f;
    std::vector<float> north; // z = 0, along x, sizeX / step + 1 samples
    std::vector<float> south; // z = sizeZ, along x
    std::vector<float> west;  // x = 0, along z, sizeZ / step + 1 samples
    std::vector<float> east;  // x = sizeX, along z
    float mean = 0.0f;        // average of all samples, the first guess of a pick

    void Reset(const float cityX, const float cityZ, const float sampleStep, const float height) {
        sizeX = cityX;
        sizeZ = cityZ;
        step = sampleStep;
        const auto countX = static_cast<size_t>(std::lround(cityX / sampleStep)) + 1;
        const auto countZ = static_cast<size_t>(std::lround(cityZ / sampleStep)) + 1;
        north.assign(countX, height);
        south.assign(countX, height);
        west.assign(countZ, height);
        east.assign(countZ, height);
        mean = height;
    }

    void UpdateMean() {
        double sum = 0.0;
        size_t count = 0;
        for (const std::vector<float>* edge : {&north, &south, &west, &east}) {
            for (const float h : *edge) {
                sum += h;
                ++count;
            }
        }
        mean = count ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
    }

    // Height at distance t along an edge.
    [[nodiscard]] float Sample(const std::vector<float>& edge, const float t) const {
        if (edge.empty()) {
            return mean;
        }
        const float u = std::clamp(t / step, 0.0f, static_cast<float>(edge.size() - 1));
        const auto i = std::min(static_cast<size_t>(u), edge.size() - 1);
        if (i + 1 >= edge.size()) {
            return edge[i];
        }
        const float f = u - static_cast<float>(i);
        return edge[i] + (edge[i + 1] - edge[i]) * f;
    }

    // The height of the nearest edge point. Inside the city: the nearest edge.
    [[nodiscard]] float Height(const float x, const float z) const {
        const float cx = std::clamp(x, 0.0f, sizeX);
        const float cz = std::clamp(z, 0.0f, sizeZ);
        if (x < 0.0f) {
            return Sample(west, cz);
        }
        if (x > sizeX) {
            return Sample(east, cz);
        }
        if (z < 0.0f) {
            return Sample(north, cx);
        }
        if (z > sizeZ) {
            return Sample(south, cx);
        }
        const float dw = x, de = sizeX - x, dn = z, ds = sizeZ - z;
        const float best = std::min({dw, de, dn, ds});
        if (best == dw) {
            return Sample(west, z);
        }
        if (best == de) {
            return Sample(east, z);
        }
        return best == dn ? Sample(north, x) : Sample(south, x);
    }
};

[[nodiscard]] inline bool IsInsideCity(const float x, const float z, const float sizeX, const float sizeZ) {
    return x >= 0.0f && x < sizeX && z >= 0.0f && z < sizeZ;
}

// The ground point under a screen position. toGround(h) gives the point at height h that
// projects to the position (CameraReader::ScreenToGround). The height follows the edge
// profile at the point found, so the answer is exact on the edge line and off by about
// (true height - edge height) / tan(pitch) deeper inside a neighbor.
template <typename ToGround>
[[nodiscard]] cS3DVector3 PickGround(ToGround&& toGround, const EdgeProfile& profile, const int iterations = 4) {
    cS3DVector3 p = toGround(profile.mean);
    for (int i = 0; i < iterations; ++i) {
        p = toGround(profile.Height(p.fX, p.fZ));
    }
    return p;
}

// The neighbor city under a world point of the current city, or nullptr inside the
// current city and over empty region cells.
[[nodiscard]] inline const CityInfo* NeighborAt(const std::vector<CityInfo>& cities, const CityInfo& current,
                                                const float x, const float z) {
    const float sizeX = static_cast<float>(current.sizeX) * 1024.0f;
    const float sizeZ = static_cast<float>(current.sizeZ) * 1024.0f;
    if (IsInsideCity(x, z, sizeX, sizeZ)) {
        return nullptr;
    }
    int cellX = 0, cellZ = 0;
    RegionLayout::WorldToCell(current, x, z, cellX, cellZ);
    const CityInfo* city = RegionLayout::FindAtCell(cities, cellX, cellZ);
    return city && !city->isCurrent ? city : nullptr;
}

// The game's own drag test (cSC4View3DWin mouse move): a move of 4 pixels or more on
// either axis is a drag, not a click.
constexpr int kDragPixels = 4;

[[nodiscard]] inline bool IsDrag(const int x0, const int z0, const int x1, const int z1) {
    return std::abs(x1 - x0) >= kDragPixels || std::abs(z1 - z0) >= kDragPixels;
}

// "Rendered 3 days ago" for a render that is `seconds` old. Writes into out.
inline void RenderAgeText(const int64_t seconds, char* out, const size_t size) {
    struct Unit {
        int64_t seconds;
        const char* one;
        const char* many;
    };
    static constexpr Unit kUnits[] = {{86400, "day", "days"}, {3600, "hour", "hours"}, {60, "minute", "minutes"}};
    for (const Unit& unit : kUnits) {
        if (seconds >= unit.seconds) {
            const int64_t count = seconds / unit.seconds;
            std::snprintf(out, size, "Rendered %lld %s ago", static_cast<long long>(count),
                          count == 1 ? unit.one : unit.many);
            return;
        }
    }
    std::snprintf(out, size, "Rendered just now");
}

// The ground footprint of a neighbor (world rectangle [x0, x1] x [z0, z1] outside the
// city) on the same surface PickGround uses, as triangles. emit(x, y, z) is called three
// times per triangle. Next to an edge the surface is split at every edge sample, so it
// follows the edge profile. Corner areas are flat at the corner height.
template <typename Emit>
void Footprint(const EdgeProfile& profile, const float x0, const float z0, const float x1, const float z1,
               Emit&& emit) {
    auto quad = [&](const float ax, const float az, const float ha, const float bx, const float bz, const float hb,
                    const bool alongX) {
        // alongX: a strip from x = ax to bx, height ha at ax and hb at bx, over z in [az, bz].
        // Otherwise a strip from z = az to bz, height ha at az and hb at bz, over x in [ax, bx].
        const float h00 = ha, h10 = alongX ? hb : ha, h01 = alongX ? ha : hb, h11 = hb;
        emit(ax, h00, az);
        emit(bx, h10, az);
        emit(bx, h11, bz);
        emit(ax, h00, az);
        emit(bx, h11, bz);
        emit(ax, h01, bz);
    };

    const float sx = profile.sizeX, sz = profile.sizeZ;
    const float xs[4] = {x0, std::clamp(0.0f, x0, x1), std::clamp(sx, x0, x1), x1};
    const float zs[4] = {z0, std::clamp(0.0f, z0, z1), std::clamp(sz, z0, z1), z1};
    for (int j = 0; j < 3; ++j) {
        const float za = zs[j], zb = zs[j + 1];
        if (!(zb > za)) {
            continue;
        }
        const float cz = (za + zb) * 0.5f;
        const bool zInside = cz > 0.0f && cz < sz;
        for (int i = 0; i < 3; ++i) {
            const float xa = xs[i], xb = xs[i + 1];
            if (!(xb > xa)) {
                continue;
            }
            const float cx = (xa + xb) * 0.5f;
            const bool xInside = cx > 0.0f && cx < sx;
            if (xInside && zInside) {
                continue; // the current city itself
            }
            if (!xInside && !zInside) {
                const float h = profile.Height(cx, cz);
                quad(xa, za, h, xb, zb, h, true);
                continue;
            }
            // A band next to one edge: split at the edge samples.
            const bool alongX = xInside;
            const float from = alongX ? xa : za;
            const float to = alongX ? xb : zb;
            float a = from;
            while (a < to) {
                const float next = std::min(to, (std::floor(a / profile.step + 1e-4f) + 1.0f) * profile.step);
                const float ha = alongX ? profile.Height(a, cz) : profile.Height(cx, a);
                const float hb = alongX ? profile.Height(next, cz) : profile.Height(cx, next);
                if (alongX) {
                    quad(a, za, ha, next, zb, hb, true);
                } else {
                    quad(xa, a, ha, xb, next, hb, false);
                }
                a = next;
            }
        }
    }
}
} // namespace InteractionMath
