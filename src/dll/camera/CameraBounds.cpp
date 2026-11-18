#include "camera/CameraBounds.hpp"

#include <algorithm>
#include <cmath>

#include "camera/CameraAddresses.hpp"
#include "view/ViewMath.hpp"

namespace
{
    constexpr float kGameEyeDistance = ViewMath::kEyeDistance;  // camera +0x120
    constexpr float kGameMinCitySize = 4096.0f;   // the clip formula never uses a smaller city
    constexpr float kGameNearFactor = 0.95f;
    constexpr float kGameFarFactor = 1.5f;
    constexpr float kFarPad = 64.0f;
    constexpr double kCollinearSine = 1e-5;
    constexpr float kSamePoint = 1e-3f;

    // (b - a) x (c - a), divided by the two lengths: the sine of the turn at a.
    double TurnSine(const cS3DVector2& a, const cS3DVector2& b, const cS3DVector2& c)
    {
        const double abx = static_cast<double>(b.fX) - a.fX, aby = static_cast<double>(b.fY) - a.fY;
        const double acx = static_cast<double>(c.fX) - a.fX, acy = static_cast<double>(c.fY) - a.fY;
        const double lengths = std::sqrt((abx * abx + aby * aby) * (acx * acx + acy * acy));
        return lengths > 0.0 ? (abx * acy - aby * acx) / lengths : 0.0;
    }

    CameraBounds::RectXZ Union(const CameraBounds::RectXZ& a, const CameraBounds::RectXZ& b)
    {
        return {std::min(a.minX, b.minX), std::min(a.minZ, b.minZ), std::max(a.maxX, b.maxX),
                std::max(a.maxZ, b.maxZ), true};
    }
}

namespace CameraBounds
{
    RectXZ RegionExtent(const std::vector<CityInfo>& cities, const CityInfo& current)
    {
        RectXZ extent;
        for (const CityInfo& city : cities) {
            const cS3DVector3 origin = RegionLayout::OriginOffset(current, city);
            const float unit = ViewMath::kRegionUnitMetres;
            const RectXZ rect{origin.fX, origin.fZ, origin.fX + static_cast<float>(city.sizeX) * unit,
                              origin.fZ + static_cast<float>(city.sizeZ) * unit, true};
            extent = extent.valid ? Union(extent, rect) : rect;
        }
        return extent;
    }

    RectXZ PolicyRect(const RectXZ& regionExtent, const float citySizeX, const float citySizeZ, const int zoomIndex,
                      const float bandMetres)
    {
        const RectXZ city{0.0f, 0.0f, citySizeX, citySizeZ, true};
        if (!regionExtent.valid) {
            return city;
        }
        if (zoomIndex <= 2) {
            return Union(regionExtent, city);
        }
        const float band = std::max(bandMetres, 0.0f);
        const RectXZ banded{std::max(regionExtent.minX, -band), std::max(regionExtent.minZ, -band),
                            std::min(regionExtent.maxX, citySizeX + band),
                            std::min(regionExtent.maxZ, citySizeZ + band), true};
        return Union(banded, city);
    }

    int ConvexHull(cS3DVector2* points, int count, cS3DVector2* out)
    {
        std::sort(points, points + count, [](const cS3DVector2& a, const cS3DVector2& b) {
            return a.fX < b.fX || (a.fX == b.fX && a.fY < b.fY);
        });
        int unique = 0;
        for (int i = 0; i < count; ++i) {
            if (unique == 0 || std::fabs(points[i].fX - points[unique - 1].fX) > kSamePoint ||
                std::fabs(points[i].fY - points[unique - 1].fY) > kSamePoint) {
                points[unique++] = points[i];
            }
        }
        count = unique;
        if (count < 3) {
            std::copy(points, points + count, out);
            return count;
        }
        // Andrew's monotone chain: the lower side left to right, then the upper side back.
        int k = 0;
        for (int i = 0; i < count; ++i) {
            while (k >= 2 && TurnSine(out[k - 2], out[k - 1], points[i]) <= kCollinearSine) {
                --k;
            }
            out[k++] = points[i];
        }
        const int lowerEnd = k + 1;
        for (int i = count - 2; i >= 0; --i) {
            while (k >= lowerEnd && TurnSine(out[k - 2], out[k - 1], points[i]) <= kCollinearSine) {
                --k;
            }
            out[k++] = points[i];
        }
        return k - 1;  // the last point repeats the first
    }

    float RaiseTerm(const float eyeDistance, const float pitchRadians, const float metresPerPixel,
                    const int viewportHeight)
    {
        // eyeOffset.y = eyeDistance * tan(pitch), up.y = cos(pitch).
        return CameraAddresses::kScrollRaiseFactor * eyeDistance * std::tan(pitchRadians) / std::cos(pitchRadians) -
               metresPerPixel * static_cast<float>(viewportHeight) * 0.5f;
    }

