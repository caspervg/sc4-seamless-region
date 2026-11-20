#pragma once

// Export: the pure math of the region map. No game calls, header only, unit tested in
// tests/ExportTests.cpp.
//
// The map canvas is one pixel plane for the whole region at one source zoom (3, 4 or 5, see
// ChooseSourceZoom) and one rotation. Every
// city is placed on it the way the neighbor renderer places a neighbor around a reference
// city (NeighborMath::PlaceLevel), so the map shows the region as the game shows it from that
// city. Output tiles are kTileSize pixels, Leaflet zoom maxZoom is the canvas at 1:1 and each
// lower zoom halves it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "region/RegionLayout.hpp"
#include "render/NeighborMath.hpp"
#include "store/TileStore.hpp"
#include "view/ViewMath.hpp"

namespace MapMath
{
    inline constexpr int kTileSize = 256;  // Leaflet default
    // The zoom indexes a map can be made from: zoom 3 to 5.
    inline constexpr int kMinSourceZoom = 2, kMaxSourceZoom = 4;
    // Ground height for outlines when meta.json has none: the sea level of a new region.
    inline constexpr float kDefaultGroundHeight = 250.0f;

    // The default source zoom of the map. zoomMasks has one entry per rendered city: bit z is set
    // when the city has a level of zoom index z that the map can use. The highest zoom from
    // kMinSourceZoom to kMaxSourceZoom that every city has. When no zoom is shared by all, the
    // one that most cities have (the higher one on a tie). -1 when no city has one.
    [[nodiscard]] inline int ChooseSourceZoom(const std::vector<uint32_t>& zoomMasks)
    {
        int best = -1;
        size_t bestCount = 0;
        for (int z = kMaxSourceZoom; z >= kMinSourceZoom; --z) {
            const size_t count = static_cast<size_t>(
                std::ranges::count_if(zoomMasks, [z](const uint32_t mask) { return ((mask >> z) & 1u) != 0; }));
            if (count > 0 && count == zoomMasks.size()) {
                return z;
            }
            if (count > bestCount) {
                best = z;
                bestCount = count;
            }
        }
        return best;
    }

    // Canvas position of the top-left corner of a city's tile (0,0), relative to the reference
    // city's base corner, in whole pixels. levelOrigin is the level's originX/originY from
    // meta.json. Same rounding as a neighbor drawn around the reference city.
    struct Point
    {
        int x = 0, y = 0;
    };

    [[nodiscard]] inline Point PlaceCity(const ViewMath::OrthoProjection& projection, const CityInfo& reference,
                                         const CityInfo& city, const float levelOriginX, const float levelOriginY)
    {
        const cS3DVector2 neighbor = projection.WorldToCanvas(RegionLayout::OriginOffset(reference, city));
        const cS3DVector2 base = projection.WorldToCanvas(
            ViewMath::BaseCorner(projection.rotation, static_cast<float>(reference.sizeX) * ViewMath::kRegionUnitMetres,
                                 static_cast<float>(reference.sizeZ) * ViewMath::kRegionUnitMetres));
        const NeighborMath::LevelPlacement p = NeighborMath::PlaceLevel(
            neighbor.fX, neighbor.fY, levelOriginX, levelOriginY, 1, 1.0, base.fX, base.fY, 0.0, 0.0);
        return {static_cast<int>(p.originX), static_cast<int>(p.originY)};
    }

