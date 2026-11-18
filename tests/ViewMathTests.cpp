#include <algorithm>
#include <cmath>

#include "TestRunner.hpp"
#include "view/ViewMath.hpp"

using namespace ViewMath;

namespace
{
    // A spread of world points: corners, the middle, heights, outside the city.
    const cS3DVector3 kPoints[] = {
        {0.0f, 0.0f, 0.0f},      {1024.0f, 0.0f, 0.0f},   {0.0f, 0.0f, 1024.0f},  {4096.0f, 250.0f, 4096.0f},
        {2048.0f, 37.5f, 513.0f}, {-1024.0f, 0.0f, 300.0f}, {5000.0f, 900.0f, -2000.0f},
    };

    bool IsWhole(const double v, const double tolerance = 0.01)
    {
        return std::fabs(v - std::round(v)) <= tolerance;
    }
}

TEST_CASE(ZoomTableMatchesGameTables)
{
    // 0x00abace0 and 0x00abaccc, pixels per metre = cell pixels / (16 * (cos 22.5 + sin 22.5)).
    const int cells[] = {8, 16, 32, 73, 146};
    const float pitch[] = {30.0f, 35.0f, 40.0f, 45.0f, 45.0f};
    const double ppm[] = {0.382683, 0.765367, 1.530734, 3.491986, 6.983973};
    CHECK_NEAR(kOrthoCellWidth, 16.0 * (std::cos(3.14159265358979 / 8.0) + std::sin(3.14159265358979 / 8.0)), 1e-6);
    for (int z = 0; z < kZoomCount; ++z) {
        CHECK(Zoom(z).cellPixels == cells[z]);
        CHECK_NEAR(Zoom(z).pitchDeg, pitch[z], 1e-4);
        CHECK_NEAR(Zoom(z).pixelsPerMetre, ppm[z], 1e-5);
        CHECK_NEAR(OrthoProjection::For(z, 0).pixelsPerMetre, cells[z] / kOrthoCellWidth, 1e-5);
    }
    CHECK_NEAR(OrthoProjection::For(4, 0, 2.0f).pixelsPerMetre, 2.0 * 146.0 / kOrthoCellWidth, 1e-4);
}

TEST_CASE(YawFollowsRotation)
{
    const double degrees = 180.0 / 3.14159265358979323846;
    // (rotation + 1) * 90 - 22.5: the view is trimetric.
    CHECK_NEAR(YawRadians(0) * degrees, 67.5, 1e-4);
    CHECK_NEAR(YawRadians(1) * degrees, 157.5, 1e-4);
    CHECK_NEAR(YawRadians(2) * degrees, 247.5, 1e-4);
    CHECK_NEAR(YawRadians(3) * degrees, 337.5, 1e-4);
}

TEST_CASE(KnownValuesZoom5Rotation0)
{
    // yaw 67.5, pitch 45: right = (sin 67.5, 0, -cos 67.5), up = (-sin45 cos67.5, cos45, -sin45 sin67.5).
    // A 1024 m step along x is 146 * 64 / sqrt(2) px across: sin 67.5 / (cos 22.5 + sin 22.5) = 1 / sqrt(2).
    const auto p = OrthoProjection::For(4, 0);
    CHECK_NEAR(p.right.fX, 0.9238795, 1e-6);
    CHECK_NEAR(p.right.fY, 0.0, 1e-6);
    CHECK_NEAR(p.right.fZ, -0.3826834, 1e-6);
    CHECK_NEAR(p.up.fX, -0.2705981, 1e-6);
    CHECK_NEAR(p.up.fY, 0.7071068, 1e-6);
    CHECK_NEAR(p.up.fZ, -0.6532815, 1e-6);
    CHECK_NEAR(p.forward.fX, -0.2705981, 1e-6);
    CHECK_NEAR(p.forward.fY, -0.7071068, 1e-6);
    CHECK_NEAR(p.forward.fZ, -0.6532815, 1e-6);

    const cS3DVector2 x = p.WorldToCanvas({1024.0f, 0.0f, 0.0f});
    CHECK_NEAR(x.fX, 146.0 * 64.0 / std::sqrt(2.0), 0.01);
    CHECK_NEAR(x.fX, 6607.206, 0.01);
    CHECK_NEAR(x.fY, 1935.206, 0.01);

    const cS3DVector2 z = p.WorldToCanvas({0.0f, 0.0f, 1024.0f});
    CHECK_NEAR(z.fX, -2736.794, 0.01);
    CHECK_NEAR(z.fY, 4672.0, 0.01);  // 146 * 64 * sin45 * sin67.5 / 1.30656 = 146 * 32

    // A height h moves a point up by h * cos(pitch) * ppm.
    const cS3DVector2 h = p.WorldToCanvas({0.0f, 100.0f, 0.0f});
    CHECK_NEAR(h.fX, 0.0, 1e-4);
    CHECK_NEAR(h.fY, -100.0 * std::sqrt(0.5) * 146.0 / kOrthoCellWidth, 0.01);
    CHECK_NEAR(h.fY, -493.837, 0.01);
}