    int BuildScrollPolygon(const cS3DVector2* planePoints, const int count, float raise, const float sideMargin,
                           const int capacity, cS3DVector2* out, bool& usedBox)
    {
        usedBox = false;
        if (count < 3 || count > kMaxPlanePoints || capacity < 4) {
            return 0;
        }
        cS3DVector2 input[kMaxPlanePoints];
        cS3DVector2 hull[kMaxPlanePoints + 1];
        std::copy(planePoints, planePoints + count, input);
        const int hullCount = ConvexHull(input, count, hull);
        if (hullCount < 3) {
            return 0;
        }
        raise = std::max(raise, 0.0f);

        // Every hull point and its raised copy, plus the side margins at the leftmost and
        // rightmost point. The hull of that set is the polygon.
        cS3DVector2 grown[kMaxScrollPoints];
        cS3DVector2 result[kMaxScrollPoints + 1];
        int left = 0, right = 0, n = 0;
        for (int i = 0; i < hullCount; ++i) {
            if (hull[i].fX < hull[left].fX) {
                left = i;
            }
            if (hull[i].fX > hull[right].fX) {
                right = i;
            }
            grown[n++] = hull[i];
            grown[n++] = {hull[i].fX, hull[i].fY + raise};
        }
        grown[n++] = {hull[left].fX - sideMargin, hull[left].fY};
        grown[n++] = {hull[left].fX - sideMargin, hull[left].fY + raise};
        grown[n++] = {hull[right].fX + sideMargin, hull[right].fY};
        grown[n++] = {hull[right].fX + sideMargin, hull[right].fY + raise};
        const int resultCount = ConvexHull(grown, n, result);
        if (resultCount < 3) {
            return 0;
        }
        if (resultCount <= capacity) {
            std::copy(result, result + resultCount, out);
            return resultCount;
        }

        usedBox = true;
        float minX = result[0].fX, maxX = minX, minY = result[0].fY, maxY = minY;
        for (int i = 1; i < resultCount; ++i) {
            minX = std::min(minX, result[i].fX);
            maxX = std::max(maxX, result[i].fX);
            minY = std::min(minY, result[i].fY);
            maxY = std::max(maxY, result[i].fY);
        }
        out[0] = {minX, minY};
        out[1] = {maxX, minY};
        out[2] = {maxX, maxY};
        out[3] = {minX, maxY};
        return 4;
    }

    ClipPlanes GameClip(const float pitchRadians, const float citySizeX, const float citySizeZ,
                        const float eyeDistance)
    {
        const float c = std::cos(pitchRadians);
        const float sx = std::max(kGameMinCitySize, citySizeX);
        const float sz = std::max(kGameMinCitySize, citySizeZ);
        const float slant = eyeDistance / c;
        return {eyeDistance, (slant - std::sqrt(sx * sx + sz * sz) / c) * kGameNearFactor, slant * kGameFarFactor};
    }

    float DepthFromEyePlane(const float eyeDistance, const float pitchRadians, const float g, const float h)
    {
        return eyeDistance / std::cos(pitchRadians) - g * std::cos(pitchRadians) - h * std::sin(pitchRadians);
    }

    ClipPlanes ComputeClip(const ClipInput& input)
    {
        const float c = std::cos(input.pitchRadians);
        const float s = std::sin(input.pitchRadians);
        const float extra = std::max(input.extraMetres, 0.0f);
        // The nearest and the farthest ground the viewport can show while the target is
        // inside the bounds: half a viewport past the bounds, plus the raise on the far side.
        const float gNear = input.gMax + input.viewportHalfHeight / s;
        const float gFar = input.gMin - (std::max(input.raise, 0.0f) + input.viewportHalfHeight) / s;

        // Depth of the nearest point is eyeDistance / c - gNear * c - height * s. Move the eye
        // plane back when that would drop below kMinNear. The same condition keeps the eye
        // behind every target inside the bounds (the k factor of UpdateCameraPositionParams
        // stays positive).
        const float needed = c * (gNear * c + kHeightAllowance * s + kMinNear + extra);
        const float eyeDistance = std::max(kGameEyeDistance, needed);
        const float shift = (eyeDistance - kGameEyeDistance) / c;

        const ClipPlanes game = GameClip(input.pitchRadians, input.citySizeX, input.citySizeZ, kGameEyeDistance);
        const float nearNeeded = DepthFromEyePlane(eyeDistance, input.pitchRadians, gNear, kHeightAllowance);
        const float farNeeded = DepthFromEyePlane(eyeDistance, input.pitchRadians, gFar, 0.0f) + kFarPad;

        ClipPlanes planes;
        planes.eyeDistance = eyeDistance;
        planes.nearClip = std::max(kMinNear, std::min(game.nearClip + shift, nearNeeded) - extra);
        planes.farClip = std::max(game.farClip + shift, farNeeded) + extra;
        return planes;
    }
}
