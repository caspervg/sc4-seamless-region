#include <cmath>
#include <vector>

#include "TestRunner.hpp"
#include "camera/CameraBounds.hpp"
#include "view/ViewMath.hpp"

namespace
{
    using CameraBounds::RectXZ;

    CityInfo MakeCity(const int x, const int z, const int size, const bool founded = true)
    {
        CityInfo c;
        c.cellX = x;
        c.cellZ = z;
        c.sizeX = size;
        c.sizeZ = size;
        c.founded = founded;
        c.key = founded ? "city" : "cell";
        return c;
    }

    // Camera plane point of a ground point: what ConvertWorldPointToCameraPlanePoint
    // (0x007cbce0) gives, see docs/re-notes/viewmath.md.
    cS3DVector2 ToPlane(const ViewMath::OrthoProjection& p, const cS3DVector3& base, const float x, const float z)
    {
        const float dx = x - base.fX, dz = z - base.fZ;
        return {dx * p.right.fX + dz * p.right.fZ, dx * p.up.fX + dz * p.up.fZ};
    }

    int ProjectRect(const RectXZ& r, const int zoom, const int rotation, const float citySize, cS3DVector2* out)
    {
        const auto p = ViewMath::OrthoProjection::For(zoom, rotation);
        const cS3DVector3 base = ViewMath::BaseCorner(rotation, citySize, citySize);
        out[0] = ToPlane(p, base, r.minX, r.minZ);
        out[1] = ToPlane(p, base, r.maxX, r.minZ);
        out[2] = ToPlane(p, base, r.maxX, r.maxZ);
        out[3] = ToPlane(p, base, r.minX, r.maxZ);
        return 4;
    }

    float SignedArea(const cS3DVector2* poly, const int n)
    {
        float area = 0.0f;
        for (int i = 0; i < n; ++i) {
            const cS3DVector2& a = poly[i];
            const cS3DVector2& b = poly[(i + 1) % n];
            area += a.fX * b.fY - b.fX * a.fY;
        }
        return area * 0.5f;
    }

    // Distance of p inside the counter-clockwise polygon: negative when outside.
    float InsideDistance(const cS3DVector2* poly, const int n, const cS3DVector2& p)
    {
        float least = 1e30f;
        for (int i = 0; i < n; ++i) {
            const cS3DVector2& a = poly[i];
            const cS3DVector2& b = poly[(i + 1) % n];
            const float ex = b.fX - a.fX, ey = b.fY - a.fY;
            const float d = (ex * (p.fY - a.fY) - ey * (p.fX - a.fX)) / std::sqrt(ex * ex + ey * ey);
            least = std::fmin(least, d);
        }
        return least;
    }

    // Port of cSC4CameraControl::ClampScrollToBounds (0x007cc650) from the Windows decompile.
    cS3DVector2 GameClamp(const cS3DVector2* poly, const int n, const cS3DVector2 from, const cS3DVector2 to)
    {
        float fromX = from.fX, fromY = from.fY, toX = to.fX, toY = to.fY;
        bool slidForward = false, slidBack = false;
        int rounds = 0;
        while (rounds < n) {
            const cS3DVector2* hitPrev = nullptr;
            const cS3DVector2* hitCur = nullptr;
            float best = 1.0f;
            const cS3DVector2* prev = &poly[n - 1];
            for (int i = 0; i < n; ++i) {
                const cS3DVector2* cur = &poly[i];
                const float ex = prev->fX - cur->fX, ey = prev->fY - cur->fY;
                const float len = std::sqrt(ex * ex + ey * ey);
                if (len >= 1e-8f) {
                    const float nx = ey / len, ny = -ex / len;
                    const float d0 = nx * prev->fX + ny * prev->fY;
                    const float dTo = nx * toX + ny * toY - d0;
                    const float dFrom = nx * fromX + ny * fromY - d0;
                    if (dTo <= 0.0f && !(dFrom < dTo)) {
                        const float t = dFrom / (dFrom - dTo);
                        if (t < best) {
                            hitPrev = prev;
                            hitCur = cur;
                            best = t;
                        }
                    }
                }
                prev = cur;
            }
            if (!hitPrev) {
                return {toX, toY};
            }
            best = best >= 0.001f ? best - 0.0001f : 0.0f;
            const float restX = (toX - fromX) * (1.0f - best), restY = (toY - fromY) * (1.0f - best);
            fromX = toX - restX;
            fromY = toY - restY;
            float dx = hitCur->fX - hitPrev->fX, dy = hitCur->fY - hitPrev->fY;
            bool nextSlidBack = slidBack;
            if (0.0f <= (to.fX - from.fX) * dx + (to.fY - from.fY) * dy) {
                slidForward = true;
            }
            else {
                slidBack = slidForward;
                nextSlidBack = true;
            }
            if (slidBack) {
                return {fromX, fromY};
            }
            ++rounds;
            const float inv = 1.0f / std::sqrt(dx * dx + dy * dy);
            dx *= inv;
            dy *= inv;
            const float along = dx * restX + dy * restY;
            toX = dx * along + fromX;
            toY = dy * along + fromY;
            slidBack = nextSlidBack;
        }
        return from;
    }

