#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "TestRunner.hpp"
#include "render/NeighborMath.hpp"
#include "render/TileLru.hpp"
#include "view/ViewMath.hpp"

using namespace NeighborMath;

namespace
{
    bool Contains(const std::vector<int>& list, const int value)
    {
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    // Deterministic pseudo-random numbers (no <random>, the same on every run).
    struct Lcg
    {
        uint32_t state;
        int Next(const int bound)
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<int>((state >> 8) % static_cast<uint32_t>(bound));
        }
    };

    // Cities of size 1, 2 and 4 on a 16 x 16 region grid that do not overlap. The first one
    // is the current city.
    std::vector<CellRect> RandomRegion(const uint32_t seed)
    {
        Lcg lcg{seed};
        bool used[16][16] = {};
        std::vector<CellRect> rects;
        for (int attempt = 0; attempt < 400 && rects.size() < 40; ++attempt) {
            const int size = 1 << lcg.Next(3);
            const int x = lcg.Next(16 / size) * size;
            const int z = lcg.Next(16 / size) * size;
            bool free = true;
            for (int i = x; i < x + size && free; ++i) {
                for (int j = z; j < z + size; ++j) {
                    free = free && !used[i][j];
                }
            }
            if (!free) {
                continue;
            }
            for (int i = x; i < x + size; ++i) {
                for (int j = z; j < z + size; ++j) {
                    used[i][j] = true;
                }
            }
            rects.push_back({x, z, x + size, z + size});
        }
        return rects;
    }

    TileKey Key(const int tx, const int ty)
    {
        TileKey key;
        key.cityKey = "a_city_with_a_long_name";
        key.rotation = 1;
        key.zoom = 2;
        key.tx = tx;
        key.ty = ty;
        return key;
    }
}

TEST_CASE(ViewSignsMatchTheCamera)
{
    // Camera-ward on the ground is the opposite of the view direction.
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < ViewMath::kZoomCount; ++z) {
            const auto p = ViewMath::OrthoProjection::For(z, r);
            const ViewSigns s = SignsForRotation(r);
            CHECK((-p.forward.fX > 0.0f) == (s.x > 0));
            CHECK((-p.forward.fZ > 0.0f) == (s.z > 0));
        }
    }
}

TEST_CASE(EdgeNeighborsAreFrontWhenLowerOnScreen)
{
    // A neighbor that shares an edge is in front exactly when it is nearer to the camera,
    // which on the ground means lower on the screen (canvas y grows downwards).
    const CellRect current{4, 4, 6, 6};
    const int steps[4][2] = {{2, 0}, {-2, 0}, {0, 2}, {0, -2}};
    std::vector<CellRect> rects;
    for (const auto& s : steps) {
        rects.push_back({current.x0 + s[0], current.z0 + s[1], current.x1 + s[0], current.z1 + s[1]});
    }
    for (int r = 0; r < 4; ++r) {
        std::vector<int> back, front;
        SortBackToFront(rects, current, SignsForRotation(r), back, front);
        CHECK(back.size() + front.size() == rects.size());
        const auto p = ViewMath::OrthoProjection::For(2, r);
        for (int i = 0; i < 4; ++i) {
            const cS3DVector2 c = p.WorldToCanvas(
                cS3DVector3(steps[i][0] * ViewMath::kRegionUnitMetres, 0.0f, steps[i][1] * ViewMath::kRegionUnitMetres));
            CHECK(Contains(front, i) == (c.fY > 0.0f));
            CHECK(Contains(back, i) == (c.fY < 0.0f));
        }
        CHECK(front.size() == 2);
    }
}