    // The ground outline of a city on the map, in map pixels. place is where the city's tile
    // (0,0) is on the map. With edges that fit the city (CityMeta::EdgesFit) it follows the edge
    // heights, one point per sample, so two neighbors share the line between them. Else the four
    // corners at groundHeight. Clockwise from the corner at (0, 0): north, east, south, west.
    [[nodiscard]] inline std::vector<cS3DVector2> Outline(const ViewMath::OrthoProjection& projection,
                                                          const CityInfo& city, const float levelOriginX,
                                                          const float levelOriginY, const Point place,
                                                          const float groundHeight,
                                                          const CityRenderMeta::Edges& edges = {})
    {
        const float sx = static_cast<float>(city.sizeX) * ViewMath::kRegionUnitMetres;
        const float sz = static_cast<float>(city.sizeZ) * ViewMath::kRegionUnitMetres;
        std::vector<cS3DVector2> out;
        const auto add = [&](const float x, const float y, const float z) {
            const cS3DVector2 c = projection.WorldToCanvas(cS3DVector3(x, y, z));
            out.push_back({static_cast<float>(place.x + (static_cast<double>(c.fX) - levelOriginX)),
                           static_cast<float>(place.y + (static_cast<double>(c.fY) - levelOriginY))});
        };
        if (!CityMeta::EdgesFit(edges, city.sizeX, city.sizeZ)) {
            add(0.0f, groundHeight, 0.0f);
            add(sx, groundHeight, 0.0f);
            add(sx, groundHeight, sz);
            add(0.0f, groundHeight, sz);
            return out;
        }
        // Each edge without its last sample: that is the first of the next edge.
        const float step = edges.step;
        const size_t nx = edges.north.size() - 1, nz = edges.west.size() - 1;
        out.reserve(2 * (nx + nz));
        for (size_t i = 0; i < nx; ++i) {
            add(static_cast<float>(i) * step, edges.north[i], 0.0f);
        }
        for (size_t i = 0; i < nz; ++i) {
            add(sx, edges.east[i], static_cast<float>(i) * step);
        }
        for (size_t i = nx; i > 0; --i) {
            add(static_cast<float>(i) * step, edges.south[i], sz);
        }
        for (size_t i = nz; i > 0; --i) {
            add(0.0f, edges.west[i], static_cast<float>(i) * step);
        }
        return out;
    }

    // One city level on the canvas: tile (0,0) at (x, y), tilesX by tilesY tiles of tileSize.
    struct Source
    {
        int x = 0, y = 0;
        int tilesX = 0, tilesY = 0;
        int tileSize = 512;
    };