TEST_CASE(KnownValuesZoom1Rotation0)
{
    // pitch 30: a 1024 m step along x is 8 * 64 / sqrt(2) px across and 74.98 px down.
    const auto p = OrthoProjection::For(0, 0);
    const cS3DVector2 x = p.WorldToCanvas({1024.0f, 0.0f, 0.0f});
    CHECK_NEAR(x.fX, 362.039, 1e-3);
    CHECK_NEAR(x.fY, 74.981, 1e-3);
    const cS3DVector2 h = p.WorldToCanvas({0.0f, 100.0f, 0.0f});
    CHECK_NEAR(h.fY, -33.141, 1e-3);
}

TEST_CASE(KnownValuesOtherRotations)
{
    // yaw 157.5: right = (0.3827, 0, 0.9239), up = (0.6533, 0.7071, -0.2706) at pitch 45.
    const auto r1 = OrthoProjection::For(4, 1);
    const cS3DVector2 a = r1.WorldToCanvas({1024.0f, 0.0f, 0.0f});
    CHECK_NEAR(a.fX, 2736.794, 0.01);
    CHECK_NEAR(a.fY, -4672.0, 0.01);
    // yaw 247.5: right = (-0.9239, 0, 0.3827).
    const auto r2 = OrthoProjection::For(4, 2);
    CHECK_NEAR(r2.WorldToCanvas({1024.0f, 0.0f, 0.0f}).fX, -6607.206, 0.01);
    CHECK_NEAR(r2.WorldToCanvas({0.0f, 0.0f, 1024.0f}).fX, 2736.794, 0.01);
    // yaw 337.5: right = (-0.3827, 0, -0.9239).
    const auto r3 = OrthoProjection::For(4, 3);
    CHECK_NEAR(r3.WorldToCanvas({0.0f, 0.0f, 1024.0f}).fX, -6607.206, 0.01);
}

TEST_CASE(CellWidthOnScreenIsTheTablePixels)
{
    // The projected screen width of one 16 m cell equals the table value at every rotation.
    for (int zoom = 0; zoom < kZoomCount; ++zoom) {
        for (int rot = 0; rot < kRotationCount; ++rot) {
            const auto p = OrthoProjection::For(zoom, rot);
            const float xs[] = {p.WorldToCanvas({0, 0, 0}).fX, p.WorldToCanvas({16, 0, 0}).fX,
                                p.WorldToCanvas({16, 0, 16}).fX, p.WorldToCanvas({0, 0, 16}).fX};
            const float width = *std::max_element(xs, xs + 4) - *std::min_element(xs, xs + 4);
            CHECK_NEAR(width, Zoom(zoom).cellPixels, 1e-3);
        }
    }
}

TEST_CASE(BaseCornerIsTheTopCornerOfTheCity)
{
    // The far corner of the city (the one the game snaps) is the top of the diamond.
    const float size = 2048.0f;
    for (int rot = 0; rot < kRotationCount; ++rot) {
        const auto p = OrthoProjection::For(2, rot);
        const float baseY = p.WorldToCanvas(BaseCorner(rot, size, size)).fY;
        for (const cS3DVector3& c : {cS3DVector3(0, 0, 0), cS3DVector3(size, 0, 0), cS3DVector3(size, 0, size),
                                     cS3DVector3(0, 0, size)}) {
            CHECK(baseY <= p.WorldToCanvas(c).fY + 1e-3f);
        }
    }
}

