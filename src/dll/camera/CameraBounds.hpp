#pragma once

// The pure math of the camera module: where the camera may scroll, the polygon the game's
// scroll clamp needs, the clip planes that fit it, and which edge walls to hide. No game
// calls, unit tested (tests/CameraBoundsTests.cpp). The reverse engineering behind every
// formula is in docs/re-notes/camera.md.

#include <cstdint>
#include <vector>

#include "cS3DVector2.h"
#include "region/RegionLayout.hpp"

namespace CameraBounds
{
    // Edge bits of CameraExtender::SetEdgeWallsHidden, in world terms of the current city.
    constexpr uint32_t kEdgeXMin = 1u << 0;  // x = 0, the west edge
    constexpr uint32_t kEdgeXMax = 1u << 1;  // x = city size, the east edge
    constexpr uint32_t kEdgeZMin = 1u << 2;  // z = 0, the north edge
    constexpr uint32_t kEdgeZMax = 1u << 3;  // z = city size, the south edge
    constexpr uint32_t kAllEdges = 0xF;

    // The two edges that face away from the camera in a rotation: the ones through
    // ViewMath::BaseCorner. The game draws no cliffs there, only water walls.
    [[nodiscard]] constexpr uint32_t BackEdges(const int rotation)
    {
        constexpr uint32_t kBack[4] = {kEdgeXMin | kEdgeZMin, kEdgeXMax | kEdgeZMin, kEdgeXMax | kEdgeZMax,
                                       kEdgeXMin | kEdgeZMax};
        return kBack[rotation & 3];
    }

    // Most points SetBounds keeps, and the scratch sizes that follow from it.
    constexpr int kMaxBoundsPoints = 16;
    constexpr int kMaxPlanePoints = kMaxBoundsPoints + 4;        // plus the city corners
    constexpr int kMaxScrollPoints = 2 * kMaxPlanePoints + 4;    // raised copies and side margins

    // Terrain and buildings up to this height stay inside the near plane.
    constexpr float kHeightAllowance = 4096.0f;
    // The near plane never comes closer to the eye plane than this.
    constexpr float kMinNear = 100.0f;

    // A rectangle on the ground, in world metres of the current city.
    struct RectXZ
    {
        float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        bool valid = false;
    };

    // The union of every city rectangle of the region, founded or not.
    [[nodiscard]] RectXZ RegionExtent(const std::vector<CityInfo>& cities, const CityInfo& current);

    // Where the view target may go. Zoom index 0..2: the region extent. Zoom index 3 and 4:
    // the city grown by bandMetres, clamped to the region extent. Always contains the city.
    // Without a valid region extent it is the city itself.
    [[nodiscard]] RectXZ PolicyRect(const RectXZ& regionExtent, float citySizeX, float citySizeZ, int zoomIndex,
                                    float bandMetres);

    // Convex hull, counter-clockwise with x right and y up. Drops duplicates and points that
    // are (almost) on an edge. Sorts `points` in place. `out` must hold count + 1 points.
    // Returns the number of hull points.
    int ConvexHull(cS3DVector2* points, int count, cS3DVector2* out);

    // The raise term of cSC4CameraControl::UpdateScrollBounds: how far above the top corner
    // the target may go, in camera plane metres. Can be negative for a very large viewport.
    [[nodiscard]] float RaiseTerm(float eyeDistance, float pitchRadians, float metresPerPixel, int viewportHeight);

    // Builds the scroll polygon from the wanted bounds in camera plane space, with the game's
    // conventions: counter-clockwise (y up), convex, the upper side raised by `raise`
    // (negative counts as 0), the leftmost and rightmost point moved out by `sideMargin`.
    // For the four city corners this is exactly the game's own six points.
    // `out` must hold kMaxScrollPoints. When the result has more than `capacity` points, it
    // is the bounding box (4 points) and usedBox is set. Returns the point count, or 0 when
    // the input is degenerate or capacity is below 4.
    int BuildScrollPolygon(const cS3DVector2* planePoints, int count, float raise, float sideMargin, int capacity,
                           cS3DVector2* out, bool& usedBox);

    struct ClipInput
    {
        float pitchRadians = 0.0f;
        float citySizeX = 0.0f;  // camera +0x158
        float citySizeZ = 0.0f;  // camera +0x15c
        // The bounds along the horizontal direction towards the eye, in metres from the base
        // corner. The city itself spans 0 .. about 1.31 * size.
        float gMin = 0.0f;
        float gMax = 0.0f;
        float viewportHalfHeight = 0.0f;  // metres: metres per pixel * viewport height / 2
        float raise = 0.0f;               // RaiseTerm
        float extraMetres = 0.0f;         // CameraExtender::SetClipRange
    };

    struct ClipPlanes
    {
        float eyeDistance = 0.0f;  // camera +0x120
        float nearClip = 0.0f;
        float farClip = 0.0f;
    };

    // What cSC4CameraControl::UpdateCameraZoomAndRotationParams computes.
    [[nodiscard]] ClipPlanes GameClip(float pitchRadians, float citySizeX, float citySizeZ, float eyeDistance);

    // Clip planes and eye distance that keep everything the viewport can show inside
    // [near, far] while the target is inside the bounds. Never tighter than the game's
    // planes. The eye distance only grows when the near plane would otherwise come within
    // kMinNear of the eye plane, which is also when the eye would pass the target.
    [[nodiscard]] ClipPlanes ComputeClip(const ClipInput& input);

    // Depth of a ground point along the view direction, measured from the eye plane.
    // g is its distance from the base corner towards the eye (horizontal), h its height.
    [[nodiscard]] float DepthFromEyePlane(float eyeDistance, float pitchRadians, float g, float h);

    // The edges of `current` that have a neighbor city for which hasRender is true.
    template <typename Pred>
    [[nodiscard]] uint32_t NeighborEdgeMask(const std::vector<CityInfo>& cities, const CityInfo& current,
                                            Pred&& hasRender)
    {
        uint32_t mask = 0;
        const auto check = [&](const int cellX, const int cellZ, const uint32_t bit) {
            if (mask & bit) {
                return;
            }
            const CityInfo* city = RegionLayout::FindAtCell(cities, cellX, cellZ);
            if (city && city != &current && city->founded && hasRender(*city)) {
                mask |= bit;
            }
        };
        for (int z = current.cellZ; z < current.cellZ + current.sizeZ; ++z) {
            check(current.cellX - 1, z, kEdgeXMin);
            check(current.cellX + current.sizeX, z, kEdgeXMax);
        }
        for (int x = current.cellX; x < current.cellX + current.sizeX; ++x) {
            check(x, current.cellZ - 1, kEdgeZMin);
            check(x, current.cellZ + current.sizeZ, kEdgeZMax);
        }
        return mask;
    }

    // Public edge bits to the terrain view's edge index bits (0 = z max, 1 = x max, 2 = z 0,
    // 3 = x 0).
    [[nodiscard]] constexpr uint32_t ToGameEdgeMask(const uint32_t edgeMask)
    {
        return ((edgeMask & kEdgeZMax) ? 1u : 0u) | ((edgeMask & kEdgeXMax) ? 2u : 0u) |
               ((edgeMask & kEdgeZMin) ? 4u : 0u) | ((edgeMask & kEdgeXMin) ? 8u : 0u);
    }
}
