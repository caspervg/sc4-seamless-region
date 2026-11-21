#pragma once

// Render: the pure math of neighbor drawing. No game calls, header only, unit tested
// in tests/RenderTests.cpp. The derivations are in docs/re-notes/render.md.

#include <algorithm>
#include <cmath>
#include <vector>

namespace NeighborMath
{
    // ---------------------------------------------------------------------------------------
    // Depth order
    // ---------------------------------------------------------------------------------------

    // A city on the region grid, in region cells. x1 and z1 are exclusive.
    struct CellRect
    {
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    };

    // The sign of the camera-ward direction on each ground axis. The eye is at
    // target + (cos yaw, tan pitch, sin yaw) * 8192 with yaw = (rotation + 1) * 90 - 22.5
    // degrees, so camera-ward on the ground is (cos yaw, sin yaw):
    // rotation 0 (+,+), 1 (-,+), 2 (-,-), 3 (+,-). Neither component is ever zero.
    struct ViewSigns
    {
        int x = 1, z = 1;
    };

    [[nodiscard]] inline ViewSigns SignsForRotation(const int rotation)
    {
        static constexpr ViewSigns kSigns[4] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
        return kSigns[rotation & 3];
    }

    // True when a can hide b, so a must be drawn after b.
    //
    // With u = signs.x * x and v = signs.z * z, both growing towards the camera, a view ray
    // that passes over b and then over a (closer to the camera) needs a point of a that is
    // greater than a point of b in both u and v. For rectangles: a.uMax > b.uMin and
    // a.vMax > b.vMin. This holds for every view direction in the quadrant, so it does not
    // depend on the exact yaw or on building heights. For rectangles that do not overlap the
    // relation has no cycles (Guibas and Yao, "On translating a set of rectangles").
    [[nodiscard]] inline bool InFrontOf(const CellRect& a, const CellRect& b, const ViewSigns signs)
    {
        const int aUMax = signs.x > 0 ? a.x1 : -a.x0;
        const int bUMin = signs.x > 0 ? b.x0 : -b.x1;
        const int aVMax = signs.z > 0 ? a.z1 : -a.z0;
        const int bVMin = signs.z > 0 ? b.z0 : -b.z1;
        return aUMax > bUMin && aVMax > bVMin;
    }