TEST_CASE(RoundTripWorldCanvasGround)
{
    for (int zoom = 0; zoom < kZoomCount; ++zoom) {
        for (int rot = 0; rot < kRotationCount; ++rot) {
            const auto p = OrthoProjection::For(zoom, rot);
            for (const auto& w : kPoints) {
                const cS3DVector3 back = p.CanvasToGround(p.WorldToCanvas(w), w.fY);
                CHECK_NEAR(back.fX, w.fX, 2e-3);
                CHECK_NEAR(back.fY, w.fY, 1e-6);
                CHECK_NEAR(back.fZ, w.fZ, 2e-3);
            }
        }
    }
}

TEST_CASE(NeighborOffsetIsTheProjectedOriginDifference)
{
    // A neighbor point q (its own world) is q + offset in the current city's world.
    const cS3DVector3 offsets[] = {{1024.0f, 0.0f, 0.0f}, {-4096.0f, 0.0f, 2048.0f}, {3072.0f, 0.0f, -1024.0f}};
    for (int zoom = 0; zoom < kZoomCount; ++zoom) {
        for (int rot = 0; rot < kRotationCount; ++rot) {
            const auto p = OrthoProjection::For(zoom, rot);
            for (const auto& o : offsets) {
                const cS3DVector2 shift = p.WorldToCanvas(o);
                for (const auto& q : kPoints) {
                    const cS3DVector2 own = p.WorldToCanvas(q);
                    const cS3DVector2 shifted = p.WorldToCanvas({q.fX + o.fX, q.fY + o.fY, q.fZ + o.fZ});
                    CHECK_NEAR(shifted.fX, own.fX + shift.fX, 0.01);
                    CHECK_NEAR(shifted.fY, own.fY + shift.fY, 0.01);
                }
            }
        }
    }
}

TEST_CASE(Zoom5IsZoom4AtTwiceTheSize)
{
    for (int rot = 0; rot < kRotationCount; ++rot) {
        const auto z4 = OrthoProjection::For(3, rot);
        const auto z5 = OrthoProjection::For(4, rot);
        for (const auto& w : kPoints) {
            const cS3DVector2 a = z4.WorldToCanvas(w);
            const cS3DVector2 b = z5.WorldToCanvas(w);
            CHECK_NEAR(b.fX, 2.0 * a.fX, 0.01);
            CHECK_NEAR(b.fY, 2.0 * a.fY, 0.01);
        }
    }
}

TEST_CASE(GameSnapsTheBaseCornerToAWholePixel)
{
    const float sizes[][2] = {{1024.0f, 1024.0f}, {2048.0f, 2048.0f}, {4096.0f, 4096.0f}};
    const cS3DVector3 targets[] = {{512.3f, 12.0f, 777.7f}, {1999.9f, 0.0f, 1.25f}, {3000.0f, 100.0f, 3000.0f}};
    for (int zoom = 0; zoom < kZoomCount; ++zoom) {
        for (int rot = 0; rot < kRotationCount; ++rot) {
            const auto p = OrthoProjection::For(zoom, rot);
            for (const auto& s : sizes) {
                const cS3DVector3 base = BaseCorner(rot, s[0], s[1]);
                for (const auto& t : targets) {
                    const cS3DVector2 c = GameCanvasToScreen(p, t, base, 1920.0f, 1080.0f);
                    const cS3DVector2 cb = p.WorldToCanvas(base);
                    // The base corner lands on a whole pixel.
                    CHECK(IsWhole(static_cast<double>(cb.fX) + c.fX));
                    CHECK(IsWhole(static_cast<double>(cb.fY) + c.fY));
                    // The target stays within one pixel of the viewport centre.
                    const cS3DVector2 ct = p.WorldToCanvas(t);
                    CHECK(std::fabs(ct.fX + c.fX - 960.0f) < 1.0f);
                    CHECK(std::fabs(ct.fY + c.fY - 540.0f) < 1.0f);
                    // Offset ranges as in UpdateCameraPositionParams.
                    const cS3DVector2 o = GameViewportOffset(p, t, base, 1920.0f, 1080.0f);
                    CHECK(o.fX >= 0.0f && o.fX < 1.0f);
                    CHECK(o.fY > -1.0f && o.fY <= 0.0f);
                }
            }
        }
    }
}