    [[nodiscard]] inline int FloorDiv(const int a, const int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    // The source tiles that overlap the canvas square [x, x + size) x [y, y + size), clamped to
    // the grid. tx1 and ty1 are exclusive. False when none overlaps.
    inline bool SourceRange(const Source& s, const int x, const int y, const int size, int& tx0, int& ty0, int& tx1,
                            int& ty1)
    {
        tx0 = std::max(0, FloorDiv(x - s.x, s.tileSize));
        ty0 = std::max(0, FloorDiv(y - s.y, s.tileSize));
        tx1 = std::min(s.tilesX, FloorDiv(x + size - 1 - s.x, s.tileSize) + 1);
        ty1 = std::min(s.tilesY, FloorDiv(y + size - 1 - s.y, s.tileSize) + 1);
        return tx0 < tx1 && ty0 < ty1;
    }

    // The Leaflet zoom of the canvas at 1:1: the smallest z where the canvas fits in 2^z by 2^z
    // tiles. Zoom 0 is then one tile for the whole region.
    [[nodiscard]] inline int MaxZoom(const int width, const int height)
    {
        const int tiles = std::max(1, (std::max(width, height) + kTileSize - 1) / kTileSize);
        int zoom = 0;
        while ((1 << zoom) < tiles) {
            ++zoom;
        }
        return zoom;
    }

    // Tiles along one side of the canvas at a zoom.
    [[nodiscard]] inline int TilesAt(const int canvasSize, const int maxZoom, const int zoom)
    {
        const int64_t span = static_cast<int64_t>(kTileSize) << (maxZoom - zoom);
        return static_cast<int>((canvasSize + span - 1) / span);
    }

    // ---- pixels ------------------------------------------------------------------------------
    // Buffers are RGBA, 4 bytes per pixel, top row first. "Premultiplied" means the colors are
    // multiplied by alpha / 255.

    [[nodiscard]] inline uint8_t Mul255(const unsigned a, const unsigned b)
    {
        const unsigned t = a * b + 128;
        return static_cast<uint8_t>((t + (t >> 8)) >> 8);
    }

    inline void Premultiply(uint8_t* rgba, const size_t pixels)
    {
        for (size_t i = 0; i < pixels; ++i, rgba += 4) {
            const unsigned a = rgba[3];
            if (a != 255) {
                rgba[0] = Mul255(rgba[0], a);
                rgba[1] = Mul255(rgba[1], a);
                rgba[2] = Mul255(rgba[2], a);
            }
        }
    }

    inline void Unpremultiply(uint8_t* rgba, const size_t pixels)
    {
        for (size_t i = 0; i < pixels; ++i, rgba += 4) {
            const unsigned a = rgba[3];
            if (a == 0) {
                rgba[0] = rgba[1] = rgba[2] = 0;
            }
            else if (a != 255) {
                for (int c = 0; c < 3; ++c) {
                    rgba[c] = static_cast<uint8_t>(std::min(255u, (rgba[c] * 255u + a / 2) / a));
                }
            }
        }
    }

    [[nodiscard]] inline bool IsTransparent(const uint8_t* rgba, const size_t pixels)
    {
        for (size_t i = 0; i < pixels; ++i) {
            if (rgba[i * 4 + 3] != 0) {
                return false;
            }
        }
        return true;
    }

    // Draws a straight-alpha source over a premultiplied square of size by size pixels. The
    // source's top-left lands at (dx, dy) in dst, parts outside dst are cut off.
    inline void BlendOver(uint8_t* dst, const int size, const uint8_t* src, const int srcW, const int srcH,
                          const int dx, const int dy)
    {
        const int x0 = std::max(0, dx), y0 = std::max(0, dy);
        const int x1 = std::min(size, dx + srcW), y1 = std::min(size, dy + srcH);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = src + (static_cast<size_t>(y - dy) * srcW + (x0 - dx)) * 4;
            uint8_t* d = dst + (static_cast<size_t>(y) * size + x0) * 4;
            for (int x = x0; x < x1; ++x, s += 4, d += 4) {
                const unsigned a = s[3];
                if (a == 0) {
                    continue;
                }
                if (a == 255) {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = 255;
                    continue;
                }
                const unsigned keep = 255 - a;
                d[0] = static_cast<uint8_t>(Mul255(s[0], a) + Mul255(d[0], keep));
                d[1] = static_cast<uint8_t>(Mul255(s[1], a) + Mul255(d[1], keep));
                d[2] = static_cast<uint8_t>(Mul255(s[2], a) + Mul255(d[2], keep));
                d[3] = static_cast<uint8_t>(a + Mul255(d[3], keep));
            }
        }
    }

    // Halves four premultiplied size by size children into one size by size tile with a 2x2
    // box filter. children: top-left, top-right, bottom-left, bottom-right. nullptr is a
    // transparent child. size must be even.
    inline void Downscale2x(const std::array<const uint8_t*, 4>& children, const int size, uint8_t* out)
    {
        const int half = size / 2;
        for (int q = 0; q < 4; ++q) {
            const uint8_t* child = children[q];
            const int ox = (q & 1) * half, oy = (q >> 1) * half;
            for (int y = 0; y < half; ++y) {
                uint8_t* d = out + (static_cast<size_t>(oy + y) * size + ox) * 4;
                if (!child) {
                    std::fill(d, d + static_cast<size_t>(half) * 4, uint8_t{0});
                    continue;
                }
                const uint8_t* r0 = child + static_cast<size_t>(2 * y) * size * 4;
                const uint8_t* r1 = r0 + static_cast<size_t>(size) * 4;
                for (int x = 0; x < half; ++x, d += 4, r0 += 8, r1 += 8) {
                    for (int c = 0; c < 4; ++c) {
                        d[c] = static_cast<uint8_t>((r0[c] + r0[c + 4] + r1[c] + r1[c + 4] + 2) / 4);
                    }
                }
            }
        }
    }
}
