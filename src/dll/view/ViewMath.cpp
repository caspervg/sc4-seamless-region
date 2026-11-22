#include "view/ViewMath.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ViewMath
{
    namespace
    {
        // The game's float constants, bit for bit.
        constexpr std::array<uint32_t, kZoomCount> kPitchBits{
            0x3f060a92u, 0x3f1c61aau, 0x3f32b8c2u, 0x3f490fdbu, 0x3f490fdbu};
        constexpr std::array<int, kZoomCount> kCellPixels{8, 16, 32, 73, 146};
        constexpr uint32_t kYawOffsetBits = 0xbec90fdbu;   // camera +0x118, -pi/8
        constexpr uint32_t kOrthoAngleBits = 0xbec90fdbu;  // table 0x00abcfc4, -pi/8
        constexpr float kHalfPi = 1.5707964f;              // the game's literal

        double OrthoCellWidth()
        {
            // GetCityWidthFromOrthoView(16, zoom) = 16 * (cos|a| + sin|a|), a as a float.
            const double a = std::fabs(static_cast<double>(std::bit_cast<float>(kOrthoAngleBits)));
            return 16.0 * std::cos(a) + 16.0 * std::sin(a);
        }

        std::array<ZoomInfo, kZoomCount> MakeZoomTable()
        {
            std::array<ZoomInfo, kZoomCount> table{};
            const double width = OrthoCellWidth();
            for (int i = 0; i < kZoomCount; ++i) {
                const float pitch = std::bit_cast<float>(kPitchBits[i]);
                table[i].cellPixels = kCellPixels[i];
                table[i].pitchDeg = static_cast<float>(pitch * 180.0 / 3.14159265358979323846);
                table[i].pixelsPerMetre = static_cast<float>(kCellPixels[i] / width);
            }
            return table;
        }

        double Dot(const double ax, const double ay, const double az, const cS3DVector3& b)
        {
            return ax * b.fX + ay * b.fY + az * b.fZ;
        }

        double Dot(const cS3DVector3& a, const cS3DVector3& b) { return Dot(a.fX, a.fY, a.fZ, b); }
    }

    bool IsValidZoom(const int zoomIndex) { return zoomIndex >= 0 && zoomIndex < kZoomCount; }

    bool IsValidRotation(const int rotation) { return rotation >= 0 && rotation < kRotationCount; }

    const ZoomInfo& Zoom(const int zoomIndex)
    {
        static const std::array<ZoomInfo, kZoomCount> table = MakeZoomTable();
        return table[std::clamp(zoomIndex, 0, kZoomCount - 1)];
    }

    float PitchRadians(const int zoomIndex)
    {
        return std::bit_cast<float>(kPitchBits[std::clamp(zoomIndex, 0, kZoomCount - 1)]);
    }

    float YawRadians(const int rotation)
    {
        // UpdateCameraZoomAndRotationParams: ((float)rotation + 1) * 1.5707964 + camera[+0x118].
        const int r = ((rotation % kRotationCount) + kRotationCount) % kRotationCount;
        return (static_cast<float>(r) + 1.0f) * kHalfPi + std::bit_cast<float>(kYawOffsetBits);
    }

    float YawOffsetRadians() { return std::bit_cast<float>(kYawOffsetBits); }

    cS3DVector3 BaseCorner(const int rotation, const float citySizeX, const float citySizeZ)
    {
        switch (((rotation % kRotationCount) + kRotationCount) % kRotationCount) {
        case 1: return {citySizeX, 0.0f, 0.0f};
        case 2: return {citySizeX, 0.0f, citySizeZ};
        case 3: return {0.0f, 0.0f, citySizeZ};
        default: return {0.0f, 0.0f, 0.0f};
        }
    }

    OrthoProjection OrthoProjection::For(const int zoomIndex, const int rotation, const float magnification)
    {
        OrthoProjection p;
        p.zoomIndex = std::clamp(zoomIndex, 0, kZoomCount - 1);
        p.rotation = ((rotation % kRotationCount) + kRotationCount) % kRotationCount;
        p.magnification = magnification;

        // camera +0x138 = cellPixels * magnification / GetCityWidthFromOrthoView(16, zoom)
        const float scaledCell = static_cast<float>(Zoom(p.zoomIndex).cellPixels) * magnification;
        p.pixelsPerMetre = static_cast<float>(scaledCell / OrthoCellWidth());

        // Eye offset from the target (camera +0x13c): (cos yaw * d, sin pitch * (d / cos pitch), sin yaw * d)
        const double yaw = YawRadians(p.rotation);
        const double pitch = PitchRadians(p.zoomIndex);
        const float slant = static_cast<float>(kEyeDistance / std::cos(pitch));
        const double ox = static_cast<float>(std::cos(yaw) * kEyeDistance);
        const double oy = static_cast<float>(std::sin(pitch) * slant);
        const double oz = static_cast<float>(std::sin(yaw) * kEyeDistance);

        // cS3DCamera::LookAt (0x007ffd60) with up (0,1,0):
        // back = normalize(eye - target), right = normalize(cross(up, back)), up' = cross(back, right).
        const double len = std::sqrt(ox * ox + oy * oy + oz * oz);
        const double bx = ox / len, by = oy / len, bz = oz / len;
        const double rlen = std::sqrt(bz * bz + bx * bx);
        const double rx = bz / rlen, rz = -bx / rlen;
        const double ux = by * rz;
        const double uy = bz * rx - bx * rz;
        const double uz = -by * rx;

        p.right = {static_cast<float>(rx), 0.0f, static_cast<float>(rz)};
        p.up = {static_cast<float>(ux), static_cast<float>(uy), static_cast<float>(uz)};
        p.forward = {static_cast<float>(-bx), static_cast<float>(-by), static_cast<float>(-bz)};
        return p;
    }

    cS3DVector2 OrthoProjection::WorldToCanvas(const cS3DVector3& world) const
    {
        const double ppm = pixelsPerMetre;
        return {static_cast<float>(ppm * Dot(world, right)), static_cast<float>(-ppm * Dot(world, up))};
    }

    cS3DVector3 OrthoProjection::CanvasToGround(const cS3DVector2& canvas, const float groundHeight) const
    {
        // Solve for x and z with y = groundHeight:
        //   right.x * x + right.z * z = canvas.x / ppm
        //   up.x * x + up.z * z       = -canvas.y / ppm - up.y * groundHeight
        const double ppm = pixelsPerMetre;
        const double a = canvas.fX / ppm;
        const double b = -canvas.fY / ppm - static_cast<double>(up.fY) * groundHeight;
        const double det = static_cast<double>(right.fX) * up.fZ - static_cast<double>(right.fZ) * up.fX;
        if (std::fabs(det) < 1e-9) {
            return {0.0f, groundHeight, 0.0f};
        }
        const double x = (a * up.fZ - right.fZ * b) / det;
        const double z = (right.fX * b - up.fX * a) / det;
        return {static_cast<float>(x), groundHeight, static_cast<float>(z)};
    }

    cS3DVector2 GameViewportOffset(const OrthoProjection& projection, const cS3DVector3& target,
                                   const cS3DVector3& baseCorner, const float viewportWidth,
                                   const float viewportHeight, const cS3DVector2& tweak)
    {
        // UpdateCameraPositionParams: project the base corner with a zero offset, then shift
        // the viewport so that it lands on floor() of that position.
        const cS3DVector2 cb = projection.WorldToCanvas(baseCorner);
        const cS3DVector2 ct = projection.WorldToCanvas(target);
        const double px = viewportWidth * 0.5 + (static_cast<double>(cb.fX) - ct.fX);
        const double py = viewportHeight * 0.5 + (static_cast<double>(cb.fY) - ct.fY);
        const double ox = (px - std::floor(px)) + tweak.fX;
        const double oy = -(py - std::floor(py)) + tweak.fY;
        return {static_cast<float>(ox), static_cast<float>(oy)};
    }

    cS3DVector2 GameCanvasToScreen(const OrthoProjection& projection, const cS3DVector3& target,
                                   const cS3DVector3& baseCorner, const float viewportWidth,
                                   const float viewportHeight, const cS3DVector2& tweak)
    {
        // cS3DCamera::Project with an ortho volume of +-W/2, +-H/2 pixels and viewport offset o:
        //   screen.x = W/2 + ppm * dot(p - target, right) - o.x
        //   screen.y = H/2 - ppm * dot(p - target, up)    + o.y
        const cS3DVector2 offset =
            GameViewportOffset(projection, target, baseCorner, viewportWidth, viewportHeight, tweak);
        const cS3DVector2 ct = projection.WorldToCanvas(target);
        return {static_cast<float>(viewportWidth * 0.5 - offset.fX - ct.fX),
                static_cast<float>(viewportHeight * 0.5 + offset.fY - ct.fY)};
    }

    TileGrid TileGrid::ForCity(const OrthoProjection& projection, const float citySizeX, const float citySizeZ,
                               const float minHeight, const float maxHeight, const int tileSize)
    {
        TileGrid grid;
        grid.tileSize = std::max(1, tileSize);

        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
        for (const float h : {minHeight, maxHeight}) {
            for (const cS3DVector3& corner : {cS3DVector3(0.0f, h, 0.0f), cS3DVector3(citySizeX, h, 0.0f),
                                              cS3DVector3(citySizeX, h, citySizeZ), cS3DVector3(0.0f, h, citySizeZ)}) {
                const cS3DVector2 c = projection.WorldToCanvas(corner);
                minX = std::min(minX, static_cast<double>(c.fX));
                minY = std::min(minY, static_cast<double>(c.fY));
                maxX = std::max(maxX, static_cast<double>(c.fX));
                maxY = std::max(maxY, static_cast<double>(c.fY));
            }
        }

        const cS3DVector2 base = projection.WorldToCanvas(BaseCorner(projection.rotation, citySizeX, citySizeZ));
        const double originX = base.fX + std::floor(minX - base.fX);
        const double originY = base.fY + std::floor(minY - base.fY);
        grid.originCanvas = {static_cast<float>(originX), static_cast<float>(originY)};
        grid.tilesX = static_cast<int>(std::ceil((maxX - originX) / grid.tileSize));
        grid.tilesY = static_cast<int>(std::ceil((maxY - originY) / grid.tileSize));
        return grid;
    }

    cS3DVector2 TileGrid::TileOrigin(const int tx, const int ty) const
    {
        return {originCanvas.fX + static_cast<float>(tx * tileSize), originCanvas.fY + static_cast<float>(ty * tileSize)};
    }

    bool TileGrid::VisibleTiles(const cS3DVector2& canvasMin, const cS3DVector2& canvasMax, int& tx0, int& ty0,
                                int& tx1, int& ty1) const
    {
        const double ts = tileSize;
        tx0 = std::clamp(static_cast<int>(std::floor((canvasMin.fX - originCanvas.fX) / ts)), 0, tilesX);
        ty0 = std::clamp(static_cast<int>(std::floor((canvasMin.fY - originCanvas.fY) / ts)), 0, tilesY);
        tx1 = std::clamp(static_cast<int>(std::ceil((canvasMax.fX - originCanvas.fX) / ts)), 0, tilesX);
        ty1 = std::clamp(static_cast<int>(std::ceil((canvasMax.fY - originCanvas.fY) / ts)), 0, tilesY);
        return tx0 < tx1 && ty0 < ty1;
    }
}