TEST_CASE(CornerNeighborsAreFrontOnlyWhenNearerOnBothAxes)
{
    const CellRect current{4, 4, 5, 5};
    const int steps[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
    std::vector<CellRect> rects;
    for (const auto& s : steps) {
        rects.push_back({current.x0 + s[0], current.z0 + s[1], current.x1 + s[0], current.z1 + s[1]});
    }
    for (int r = 0; r < 4; ++r) {
        const ViewSigns signs = SignsForRotation(r);
        std::vector<int> back, front;
        SortBackToFront(rects, current, signs, back, front);
        for (int i = 0; i < 4; ++i) {
            const bool nearer = steps[i][0] * signs.x > 0 && steps[i][1] * signs.z > 0;
            CHECK(Contains(front, i) == nearer);
        }
        CHECK(front.size() == 1);
        CHECK(back.size() == 3);
    }
}

TEST_CASE(NeighborBesideAnEdgePlaneIsNotAlwaysFront)
{
    // Rotation 0, camera-ward is +x +z. F lies fully on the camera side of the current
    // city's +x edge plane, but it is further away in z, so no view ray crosses both. B is
    // behind the current city and can hide F. Drawing F after the current city (as "front")
    // would put it over B. Both must be back, F first.
    const CellRect current{0, 0, 2, 2};
    const std::vector<CellRect> rects{{0, -2, 4, 0} /* B */, {3, -6, 6, -2} /* F */};
    const ViewSigns signs = SignsForRotation(0);
    CHECK(InFrontOf(rects[0], rects[1], signs));
    CHECK(!InFrontOf(rects[1], current, signs));
    std::vector<int> back, front;
    SortBackToFront(rects, current, signs, back, front);
    CHECK(front.empty());
    CHECK(back.size() == 2);
    CHECK(back.size() == 2 && back[0] == 1 && back[1] == 0);
}

TEST_CASE(NeighborThatHidesAFrontNeighborIsFront)
{
    // F is in front of the current city. B cannot hide the current city itself, but it can
    // hide F, so it must come after F and is front too.
    const CellRect current{0, 0, 2, 2};
    const std::vector<CellRect> rects{{-8, 4, -6, 6} /* B */, {-10, 2, 6, 4} /* F */};
    const ViewSigns signs = SignsForRotation(0);
    CHECK(!InFrontOf(rects[0], current, signs));
    CHECK(InFrontOf(rects[1], current, signs));
    CHECK(InFrontOf(rects[0], rects[1], signs));
    std::vector<int> back, front;
    SortBackToFront(rects, current, signs, back, front);
    CHECK(back.empty());
    CHECK(front.size() == 2 && front[0] == 1 && front[1] == 0);
}

TEST_CASE(SortIsBackToFrontForRandomRegionsAndAllRotations)
{
    for (uint32_t seed = 1; seed <= 25; ++seed) {
        std::vector<CellRect> all = RandomRegion(seed);
        CHECK(all.size() > 5);
        const CellRect current = all.front();
        const std::vector<CellRect> rects(all.begin() + 1, all.end());
        const int n = static_cast<int>(rects.size());
        for (int r = 0; r < 4; ++r) {
            const ViewSigns signs = SignsForRotation(r);
            std::vector<int> back, front;
            SortBackToFront(rects, current, signs, back, front);

            // The full draw sequence: back, the current city (index n), front.
            std::vector<int> sequence = back;
            sequence.push_back(n);
            sequence.insert(sequence.end(), front.begin(), front.end());
            CHECK(static_cast<int>(sequence.size()) == n + 1);
            std::vector<int> sorted = sequence;
            std::sort(sorted.begin(), sorted.end());
            for (int i = 0; i <= n; ++i) {
                CHECK(sorted[i] == i);
            }
            auto rect = [&](const int i) -> const CellRect& { return i == n ? current : rects[i]; };

            // Nothing drawn earlier may hide something drawn later.
            for (size_t i = 0; i < sequence.size(); ++i) {
                for (size_t j = i + 1; j < sequence.size(); ++j) {
                    CHECK(!InFrontOf(rect(sequence[i]), rect(sequence[j]), signs));
                }
            }

            // Front is exactly what must come after the current city: everything that can
            // hide it, directly or through other front neighbors.
            std::vector<char> mustFollow(n, 0);
            for (bool grew = true; grew;) {
                grew = false;
                for (int i = 0; i < n; ++i) {
                    if (mustFollow[i]) {
                        continue;
                    }
                    bool follows = InFrontOf(rects[i], current, signs);
                    for (int j = 0; j < n && !follows; ++j) {
                        follows = mustFollow[j] && InFrontOf(rects[i], rects[j], signs);
                    }
                    if (follows) {
                        mustFollow[i] = 1;
                        grew = true;
                    }
                }
            }
            for (int i = 0; i < n; ++i) {
                CHECK(Contains(front, i) == (mustFollow[i] != 0));
                // Every front neighbor is fully on the camera side of an edge plane of the
                // current city.
                if (mustFollow[i]) {
                    const int uMin = signs.x > 0 ? rects[i].x0 : -rects[i].x1;
                    const int vMin = signs.z > 0 ? rects[i].z0 : -rects[i].z1;
                    const int curUMax = signs.x > 0 ? current.x1 : -current.x0;
                    const int curVMax = signs.z > 0 ? current.z1 : -current.z0;
                    CHECK(uMin >= curUMax || vMin >= curVMax);
                }
            }
        }
    }
}

TEST_CASE(FractionalNeighborOffsetIsRoundedToThePixelGrid)
{
    // Neighbor canvas offset and base corner with fractions, base corner on a whole pixel.
    const LevelPlacement p = PlaceLevel(1234.4, -876.6, -300.0, -200.0, 512, 1.0, 10.25, 20.75, 412.0, 87.0);
    CHECK_NEAR(p.originX, 1336.0, 1e-9);   // 1234.4 - 300 - 10.25 = 924.15 -> 924, + 412
    CHECK_NEAR(p.originY, -1010.0, 1e-9);  // -876.6 - 200 - 20.75 = -1097.35 -> -1097, + 87
    CHECK_NEAR(p.tilePixels, 512.0, 1e-9);
    // At most half a pixel away from the exact position.
    CHECK(std::fabs(p.originX - (924.15 + 412.0)) <= 0.5);
    CHECK(std::fabs(p.originY - (-1097.35 + 87.0)) <= 0.5);

    // A pan moves the base corner by whole pixels. The tiles move by the same amount.
    const LevelPlacement panned = PlaceLevel(1234.4, -876.6, -300.0, -200.0, 512, 1.0, 10.25, 20.75, 375.0, 99.0);
    CHECK_NEAR(panned.originX - p.originX, -37.0, 1e-9);
    CHECK_NEAR(panned.originY - p.originY, 12.0, 1e-9);

    // Halves round up on both sides of zero.
    CHECK_NEAR(PlaceLevel(0.5, -0.5, 0.0, 0.0, 512, 1.0, 0.0, 0.0, 0.0, 0.0).originX, 1.0, 1e-9);
    CHECK_NEAR(PlaceLevel(0.5, -0.5, 0.0, 0.0, 512, 1.0, 0.0, 0.0, 0.0, 0.0).originY, 0.0, 1e-9);

    // Zoom 5 shows zoom 4 tiles at twice the size: the level origin scales too.
    const LevelPlacement doubled = PlaceLevel(100.3, 50.2, -300.0, -200.0, 512, 2.0, 0.0, 0.0, 0.0, 0.0);
    CHECK_NEAR(doubled.originX, -500.0, 1e-9);
    CHECK_NEAR(doubled.originY, -350.0, 1e-9);
    CHECK_NEAR(doubled.tilePixels, 1024.0, 1e-9);

    // Adjacent tiles share their edge exactly.
    for (int t = 0; t < 5; ++t) {
        CHECK(TileScreenRect(p, t, 0).right == TileScreenRect(p, t + 1, 0).left);
        CHECK(TileScreenRect(p, 0, t).bottom == TileScreenRect(p, 0, t + 1).top);
        CHECK(TileScreenRect(p, t, 0).right - TileScreenRect(p, t, 0).left == 512);
    }
}

TEST_CASE(VisibleTileRangeCoversExactlyTheViewport)
{
    int tx0, ty0, tx1, ty1;
    const LevelPlacement p{-100.0, -50.0, 512.0};
    CHECK(VisibleTiles(p, 4, 3, 1024, 768, tx0, ty0, tx1, ty1));
    CHECK(tx0 == 0 && tx1 == 3 && ty0 == 0 && ty1 == 2);

    // Clamped to the level's tile counts.
    CHECK(VisibleTiles(LevelPlacement{-1100.0, 0.0, 512.0}, 4, 3, 1024, 768, tx0, ty0, tx1, ty1));
    CHECK(tx0 == 2 && tx1 == 4 && ty0 == 0 && ty1 == 2);

    // Off screen on any side.
    CHECK(!VisibleTiles(LevelPlacement{2000.0, 0.0, 512.0}, 4, 3, 1024, 768, tx0, ty0, tx1, ty1));
    CHECK(!VisibleTiles(LevelPlacement{-2048.0, 0.0, 512.0}, 4, 3, 1024, 768, tx0, ty0, tx1, ty1));
    CHECK(!VisibleTiles(LevelPlacement{0.0, 768.0, 512.0}, 4, 3, 1024, 768, tx0, ty0, tx1, ty1));
    CHECK(!VisibleTiles(LevelPlacement{0.0, 0.0, 0.0}, 4, 3, 1024, 768, tx0, ty0, tx1, ty1));

    // Against a brute force test of every tile, at 1x and at 2x.
    for (const double tilePixels : {512.0, 1024.0}) {
        for (const double origin : {-3000.0, -1024.0, -511.0, -1.0, 0.0, 300.0, 1919.0, 1920.0}) {
            const LevelPlacement q{origin, origin * 0.5, tilePixels};
            const bool any = VisibleTiles(q, 7, 5, 1920, 1080, tx0, ty0, tx1, ty1);
            for (int ty = 0; ty < 5; ++ty) {
                for (int tx = 0; tx < 7; ++tx) {
                    const ScreenRect r = TileScreenRect(q, tx, ty);
                    const bool onScreen = r.right > 0 && r.left < 1920 && r.bottom > 0 && r.top < 1080;
                    const bool listed = any && tx >= tx0 && tx < tx1 && ty >= ty0 && ty < ty1;
                    CHECK(onScreen == listed);
                }
            }
        }
    }
}

TEST_CASE(DirtyRectsMergeTilesThatArriveTogether)
{
    DirtyRects dirty;
    CHECK(dirty.Empty());
    dirty.Add({0, 0, 512, 512});
    dirty.Add({512, 0, 1024, 512});  // next to the first: one rectangle
    CHECK(dirty.Count() == 1);
    CHECK(dirty[0].left == 0 && dirty[0].right == 1024 && dirty[0].top == 0 && dirty[0].bottom == 512);
    dirty.Add({100, 100, 200, 200});  // inside
    CHECK(dirty.Count() == 1);
    dirty.Add({0, 1024, 512, 1536});  // apart: joining would redraw pixels nobody asked for
    CHECK(dirty.Count() == 2);
    dirty.Add({10, 10, 10, 50});  // empty
    CHECK(dirty.Count() == 2);
    CHECK(!dirty.WholeView());

    // Too many separate rectangles become one full redraw.
    dirty.Clear();
    for (int i = 0; i < DirtyRects::kCapacity; ++i) {
        dirty.Add({i * 100, i * 100, i * 100 + 10, i * 100 + 10});
    }
    CHECK(dirty.Count() == DirtyRects::kCapacity && !dirty.WholeView());
    dirty.Add({5000, 0, 5010, 10});
    CHECK(dirty.WholeView() && dirty.Count() == 0 && !dirty.Empty());
    dirty.Clear();
    CHECK(dirty.Empty());
}

TEST_CASE(TileKeysHashAndCompareByEveryField)
{
    const TileKeyEqual equal;
    const TileKeyHash hash;
    TileKey a = Key(3, 4);
    TileKey b = Key(3, 4);
    CHECK(equal(a, b) && hash(a) == hash(b));
    b.tx = 4;
    CHECK(!equal(a, b));
    b = a;
    b.zoom = 3;
    CHECK(!equal(a, b));
    b = a;
    b.rotation = 0;
    CHECK(!equal(a, b));
    b = a;
    b.cityKey = "another";
    CHECK(!equal(a, b));
    CHECK(hash(Key(0, 1)) != hash(Key(1, 0)));
}

TEST_CASE(TileLruCountsBytesAndEvictsTheLeastRecentlyDrawn)
{
    constexpr size_t kMB = 1024 * 1024;
    TileLru<int> lru;
    lru.SetBudget(4 * kMB);
    std::vector<int> evicted;
    auto onEvict = [&](const int& value) { evicted.push_back(value); };

    // Four tiles of 1 MB, drawn in passes 1 to 4.
    for (int i = 0; i < 4; ++i) {
        CHECK(lru.MakeRoom(kMB, 100, onEvict));
        lru.Insert(Key(i, 0), i, kMB, static_cast<uint64_t>(i) + 1);
    }
    CHECK(lru.Count() == 4 && lru.Bytes() == 4 * kMB && evicted.empty());

    // A fifth tile evicts the one drawn longest ago.
    CHECK(lru.MakeRoom(kMB, 100, onEvict));
    CHECK(evicted.size() == 1 && evicted[0] == 0);
    CHECK(lru.Bytes() == 3 * kMB);
    lru.Insert(Key(4, 0), 4, kMB, 5);
    CHECK(lru.Find(Key(0, 0), 6) == nullptr);

    // Drawing a tile makes it recent: tile 1 is used in pass 6, so tile 2 goes next.
    CHECK(lru.Find(Key(1, 0), 6) != nullptr && *lru.Find(Key(1, 0), 6) == 1);
    CHECK(lru.MakeRoom(kMB, 100, onEvict));
    CHECK(evicted.size() == 2 && evicted[1] == 2);
    lru.Insert(Key(5, 0), 5, kMB, 6);
    CHECK(lru.Bytes() == 4 * kMB);

    // Tiles drawn in the pass that asks for the new tile are never evicted: tiles 1 and 5
    // (pass 6) stay, tiles 3 (pass 4) and 4 (pass 5) may go.
    CHECK(lru.MakeRoom(2 * kMB, 6, onEvict));
    CHECK(evicted.size() == 4 && evicted[2] == 3 && evicted[3] == 4);
    CHECK(!lru.MakeRoom(3 * kMB, 6, onEvict));
    CHECK(evicted.size() == 4 && lru.Count() == 2 && lru.Bytes() == 2 * kMB);

    // A tile larger than the whole budget never fits and evicts nothing.
    CHECK(!lru.MakeRoom(5 * kMB, 100, onEvict));
    CHECK(lru.Count() == 2);

    // A smaller budget trims the cache.
    lru.SetBudget(kMB);
    CHECK(lru.MakeRoom(0, UINT64_MAX, onEvict));
    CHECK(lru.Count() == 1 && lru.Bytes() == kMB && evicted.size() == 5);

    lru.Clear(onEvict);
    CHECK(lru.Count() == 0 && lru.Bytes() == 0 && evicted.size() == 6);
}