    // Sorts the neighbors back to front and splits them at the current city.
    //
    // back gets the indices (into rects) to draw before the current city, front the ones to
    // draw after it, each list back to front. A neighbor is "front" when it must be drawn
    // after the current city: it can hide the current city, or it can hide another front
    // neighbor. Every front neighbor lies fully on the camera side of one of the current
    // city's edge planes. Everything else is "back".
    //
    // This is a topological sort (InFrontOf is not a strict weak order, so std::sort cannot
    // be used). The current city is held back until nothing else can be drawn, so exactly
    // the neighbors that depend on it come after it.
    // ponytail: O(n^2) pair tests per rotation, run once per city load. Fine for a few
    // thousand cities. Use a sweep if regions ever get larger.
    inline void SortBackToFront(const std::vector<CellRect>& rects, const CellRect& current, const ViewSigns signs,
                                std::vector<int>& back, std::vector<int>& front)
    {
        const int n = static_cast<int>(rects.size());
        back.clear();
        front.clear();

        // Node n is the current city. blockers[i] = how many undrawn nodes i can hide.
        auto rect = [&](const int i) -> const CellRect& { return i == n ? current : rects[i]; };
        std::vector<int> blockers(n + 1, 0);
        std::vector<char> drawn(n + 1, 0);
        for (int i = 0; i <= n; ++i) {
            for (int j = 0; j <= n; ++j) {
                if (i != j && InFrontOf(rect(i), rect(j), signs)) {
                    ++blockers[i];
                }
            }
        }

        bool currentDrawn = false;
        for (int emitted = 0; emitted <= n; ++emitted) {
            int next = -1;
            for (int i = 0; i < n; ++i) {
                if (!drawn[i] && blockers[i] == 0) {
                    next = i;
                    break;
                }
            }
            if (next < 0 && !currentDrawn) {
                next = n;
            }
            if (next < 0) {
                // Only possible when cities overlap (bad data). Keep the order total.
                for (int i = 0; i < n; ++i) {
                    if (!drawn[i]) {
                        next = i;
                        break;
                    }
                }
            }
            drawn[next] = 1;
            if (next == n) {
                currentDrawn = true;
            }
            else {
                (currentDrawn ? front : back).push_back(next);
            }
            for (int i = 0; i <= n; ++i) {
                if (!drawn[i] && InFrontOf(rect(i), rect(next), signs)) {
                    --blockers[i];
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // Placement
    // ---------------------------------------------------------------------------------------

    // Where one render level of a neighbor lands on the screen.
    struct LevelPlacement
    {
        double originX = 0.0;     // screen position of the top-left corner of tile (0,0)
        double originY = 0.0;
        double tilePixels = 0.0;  // on-screen size of one tile: tile size * scale
    };

    // neighborCanvas: WorldToCanvas(origin offset of the neighbor) in the current canvas.
    // levelOrigin: canvas origin of tile (0,0) from meta.json, in the neighbor's own canvas
    //   at the level's zoom.
    // scale: current pixels per level pixel (CityMeta::LevelScale times the view's magnification:
    //   1 for the level of the view's zoom, 2 when zoom 5 shows zoom 4 tiles).
    // baseCanvas: WorldToCanvas(base corner of the current city).
    // baseScreen: baseCanvas + canvasToScreen. The game snaps it to a whole pixel.
    //
    // The neighbor offset is a fraction of a pixel off the game's pixel grid. The position
    // is rounded relative to the base corner, which does not move within the pixel grid
    // while the view pans. So the rounding gives the same result on every frame of a pan
    // and tiles never jitter. The error is at most half a pixel on each axis.
    [[nodiscard]] inline LevelPlacement PlaceLevel(const double neighborCanvasX, const double neighborCanvasY,
                                                   const double levelOriginX, const double levelOriginY,
                                                   const int tileSize, const double scale, const double baseCanvasX,
                                                   const double baseCanvasY, const double baseScreenX,
                                                   const double baseScreenY)
    {
        const double dx = neighborCanvasX + scale * levelOriginX - baseCanvasX;
        const double dy = neighborCanvasY + scale * levelOriginY - baseCanvasY;
        return {std::floor(dx + 0.5) + baseScreenX, std::floor(dy + 0.5) + baseScreenY, tileSize * scale};
    }

    // Screen position of the left (or top) edge of tile t. Adjacent tiles share the edge.
    [[nodiscard]] inline double TileEdge(const double origin, const double tilePixels, const int t)
    {
        return origin + std::floor(t * tilePixels + 0.5);
    }

    // The tiles that overlap the viewport [0, viewWidth) x [0, viewHeight), clamped to the
    // level's tile counts. tx1 and ty1 are exclusive. Returns false when none is visible.
    inline bool VisibleTiles(const LevelPlacement& p, const int tilesX, const int tilesY, const int viewWidth,
                             const int viewHeight, int& tx0, int& ty0, int& tx1, int& ty1)
    {
        if (p.tilePixels <= 0.0) {
            return false;
        }
        auto first = [&](const double origin, const int count) {
            return std::clamp(static_cast<int>(std::floor(-origin / p.tilePixels)), 0, count);
        };
        auto last = [&](const double origin, const int view, const int count) {
            return std::clamp(static_cast<int>(std::ceil((view - origin) / p.tilePixels)), 0, count);
        };
        tx0 = first(p.originX, tilesX);
        ty0 = first(p.originY, tilesY);
        tx1 = last(p.originX, viewWidth, tilesX);
        ty1 = last(p.originY, viewHeight, tilesY);
        return tx0 < tx1 && ty0 < ty1;
    }

    // ---------------------------------------------------------------------------------------
    // Invalidation
    // ---------------------------------------------------------------------------------------

    // Viewport pixels, right and bottom exclusive. Same layout as the game's cRZRect.
    struct ScreenRect
    {
        int left = 0, top = 0, right = 0, bottom = 0;

        [[nodiscard]] long long Area() const
        {
            return static_cast<long long>(right - left) * (bottom - top);
        }
    };

    [[nodiscard]] inline ScreenRect TileScreenRect(const LevelPlacement& p, const int tx, const int ty)
    {
        return {static_cast<int>(std::floor(TileEdge(p.originX, p.tilePixels, tx))),
                static_cast<int>(std::floor(TileEdge(p.originY, p.tilePixels, ty))),
                static_cast<int>(std::ceil(TileEdge(p.originX, p.tilePixels, tx + 1))),
                static_cast<int>(std::ceil(TileEdge(p.originY, p.tilePixels, ty + 1)))};
    }

    // Collects the screen rectangles of the tiles that became ready in one frame. Rectangles
    // that join without wasted area (tiles next to each other, or one inside the other) are
    // merged. When there are more than kCapacity separate rectangles, the whole view is
    // marked instead.
    class DirtyRects
    {
    public:
        static constexpr int kCapacity = 16;

        void Add(const ScreenRect& r)
        {
            if (wholeView_ || r.right <= r.left || r.bottom <= r.top) {
                return;
            }
            for (int i = 0; i < count_; ++i) {
                const ScreenRect u{std::min(rects_[i].left, r.left), std::min(rects_[i].top, r.top),
                                   std::max(rects_[i].right, r.right), std::max(rects_[i].bottom, r.bottom)};
                if (u.Area() <= rects_[i].Area() + r.Area()) {
                    rects_[i] = u;
                    return;
                }
            }
            if (count_ == kCapacity) {
                MarkWholeView();
                return;
            }
            rects_[count_++] = r;
        }

        void MarkWholeView()
        {
            wholeView_ = true;
            count_ = 0;
        }

        void Clear()
        {
            wholeView_ = false;
            count_ = 0;
        }

        [[nodiscard]] bool Empty() const { return !wholeView_ && count_ == 0; }
        [[nodiscard]] bool WholeView() const { return wholeView_; }
        [[nodiscard]] int Count() const { return count_; }
        [[nodiscard]] const ScreenRect& operator[](const int i) const { return rects_[i]; }

    private:
        ScreenRect rects_[kCapacity];
        int count_ = 0;
        bool wholeView_ = false;
    };
}
