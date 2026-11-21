#pragma once

// Overlay: the pure parts of the map overlay, header only so the tests need no extra
// sources. Image split into power-of-two textures, placement math, the draped mesh and the
// per-city JSON file. No game calls here. Facts and choices: docs/re-notes/overlay.md.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace OverlayMath
{
    // The built-in DirectX driver pads textures that are not a power of two and only the ImGui
    // renderer knows the padding (docs/re-notes/render.md). 2048 is the largest size every
    // driver the game supports accepts.
    inline constexpr int kMaxTexture = 2048;
    // One pixel copied from the next piece (or the image edge) on each side of a piece, so
    // bilinear filtering at a piece edge never reads padding or wraps around.
    inline constexpr int kGutter = 1;
    inline constexpr int kMaxContent = kMaxTexture - 2 * kGutter;
    // Textures cost twice their size: the driver texture and the copy cIGZImGuiService keeps
    // for device loss (docs/re-notes/render.md). Images above this are scaled down.
    inline constexpr size_t kTextureBudgetBytes = size_t{64} * 1024 * 1024;
    inline constexpr int kCellMetres = 16;
    // Cells per mesh chunk side. 16 x 16 cells is at most 1536 vertices when the overlay covers
    // the whole chunk, under the 2046 vertices the built-in driver takes in one draw.
    inline constexpr int kChunkCells = 16;

    [[nodiscard]] inline int NextPowerOfTwo(const int v)
    {
        int p = 1;
        while (p < v) {
            p <<= 1;
        }
        return p;
    }

    // One texture of the split image. x0, y0, w, h is the part of the image it shows. The
    // texture holds that part at (kGutter, kGutter), with a one pixel border from the
    // neighbouring pixels and edge pixels repeated into the padding.
    struct Piece
    {
        int x0 = 0, y0 = 0, w = 0, h = 0;
        int texW = 0, texH = 0;
        // UV of the image part inside the texture.
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    };

    // Row by row, top to bottom, left to right.
    [[nodiscard]] inline std::vector<Piece> SplitImage(const int w, const int h)
    {
        std::vector<Piece> pieces;
        if (w <= 0 || h <= 0) {
            return pieces;
        }
        for (int y = 0; y < h; y += kMaxContent) {
            const int ph = std::min(kMaxContent, h - y);
            for (int x = 0; x < w; x += kMaxContent) {
                const int pw = std::min(kMaxContent, w - x);
                Piece p{x, y, pw, ph, NextPowerOfTwo(pw + 2 * kGutter), NextPowerOfTwo(ph + 2 * kGutter)};
                p.u0 = static_cast<float>(kGutter) / static_cast<float>(p.texW);
                p.v0 = static_cast<float>(kGutter) / static_cast<float>(p.texH);
                p.u1 = static_cast<float>(kGutter + pw) / static_cast<float>(p.texW);
                p.v1 = static_cast<float>(kGutter + ph) / static_cast<float>(p.texH);
                pieces.push_back(p);
            }
        }
        return pieces;
    }

    [[nodiscard]] inline size_t TextureBytes(const std::vector<Piece>& pieces)
    {
        size_t bytes = 0;
        for (const Piece& p : pieces) {
            bytes += static_cast<size_t>(p.texW) * p.texH * 4 * 2;
        }
        return bytes;
    }

    // The largest size with the same aspect (at most w x h) whose textures fit the budget.
    inline void FitBudget(const int w, const int h, const size_t budget, int& outW, int& outH)
    {
        outW = w;
        outH = h;
        for (;;) {
            const size_t bytes = TextureBytes(SplitImage(outW, outH));
            if (bytes <= budget || (outW <= 1 && outH <= 1)) {
                return;
            }
            const double f = std::min(0.99, std::sqrt(static_cast<double>(budget) / static_cast<double>(bytes)));
            outW = std::max(1, static_cast<int>(outW * f));
            outH = std::max(1, static_cast<int>(std::lround(static_cast<double>(outW) * h / w)));
        }
    }

    // Fills every pixel outside the filled rectangle [fx0, fx1) x [fy0, fy1) of an RGBA
    // texture with the nearest filled pixel.
    inline void ReplicateEdges(uint8_t* rgba, const int texW, const int texH, const int fx0, const int fy0,
                               const int fx1, const int fy1)
    {
        const size_t stride = static_cast<size_t>(texW) * 4;
        for (int y = fy0; y < fy1; ++y) {
            uint8_t* row = rgba + y * stride;
            for (int x = 0; x < fx0; ++x) {
                std::memcpy(row + x * 4, row + fx0 * 4, 4);
            }
            for (int x = fx1; x < texW; ++x) {
                std::memcpy(row + x * 4, row + (fx1 - 1) * 4, 4);
            }
        }
        for (int y = 0; y < fy0; ++y) {
            std::memcpy(rgba + y * stride, rgba + fy0 * stride, stride);
        }
        for (int y = fy1; y < texH; ++y) {
            std::memcpy(rgba + y * stride, rgba + (fy1 - 1) * stride, stride);
        }
    }

    // Where the image lies on the ground. Local coordinates (a, b) run from 0 to 1 across the
    // image: a from the left edge to the right edge, b from the top edge to the bottom edge.
    // At rotation 0 a runs along world +X and b along world +Z. Rotation turns the image
    // around its centre from +X towards +Z.
    struct Placement
    {
        double cx = 0, cz = 0;          // centre, world metres
        double width = 0, height = 0;   // metres
        double rotationDeg = 0;
    };

    inline void LocalToWorld(const Placement& p, const double a, const double b, double& x, double& z)
    {
        const double r = p.rotationDeg * 3.14159265358979323846 / 180.0;
        const double c = std::cos(r), s = std::sin(r);
        const double lx = (a - 0.5) * p.width, lz = (b - 0.5) * p.height;
        x = p.cx + lx * c - lz * s;
        z = p.cz + lx * s + lz * c;
    }

    inline void WorldToLocal(const Placement& p, const double x, const double z, double& a, double& b)
    {
        const double r = p.rotationDeg * 3.14159265358979323846 / 180.0;
        const double c = std::cos(r), s = std::sin(r);
        const double dx = x - p.cx, dz = z - p.cz;
        a = p.width > 0 ? (dx * c + dz * s) / p.width + 0.5 : 0.5;
        b = p.height > 0 ? (-dx * s + dz * c) / p.height + 0.5 : 0.5;
    }

    // World bounding box of the local rectangle [a0, a1] x [b0, b1].
    inline void Bounds(const Placement& p, const double a0, const double b0, const double a1, const double b1,
                       double& minX, double& minZ, double& maxX, double& maxZ)
    {
        minX = minZ = 1e300;
        maxX = maxZ = -1e300;
        const double as[2] = {a0, a1}, bs[2] = {b0, b1};
        for (const double a : as) {
            for (const double b : bs) {
                double x, z;
                LocalToWorld(p, a, b, x, z);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minZ = std::min(minZ, z);
                maxZ = std::max(maxZ, z);
            }
        }
    }

    // Centres the image on an area of areaW x areaH metres from (0, 0) and makes it as large
    // as fits, keeping its aspect and rotation.
    [[nodiscard]] inline Placement FitToArea(Placement p, const double areaW, const double areaH)
    {
        p.cx = areaW * 0.5;
        p.cz = areaH * 0.5;
        if (p.width <= 0 || p.height <= 0) {
            return p;
        }
        double minX, minZ, maxX, maxZ;
        Bounds(p, 0, 0, 1, 1, minX, minZ, maxX, maxZ);
        const double k = std::min(areaW / (maxX - minX), areaH / (maxZ - minZ));
        p.width *= k;
        p.height *= k;
        return p;
    }

    // Scales the image by k around a world point, which stays where it is.
    [[nodiscard]] inline Placement ScaleAbout(Placement p, const double fx, const double fz, const double k)
    {
        p.cx = fx + (p.cx - fx) * k;
        p.cz = fz + (p.cz - fz) * k;
        p.width *= k;
        p.height *= k;
        return p;
    }

    // Terrain heights at the vertices of a block of cells, and the diagonal of each cell.
    struct HeightGrid
    {
        int cx0 = 0, cz0 = 0;     // first cell
        int cellsX = 0, cellsZ = 0;
        std::vector<float> heights;   // (cellsX + 1) * (cellsZ + 1), row by row
        std::vector<uint8_t> flipped; // cellsX * cellsZ: 1 = diagonal from (x+1, z) to (x, z+1)
        bool flat = false;            // all heights are flatHeight, no grid needed
        float flatHeight = 0;

        [[nodiscard]] float At(const int x, const int z) const
        {
            return flat ? flatHeight : heights[(z - cz0) * (cellsX + 1) + (x - cx0)];
        }
        [[nodiscard]] bool Flipped(const int x, const int z) const
        {
            return !flat && flipped[(z - cz0) * cellsX + (x - cx0)] != 0;
        }
    };

    // The cells under the image, clipped to a city of cityCellsX x cityCellsZ cells. False when
    // none.
    [[nodiscard]] inline bool CellRange(const Placement& p, const int cityCellsX, const int cityCellsZ, int& cx0,
                                        int& cz0, int& cx1, int& cz1)
    {
        if (p.width <= 0 || p.height <= 0) {
            return false;
        }
        double minX, minZ, maxX, maxZ;
        Bounds(p, 0, 0, 1, 1, minX, minZ, maxX, maxZ);
        cx0 = std::max(0, static_cast<int>(std::floor(minX / kCellMetres)));
        cz0 = std::max(0, static_cast<int>(std::floor(minZ / kCellMetres)));
        cx1 = std::min(cityCellsX, static_cast<int>(std::ceil(maxX / kCellMetres)));
        cz1 = std::min(cityCellsZ, static_cast<int>(std::ceil(maxZ / kCellMetres)));
        return cx0 < cx1 && cz0 < cz1;
    }

    // A run of vertices that uses one texture piece and covers one block of cells.
    struct Chunk
    {
        int piece = 0;
        uint32_t first = 0, count = 0;
        float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    };

    namespace Detail
    {
        struct ClipPoint
        {
            double x, y, z, a, b;
        };

        // Sutherland-Hodgman against one axis-aligned local bound. axis 0 = a, 1 = b. keepAbove
        // keeps the side >= bound.
        inline int ClipAgainst(const ClipPoint* in, const int count, ClipPoint* out, const int axis,
                               const double bound, const bool keepAbove)
        {
            auto value = [axis](const ClipPoint& p) { return axis == 0 ? p.a : p.b; };
            auto inside = [&](const ClipPoint& p) { return keepAbove ? value(p) >= bound : value(p) <= bound; };
            int n = 0;
            for (int i = 0; i < count; ++i) {
                const ClipPoint& cur = in[i];
                const ClipPoint& prev = in[(i + count - 1) % count];
                const bool curIn = inside(cur), prevIn = inside(prev);
                if (curIn != prevIn) {
                    const double t = (bound - value(prev)) / (value(cur) - value(prev));
                    out[n++] = {prev.x + (cur.x - prev.x) * t, prev.y + (cur.y - prev.y) * t,
                                prev.z + (cur.z - prev.z) * t, prev.a + (cur.a - prev.a) * t,
                                prev.b + (cur.b - prev.b) * t};
                }
                if (curIn) {
                    out[n++] = cur;
                }
            }
            return n;
        }
    }

    // Builds the draped mesh: the terrain triangles of every cell under each piece, clipped to
    // the piece, with the piece's UVs. Vertex is any aggregate {x, y, z, color, u, v}. verts and
    // chunks are cleared first and keep their capacity. Chunks are in piece order.
    template <typename Vertex>
    void BuildMesh(const Placement& p, const int imageW, const int imageH, const std::vector<Piece>& pieces,
                   const HeightGrid& grid, const float lift, const uint32_t color, std::vector<Vertex>& verts,
                   std::vector<Chunk>& chunks)
    {
        verts.clear();
        chunks.clear();
        if (imageW <= 0 || imageH <= 0 || p.width <= 0 || p.height <= 0 || grid.cellsX <= 0 || grid.cellsZ <= 0) {
            return;
        }
        Detail::ClipPoint bufA[8], bufB[8];
        for (int pi = 0; pi < static_cast<int>(pieces.size()); ++pi) {
            const Piece& piece = pieces[pi];
            const double a0 = static_cast<double>(piece.x0) / imageW, a1 = static_cast<double>(piece.x0 + piece.w) / imageW;
            const double b0 = static_cast<double>(piece.y0) / imageH, b1 = static_cast<double>(piece.y0 + piece.h) / imageH;
            double minX, minZ, maxX, maxZ;
            Bounds(p, a0, b0, a1, b1, minX, minZ, maxX, maxZ);
            const int px0 = std::max(grid.cx0, static_cast<int>(std::floor(minX / kCellMetres)));
            const int pz0 = std::max(grid.cz0, static_cast<int>(std::floor(minZ / kCellMetres)));
            const int px1 = std::min(grid.cx0 + grid.cellsX, static_cast<int>(std::ceil(maxX / kCellMetres)));
            const int pz1 = std::min(grid.cz0 + grid.cellsZ, static_cast<int>(std::ceil(maxZ / kCellMetres)));

            for (int bz = pz0; bz < pz1; bz += kChunkCells) {
                for (int bx = px0; bx < px1; bx += kChunkCells) {
                    Chunk chunk{pi, static_cast<uint32_t>(verts.size()), 0, 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
                    for (int cz = bz; cz < std::min(bz + kChunkCells, pz1); ++cz) {
                        for (int cx = bx; cx < std::min(bx + kChunkCells, px1); ++cx) {
                            Detail::ClipPoint corner[4];  // (x, z), (x+1, z), (x, z+1), (x+1, z+1)
                            for (int k = 0; k < 4; ++k) {
                                const int vx = cx + (k & 1), vz = cz + (k >> 1);
                                Detail::ClipPoint& c = corner[k];
                                c.x = static_cast<double>(vx) * kCellMetres;
                                c.z = static_cast<double>(vz) * kCellMetres;
                                c.y = grid.At(vx, vz);
                                WorldToLocal(p, c.x, c.z, c.a, c.b);
                            }
                            // The game's terrain triangles (cSTETerrainView3D::SetTriangleIndicesForCell).
                            static constexpr int kTris[2][2][3] = {{{0, 2, 3}, {0, 3, 1}}, {{0, 2, 1}, {1, 2, 3}}};
                            const int flip = grid.Flipped(cx, cz) ? 1 : 0;
                            for (const auto& tri : kTris[flip]) {
                                int n = 3;
                                for (int k = 0; k < 3; ++k) {
                                    bufA[k] = corner[tri[k]];
                                }
                                n = Detail::ClipAgainst(bufA, n, bufB, 0, a0, true);
                                n = Detail::ClipAgainst(bufB, n, bufA, 0, a1, false);
                                n = Detail::ClipAgainst(bufA, n, bufB, 1, b0, true);
                                n = Detail::ClipAgainst(bufB, n, bufA, 1, b1, false);
                                if (n < 3) {
                                    continue;
                                }
                                auto emit = [&](const Detail::ClipPoint& c) {
                                    const float u = static_cast<float>((kGutter + c.a * imageW - piece.x0) / piece.texW);
                                    const float v = static_cast<float>((kGutter + c.b * imageH - piece.y0) / piece.texH);
                                    const Vertex vert{static_cast<float>(c.x), static_cast<float>(c.y) + lift,
                                                      static_cast<float>(c.z), color, u, v};
                                    chunk.minX = std::min(chunk.minX, vert.x);
                                    chunk.minY = std::min(chunk.minY, vert.y);
                                    chunk.minZ = std::min(chunk.minZ, vert.z);
                                    chunk.maxX = std::max(chunk.maxX, vert.x);
                                    chunk.maxY = std::max(chunk.maxY, vert.y);
                                    chunk.maxZ = std::max(chunk.maxZ, vert.z);
                                    verts.push_back(vert);
                                };
                                for (int k = 1; k + 1 < n; ++k) {
                                    emit(bufA[0]);
                                    emit(bufA[k]);
                                    emit(bufA[k + 1]);
                                }
                            }
                        }
                    }
                    chunk.count = static_cast<uint32_t>(verts.size()) - chunk.first;
                    if (chunk.count > 0) {
                        chunks.push_back(chunk);
                    }
                }
            }
        }
    }

    // The overlay of one city, saved as <RendersDirectory>/overlays/<cityKey>.json.
    struct State
    {
        static constexpr int kFormatVersion = 1;
        std::string imagePath;     // absolute, UTF-8. Empty = no overlay
        std::string relativePath;  // to the region folder when the image is inside it, else empty
        Placement placement;
        bool keepAspect = true;
        float opacity = 0.6f;
        bool visible = true;
        bool followTerrain = true;
        float flatHeight = 0;      // metres, when followTerrain is false
        bool overBuildings = false;
    };

    [[nodiscard]] inline std::string ToJson(const State& s)
    {
        const nlohmann::json j{
            {"formatVersion", State::kFormatVersion},
            {"imagePath", s.imagePath},
            {"relativePath", s.relativePath},
            {"centreX", s.placement.cx},
            {"centreZ", s.placement.cz},
            {"width", s.placement.width},
            {"height", s.placement.height},
            {"rotation", s.placement.rotationDeg},
            {"keepAspect", s.keepAspect},
            {"opacity", s.opacity},
            {"visible", s.visible},
            {"followTerrain", s.followTerrain},
            {"flatHeight", s.flatHeight},
            {"overBuildings", s.overBuildings},
        };
        return j.dump(2);
    }

    // Missing fields keep their defaults. Nullopt for text that is not a JSON object, another
    // format version or mistyped fields.
    [[nodiscard]] inline std::optional<State> FromJson(const std::string_view text)
    {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object() || j.value("formatVersion", 0) != State::kFormatVersion) {
            return std::nullopt;
        }
        try {
            State s;
            s.imagePath = j.value("imagePath", s.imagePath);
            s.relativePath = j.value("relativePath", s.relativePath);
            s.placement.cx = j.value("centreX", s.placement.cx);
            s.placement.cz = j.value("centreZ", s.placement.cz);
            s.placement.width = j.value("width", s.placement.width);
            s.placement.height = j.value("height", s.placement.height);
            s.placement.rotationDeg = j.value("rotation", s.placement.rotationDeg);
            s.keepAspect = j.value("keepAspect", s.keepAspect);
            s.opacity = std::clamp(j.value("opacity", s.opacity), 0.0f, 1.0f);
            s.visible = j.value("visible", s.visible);
            s.followTerrain = j.value("followTerrain", s.followTerrain);
            s.flatHeight = j.value("flatHeight", s.flatHeight);
            s.overBuildings = j.value("overBuildings", s.overBuildings);
            if (!std::isfinite(s.placement.cx) || !std::isfinite(s.placement.cz) || !std::isfinite(s.placement.width) ||
                !std::isfinite(s.placement.height) || !std::isfinite(s.placement.rotationDeg) || s.placement.width < 0 ||
                s.placement.height < 0) {
                return std::nullopt;
            }
            return s;
        }
        catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
    }
}