    bool HasPoint(const cS3DVector2* poly, const int n, const float x, const float y)
    {
        for (int i = 0; i < n; ++i) {
            if (std::fabs(poly[i].fX - x) < 0.05f && std::fabs(poly[i].fY - y) < 0.05f) {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE(RegionExtentIsTheUnionOfAllCities)
{
    const std::vector<CityInfo> cities{MakeCity(0, 0, 4), MakeCity(4, 2, 2), MakeCity(6, 2, 1, false)};
    const RectXZ extent = CameraBounds::RegionExtent(cities, cities[1]);
    CHECK(extent.valid);
    CHECK_NEAR(extent.minX, -4096.0, 1e-3);
    CHECK_NEAR(extent.minZ, -2048.0, 1e-3);
    CHECK_NEAR(extent.maxX, 3072.0, 1e-3);  // the unfounded small city counts
    CHECK_NEAR(extent.maxZ, 2048.0, 1e-3);
    CHECK(!CameraBounds::RegionExtent({}, cities[1]).valid);
}

TEST_CASE(PolicyRectFollowsTheZoom)
{
    const RectXZ region{-4096.0f, -2048.0f, 3072.0f, 2048.0f, true};
    for (int zoom = 0; zoom <= 2; ++zoom) {
        const RectXZ r = CameraBounds::PolicyRect(region, 2048.0f, 2048.0f, zoom, 1024.0f);
        CHECK(r.minX == region.minX && r.minZ == region.minZ && r.maxX == region.maxX && r.maxZ == region.maxZ);
    }
    for (int zoom = 3; zoom <= 4; ++zoom) {
        const RectXZ r = CameraBounds::PolicyRect(region, 2048.0f, 2048.0f, zoom, 1500.0f);
        CHECK_NEAR(r.minX, -1500.0, 1e-3);
        CHECK_NEAR(r.minZ, -1500.0, 1e-3);
        CHECK_NEAR(r.maxX, 3072.0, 1e-3);  // 2048 + 1500 is past the region edge
        CHECK_NEAR(r.maxZ, 2048.0, 1e-3);  // the city is at the region edge
    }
    // No band: the city. A negative band counts as none.
    const RectXZ city = CameraBounds::PolicyRect(region, 2048.0f, 2048.0f, 4, -5.0f);
    CHECK(city.minX == 0.0f && city.minZ == 0.0f && city.maxX == 2048.0f && city.maxZ == 2048.0f);
    // No region: the city, at every zoom.
    const RectXZ none = CameraBounds::PolicyRect(RectXZ(), 1024.0f, 1024.0f, 0, 1024.0f);
    CHECK(none.valid && none.minX == 0.0f && none.maxX == 1024.0f && none.maxZ == 1024.0f);
}

TEST_CASE(ConvexHullDropsInnerAndEdgePoints)
{
    cS3DVector2 points[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 5}, {5, 0}, {10, 10}, {0, 5.00001f}};
    cS3DVector2 hull[9];
    const int n = CameraBounds::ConvexHull(points, 8, hull);
    CHECK(n == 4);
    CHECK(SignedArea(hull, n) > 0.0f);  // counter-clockwise
    CHECK_NEAR(SignedArea(hull, n), 100.0, 1e-3);
}

TEST_CASE(ScrollPolygonOfTheCityIsTheGamePolygon)
{
    for (int zoom = 0; zoom < ViewMath::kZoomCount; ++zoom) {
        for (int rotation = 0; rotation < ViewMath::kRotationCount; ++rotation) {
            const float size = 2048.0f;
            cS3DVector2 corners[4];
            ProjectRect({0.0f, 0.0f, size, size, true}, zoom, rotation, size, corners);
            // The game names the corners by their place on screen (GetCityCornerPosition).
            int top = 0, left = 0, right = 0, bottom = 0;
            for (int i = 1; i < 4; ++i) {
                top = corners[i].fY > corners[top].fY ? i : top;
                bottom = corners[i].fY < corners[bottom].fY ? i : bottom;
                left = corners[i].fX < corners[left].fX ? i : left;
                right = corners[i].fX > corners[right].fX ? i : right;
            }
            const float raise = CameraBounds::RaiseTerm(ViewMath::kEyeDistance, ViewMath::PitchRadians(zoom),
                                                        1.0f / ViewMath::Zoom(zoom).pixelsPerMetre, 768);
            CHECK(raise > 0.0f);
            // UpdateScrollBounds (0x007ccc60), in the order it pushes the points.
            const cS3DVector2 game[6] = {
                {corners[top].fX, corners[top].fY + raise},
                {corners[left].fX - 500.0f, corners[left].fY + raise},
                {corners[left].fX - 500.0f, corners[left].fY},
                corners[bottom],
                {corners[right].fX + 500.0f, corners[right].fY},
                {corners[right].fX + 500.0f, corners[right].fY + raise},
            };
            CHECK(SignedArea(game, 6) > 0.0f);

            cS3DVector2 polygon[CameraBounds::kMaxScrollPoints];
            bool usedBox = true;
            const int n = CameraBounds::BuildScrollPolygon(corners, 4, raise, 500.0f, 6, polygon, usedBox);
            CHECK(n == 6);
            CHECK(!usedBox);
            CHECK(SignedArea(polygon, n) > 0.0f);  // same winding as the game
            for (const cS3DVector2& p : game) {
                CHECK(HasPoint(polygon, n, p.fX, p.fY));
            }
        }
    }
}

TEST_CASE(ScrollPolygonWorksWithTheGameClamp)
{
    const float size = 4096.0f;
    const RectXZ region{-8192.0f, -4096.0f, 12288.0f, 8192.0f, true};
    for (int rotation = 0; rotation < ViewMath::kRotationCount; ++rotation) {
        const int zoom = 1;
        cS3DVector2 corners[4];
        ProjectRect(region, zoom, rotation, size, corners);
        const float raise = CameraBounds::RaiseTerm(ViewMath::kEyeDistance, ViewMath::PitchRadians(zoom),
                                                    1.0f / ViewMath::Zoom(zoom).pixelsPerMetre, 768);
        cS3DVector2 polygon[CameraBounds::kMaxScrollPoints];
        bool usedBox = false;
        const int n = CameraBounds::BuildScrollPolygon(corners, 4, raise, 500.0f, 6, polygon, usedBox);
        CHECK(n == 6);

        // The base corner is plane (0, 0). It is inside, as ClampTargetToScrollBounds needs.
        const cS3DVector2 base{0.0f, 0.0f};
        CHECK(InsideDistance(polygon, n, base) > 0.0f);
        // Every corner of the region can be reached.
        for (const cS3DVector2& corner : corners) {
            CHECK(InsideDistance(polygon, n, corner) > -0.01f);
            const cS3DVector2 almost{corner.fX * 0.999f, corner.fY * 0.999f};
            const cS3DVector2 out = GameClamp(polygon, n, base, almost);
            CHECK_NEAR(out.fX, almost.fX, 0.01);
            CHECK_NEAR(out.fY, almost.fY, 0.01);
        }
        // A move far past the bounds, in eight directions, ends on the polygon.
        for (int i = 0; i < 8; ++i) {
            const float angle = static_cast<float>(i) * 0.785398f + 0.1f;
            const cS3DVector2 far{std::cos(angle) * 100000.0f, std::sin(angle) * 100000.0f};
            const cS3DVector2 out = GameClamp(polygon, n, base, far);
            const float inside = InsideDistance(polygon, n, out);
            CHECK(inside > -1.0f);
            CHECK(inside < 50.0f);
        }
    }
}

TEST_CASE(ScrollPolygonFallsBackToTheBoundingBox)
{
    cS3DVector2 corners[4];
    ProjectRect({0.0f, 0.0f, 1024.0f, 1024.0f, true}, 2, 0, 1024.0f, corners);
    cS3DVector2 polygon[CameraBounds::kMaxScrollPoints];
    bool usedBox = false;
    const int n = CameraBounds::BuildScrollPolygon(corners, 4, 300.0f, 500.0f, 5, polygon, usedBox);
    CHECK(n == 4);
    CHECK(usedBox);
    CHECK(SignedArea(polygon, n) > 0.0f);
    for (const cS3DVector2& corner : corners) {
        CHECK(InsideDistance(polygon, n, corner) > -0.01f);
    }
    // Too little room, or not a polygon: nothing.
    CHECK(CameraBounds::BuildScrollPolygon(corners, 4, 300.0f, 500.0f, 3, polygon, usedBox) == 0);
    CHECK(CameraBounds::BuildScrollPolygon(corners, 2, 300.0f, 500.0f, 6, polygon, usedBox) == 0);
    // A negative raise (very large viewport) counts as none: left, bottom, right and top.
    const int flat = CameraBounds::BuildScrollPolygon(corners, 4, -200.0f, 500.0f, 6, polygon, usedBox);
    CHECK(flat == 4);
    CHECK(!usedBox);
}

TEST_CASE(ClipForTheCityAloneIsTheGameClip)
{
    // Zoom 5 and 4: the viewport is small against the game's own slack.
    for (int zoom = 3; zoom <= 4; ++zoom) {
        const float pitch = ViewMath::PitchRadians(zoom);
        const float mpp = 1.0f / ViewMath::Zoom(zoom).pixelsPerMetre;
        CameraBounds::ClipInput in;
        in.pitchRadians = pitch;
        in.citySizeX = in.citySizeZ = 4096.0f;
        in.gMin = 0.0f;
        in.gMax = 4096.0f * 1.3066f;
        in.viewportHalfHeight = mpp * 1080.0f * 0.5f;
        in.raise = CameraBounds::RaiseTerm(ViewMath::kEyeDistance, pitch, mpp, 1080);
        const CameraBounds::ClipPlanes game = CameraBounds::GameClip(pitch, 4096.0f, 4096.0f, ViewMath::kEyeDistance);
        const CameraBounds::ClipPlanes clip = CameraBounds::ComputeClip(in);
        CHECK(clip.eyeDistance == ViewMath::kEyeDistance);
        CHECK_NEAR(clip.nearClip, game.nearClip, 0.01);
        CHECK_NEAR(clip.farClip, game.farClip, 0.01);
    }
    // The game's numbers at 45 degrees: near (11585.2 - 8192.6) * 0.95, far 17377.9.
    const CameraBounds::ClipPlanes game = CameraBounds::GameClip(0.785398f, 1024.0f, 1024.0f, 8192.0f);
    CHECK_NEAR(game.nearClip, 3223.0, 1.0);
    CHECK_NEAR(game.farClip, 17377.9, 1.0);
}

TEST_CASE(ClipCoversALargeRegion)
{
    for (int zoom = 0; zoom < ViewMath::kZoomCount; ++zoom) {
        const float pitch = ViewMath::PitchRadians(zoom);
        const float mpp = 1.0f / ViewMath::Zoom(zoom).pixelsPerMetre;
        CameraBounds::ClipInput in;
        in.pitchRadians = pitch;
        in.citySizeX = in.citySizeZ = 1024.0f;
        in.gMin = -30000.0f;
        in.gMax = 40000.0f;
        in.viewportHalfHeight = mpp * 1440.0f * 0.5f;
        in.raise = CameraBounds::RaiseTerm(ViewMath::kEyeDistance, pitch, mpp, 1440);
        const CameraBounds::ClipPlanes clip = CameraBounds::ComputeClip(in);
        const CameraBounds::ClipPlanes game = CameraBounds::GameClip(pitch, 1024.0f, 1024.0f, ViewMath::kEyeDistance);
        const float shift = (clip.eyeDistance - ViewMath::kEyeDistance) / std::cos(pitch);

        CHECK(clip.eyeDistance > ViewMath::kEyeDistance);
        CHECK(clip.nearClip >= CameraBounds::kMinNear - 0.01f);
        // Never tighter than the game's planes (which moved back with the eye plane).
        CHECK(clip.nearClip <= game.nearClip + shift + 0.01f);
        CHECK(clip.farClip >= game.farClip + shift - 0.01f);
        // The nearest tall point and the farthest ground point the viewport can show.
        const float gNear = in.gMax + in.viewportHalfHeight / std::sin(pitch);
        const float gFar = in.gMin - (std::fmax(in.raise, 0.0f) + in.viewportHalfHeight) / std::sin(pitch);
        CHECK(CameraBounds::DepthFromEyePlane(clip.eyeDistance, pitch, gNear, CameraBounds::kHeightAllowance) >=
              clip.nearClip - 0.5f);
        CHECK(CameraBounds::DepthFromEyePlane(clip.eyeDistance, pitch, gNear, 0.0f) <= clip.farClip);
        CHECK(CameraBounds::DepthFromEyePlane(clip.eyeDistance, pitch, gFar, 0.0f) <= clip.farClip);
        // The eye stays behind the target: k of UpdateCameraPositionParams is positive.
        const float c = std::cos(pitch), s = std::sin(pitch);
        const float k = 1.0f - (in.gMax * c * c + CameraBounds::kHeightAllowance * s * c) / clip.eyeDistance;
        CHECK(k > 0.0f);

        // SetClipRange moves both planes apart.
        in.extraMetres = 2000.0f;
        const CameraBounds::ClipPlanes wider = CameraBounds::ComputeClip(in);
        CHECK(wider.farClip - wider.nearClip >= clip.farClip - clip.nearClip + 3999.0f);
        CHECK(wider.nearClip >= CameraBounds::kMinNear - 0.01f);
    }
}

TEST_CASE(NeighborEdgeMaskNeedsARenderedNeighbor)
{
    // Current city (2,2) size 2. West: rendered. East: founded, no render. North: not founded.
    // South: nothing. A corner neighbor does not count.
    std::vector<CityInfo> cities{MakeCity(2, 2, 2), MakeCity(1, 3, 1), MakeCity(4, 2, 2), MakeCity(2, 1, 1, false),
                                 MakeCity(1, 1, 1)};
    cities[1].key = "west";
    cities[4].key = "corner";
    const auto rendered = [](const CityInfo& c) { return c.key == "west" || c.key == "corner" || c.key == "cell"; };
    CHECK(CameraBounds::NeighborEdgeMask(cities, cities[0], rendered) == CameraBounds::kEdgeXMin);
    const auto all = [](const CityInfo&) { return true; };
    CHECK(CameraBounds::NeighborEdgeMask(cities, cities[0], all) ==
          (CameraBounds::kEdgeXMin | CameraBounds::kEdgeXMax));
    const auto none = [](const CityInfo&) { return false; };
    CHECK(CameraBounds::NeighborEdgeMask(cities, cities[0], none) == 0u);
}

TEST_CASE(EdgeBitsMapToTheTerrainEdges)
{
    // Terrain view edges: 0 = z max, 1 = x max, 2 = z 0, 3 = x 0 (ObtainHeightFieldInfo 0x007569f0).
    CHECK(CameraBounds::ToGameEdgeMask(CameraBounds::kEdgeZMax) == 1u);
    CHECK(CameraBounds::ToGameEdgeMask(CameraBounds::kEdgeXMax) == 2u);
    CHECK(CameraBounds::ToGameEdgeMask(CameraBounds::kEdgeZMin) == 4u);
    CHECK(CameraBounds::ToGameEdgeMask(CameraBounds::kEdgeXMin) == 8u);
    CHECK(CameraBounds::ToGameEdgeMask(CameraBounds::kAllEdges) == 0xFu);
    CHECK(CameraBounds::ToGameEdgeMask(0) == 0u);
}

TEST_CASE(BackEdgesFaceAwayFromTheEye)
{
    namespace CB = CameraBounds;
    for (int rotation = 0; rotation < ViewMath::kRotationCount; ++rotation) {
        // forward runs from the eye to the target: an eye at +x has the west edge at the back.
        const cS3DVector3 f = ViewMath::OrthoProjection::For(0, rotation).forward;
        const uint32_t expected = (f.fX < 0 ? CB::kEdgeXMin : CB::kEdgeXMax) | (f.fZ < 0 ? CB::kEdgeZMin : CB::kEdgeZMax);
        CHECK(CB::BackEdges(rotation) == expected);
    }
}