TEST_CASE(ShakeTweakShiftsTheScreen)
{
    const auto p = OrthoProjection::For(3, 0);
    const cS3DVector3 t{1000.0f, 0.0f, 1000.0f};
    const cS3DVector2 plain = GameCanvasToScreen(p, t, BaseCorner(0, 4096, 4096), 800, 600);
    const cS3DVector2 shaken = GameCanvasToScreen(p, t, BaseCorner(0, 4096, 4096), 800, 600, {3.0f, -2.0f});
    CHECK_NEAR(shaken.fX, plain.fX - 3.0, 1e-3);
    CHECK_NEAR(shaken.fY, plain.fY - 2.0, 1e-3);
}

TEST_CASE(TileGridCoversTheCityOnThePixelGrid)
{
    for (int zoom = 0; zoom < kZoomCount; ++zoom) {
        for (int rot = 0; rot < kRotationCount; ++rot) {
            const auto p = OrthoProjection::For(zoom, rot);
            const float sx = 2048.0f, sz = 4096.0f, minH = 0.0f, maxH = 1500.0f;
            const TileGrid grid = TileGrid::ForCity(p, sx, sz, minH, maxH, 512);
            const cS3DVector2 base = p.WorldToCanvas(BaseCorner(rot, sx, sz));
            CHECK(IsWhole(static_cast<double>(grid.originCanvas.fX) - base.fX));
            CHECK(IsWhole(static_cast<double>(grid.originCanvas.fY) - base.fY));
            CHECK(grid.tilesX > 0 && grid.tilesY > 0);
            const float right = grid.originCanvas.fX + static_cast<float>(grid.tilesX * grid.tileSize);
            const float bottom = grid.originCanvas.fY + static_cast<float>(grid.tilesY * grid.tileSize);
            for (const float h : {minH, maxH}) {
                for (const cS3DVector3& c : {cS3DVector3(0, h, 0), cS3DVector3(sx, h, 0), cS3DVector3(sx, h, sz),
                                             cS3DVector3(0, h, sz)}) {
                    const cS3DVector2 cc = p.WorldToCanvas(c);
                    CHECK(cc.fX >= grid.originCanvas.fX && cc.fX <= right);
                    CHECK(cc.fY >= grid.originCanvas.fY && cc.fY <= bottom);
                }
            }
            // The origin is less than one pixel outside the bounds.
            const TileGrid flat = TileGrid::ForCity(p, sx, sz, 0.0f, 0.0f, 512);
            CHECK(flat.originCanvas.fY > p.WorldToCanvas(BaseCorner(rot, sx, sz)).fY - 1.0f);
        }
    }
}

TEST_CASE(TileGridVisibleRange)
{
    TileGrid grid;
    grid.tileSize = 512;
    grid.originCanvas = {-1000.5f, 200.25f};
    grid.tilesX = 10;
    grid.tilesY = 6;
    int tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    CHECK(grid.VisibleTiles({-1000.5f, 200.25f}, {-1000.5f + 512.0f, 200.25f + 1.0f}, tx0, ty0, tx1, ty1));
    CHECK(tx0 == 0 && tx1 == 1 && ty0 == 0 && ty1 == 1);
    CHECK(grid.VisibleTiles({0.0f, 1000.0f}, {1920.0f, 2080.0f}, tx0, ty0, tx1, ty1));
    CHECK(tx0 == 1 && tx1 == 6 && ty0 == 1 && ty1 == 4);
    CHECK(!grid.VisibleTiles({-5000.0f, 0.0f}, {-2000.0f, 100.0f}, tx0, ty0, tx1, ty1));
    CHECK(grid.VisibleTiles({-1e6f, -1e6f}, {1e6f, 1e6f}, tx0, ty0, tx1, ty1));
    CHECK(tx0 == 0 && tx1 == 10 && ty0 == 0 && ty1 == 6);
    const cS3DVector2 o = grid.TileOrigin(2, 3);
    CHECK_NEAR(o.fX, -1000.5 + 1024.0, 1e-3);
    CHECK_NEAR(o.fY, 200.25 + 1536.0, 1e-3);
}
