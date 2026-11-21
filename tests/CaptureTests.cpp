// Offline tests for the pure parts of the capture module (capture/CaptureMath.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "TestRunner.hpp"
#include "capture/CaptureMath.hpp"
#include "view/ViewMath.hpp"

using CaptureMath::Rgb;

namespace
{
    void Put(std::vector<uint8_t>& image, const size_t pixel, const int r, const int g, const int b, const int a = 255)
    {
        image[pixel * 4 + 0] = static_cast<uint8_t>(r);
        image[pixel * 4 + 1] = static_cast<uint8_t>(g);
        image[pixel * 4 + 2] = static_cast<uint8_t>(b);
        image[pixel * 4 + 3] = static_cast<uint8_t>(a);
    }

    // What the renderer gives for a pixel of `color` with coverage alpha over the clear color.
    int Blend(const int color, const int key, const int alpha)
    {
        return (color * alpha + key * (255 - alpha) + 127) / 255;
    }
}

TEST_CASE(CaptureConvertSwapsRedAndBlueAndHonorsTheStride)
{
    // 2 x 2 pixels, rows of 12 bytes (4 bytes of padding), bytes B G R X.
    const std::vector<uint8_t> src{1, 2, 3, 0, 4, 5, 6, 77, 99, 99, 99, 99, 7, 8, 9, 0, 10, 11, 12, 0, 99, 99, 99, 99};
    std::vector<uint8_t> dst(16, 0);
    CaptureMath::ConvertBgraToRgba(src.data(), 12, 2, 2, dst.data());
    const std::vector<uint8_t> expected{3, 2, 1, 255, 6, 5, 4, 255, 9, 8, 7, 255, 12, 11, 10, 255};
    CHECK(dst == expected);
}

TEST_CASE(CaptureBlitPlacesAPartInsideTheTile)
{
    std::vector<uint8_t> tile(4 * 4 * 4, 0);
    const std::vector<uint8_t> part(2 * 2 * 4, 200);
    CaptureMath::BlitRgba(part.data(), 2, 2, tile.data(), 4, 1, 2);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool inside = x >= 1 && x < 3 && y >= 2;
            CHECK(tile[(y * 4 + x) * 4] == (inside ? 200 : 0));
        }
    }
}

TEST_CASE(CaptureCountKeyPixelsIsExact)
{
    std::vector<uint8_t> image(4 * 4);
    Put(image, 0, 255, 0, 255);
    Put(image, 1, 255, 0, 254);
    Put(image, 2, 255, 0, 255, 0);  // alpha is ignored
    Put(image, 3, 0, 0, 0);
    CHECK(CaptureMath::CountKeyPixels(image.data(), 4, Rgb{255, 0, 255}) == 2);
}

TEST_CASE(CaptureKeyContrastPicksTheStrongestChannel)
{
    int channel = -1;
    int contrast = 0;
    CaptureMath::KeyContrast(Rgb{255, 0, 255}, Rgb{0, 255, 0}, channel, contrast);
    CHECK(std::abs(contrast) == 255);
    // A 16 bit back buffer gives the keys back with fewer bits. Green keeps the most.
    CaptureMath::KeyContrast(Rgb{248, 0, 248}, Rgb{8, 252, 0}, channel, contrast);
    CHECK(channel == 1);
    CHECK(contrast == 252);
    CaptureMath::KeyContrast(Rgb{10, 20, 30}, Rgb{10, 20, 30}, channel, contrast);
    CHECK(contrast == 0);
}

TEST_CASE(CaptureTwoPassesSeparateCityFromBackground)
{
    const Rgb keyA{255, 0, 255};
    const Rgb keyB{0, 255, 0};
    std::vector<uint8_t> a(5 * 4), b(5 * 4);
    // 0: background. 1: an opaque city pixel that has the key color. 2: an opaque pixel.
    // 3: a half transparent grey pixel over the background. 4: an opaque pixel with render noise.
    Put(a, 0, 255, 0, 255);
    Put(b, 0, 0, 255, 0);
    Put(a, 1, 255, 0, 255);
    Put(b, 1, 255, 0, 255);
    Put(a, 2, 10, 120, 30);
    Put(b, 2, 10, 120, 30);
    Put(a, 3, Blend(100, 255, 128), Blend(100, 0, 128), Blend(100, 255, 128));
    Put(b, 3, Blend(100, 0, 128), Blend(100, 255, 128), Blend(100, 0, 128));
    Put(a, 4, 50, 60, 70);
    Put(b, 4, 52, 66, 69);

    const size_t visible = CaptureMath::ComposeTwoPasses(a.data(), b.data(), 5, keyA, keyB);
    CHECK(visible == 4);
    CHECK(a[3] == 0 && a[0] == 0 && a[1] == 0 && a[2] == 0);
    CHECK(a[4 + 3] == 255 && a[4 + 0] == 255 && a[4 + 1] == 0 && a[4 + 2] == 255);
    CHECK(a[8 + 3] == 255 && a[8 + 1] == 120);
    CHECK_NEAR(a[12 + 3], 128, 2);
    CHECK_NEAR(a[12 + 0], 100, 3);
    CHECK_NEAR(a[12 + 1], 100, 3);
    CHECK_NEAR(a[12 + 2], 100, 3);
    CHECK(a[16 + 3] == 255 && a[16 + 0] == 50);
}

TEST_CASE(CaptureTwoPassesWorkWithQuantizedKeys)
{
    // Keys as a 5:6:5 back buffer gives them back, with the contrast on a channel that falls.
    const Rgb keyA{248, 252, 0};
    const Rgb keyB{248, 0, 248};
    std::vector<uint8_t> a(2 * 4), b(2 * 4);
    Put(a, 0, 248, 252, 0);
    Put(b, 0, 248, 0, 248);
    Put(a, 1, 248, 252, 0);  // a city pixel in the first key color
    Put(b, 1, 248, 252, 0);
    CHECK(CaptureMath::ComposeTwoPasses(a.data(), b.data(), 2, keyA, keyB) == 1);
    CHECK(a[3] == 0);
    CHECK(a[7] == 255);
}

TEST_CASE(CaptureOutlineHoldsTheCityAndRejectsFarTiles)
{
    for (int rotation = 0; rotation < 4; ++rotation) {
        for (int zoom = 0; zoom < ViewMath::kZoomCount; ++zoom) {
            const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(zoom, rotation);
            const float size = 1024.0f;
            const CaptureMath::CityOutline outline = CaptureMath::CityOutline::For(p, size, size, 0.0f, 300.0f);
            CHECK(outline.count >= 4 && outline.count <= 8);

            // Every point of the city box lies in a one pixel square that touches the outline.
            for (const float h : {0.0f, 150.0f, 300.0f}) {
                for (const float x : {0.0f, 512.0f, 1024.0f}) {
                    for (const float z : {0.0f, 512.0f, 1024.0f}) {
                        const cS3DVector2 c = p.WorldToCanvas(cS3DVector3(x, h, z));
                        CHECK(outline.Intersects(c.fX - 0.5f, c.fY - 0.5f, c.fX + 0.5f, c.fY + 0.5f));
                    }
                }
            }
            // Points well outside the city on the ground, and one high above it. A ground point
            // close behind the city is hidden by the 300 m box, so these are far enough for
            // the flattest pitch (30 degrees).
            for (const cS3DVector3& far : {cS3DVector3(-800.0f, 0.0f, -800.0f), cS3DVector3(2500.0f, 0.0f, 512.0f),
                                           cS3DVector3(512.0f, 1500.0f, 512.0f)}) {
                const cS3DVector2 c = p.WorldToCanvas(far);
                CHECK(!outline.Intersects(c.fX - 0.5f, c.fY - 0.5f, c.fX + 0.5f, c.fY + 0.5f));
            }

            // The grid corners that the diamond leaves empty are rejected.
            const ViewMath::TileGrid grid = ViewMath::TileGrid::ForCity(p, 4096.0f, 4096.0f, 0.0f, 300.0f, 512);
            const CaptureMath::CityOutline large = CaptureMath::CityOutline::For(p, 4096.0f, 4096.0f, 0.0f, 300.0f);
            int touched = 0;
            for (int ty = 0; ty < grid.tilesY; ++ty) {
                for (int tx = 0; tx < grid.tilesX; ++tx) {
                    const cS3DVector2 o = grid.TileOrigin(tx, ty);
                    touched += large.Intersects(o.fX, o.fY, o.fX + 512.0f, o.fY + 512.0f) ? 1 : 0;
                }
            }
            CHECK(touched > 0);
            if (grid.tilesX * grid.tilesY >= 16) {
                CHECK(touched < grid.tilesX * grid.tilesY);
            }
        }
    }
}

TEST_CASE(CapturePlanCoversTheBlockOnce)
{
    std::vector<CaptureMath::TileRead> reads;
    CaptureMath::PlanReads(512, 512, 1920, 1080, reads);
    CHECK(reads.size() == 1);
    CHECK(reads[0].x == 0 && reads[0].y == 0 && reads[0].w == 512 && reads[0].h == 512);

    CaptureMath::PlanReads(1536, 1024, 1920, 1080, reads);
    CHECK(reads.size() == 1);
    CHECK(reads[0].w == 1536 && reads[0].h == 1024);

    // A window smaller than a tile, and a wide window lower than a tile.
    for (const auto& [bw, bh, vw, vh] : {std::array{512, 512, 400, 300}, std::array{1536, 512, 1920, 400}}) {
        CaptureMath::PlanReads(bw, bh, vw, vh, reads);
        CHECK(reads.size() > 1);
        std::vector<int> covered(static_cast<size_t>(bw) * bh, 0);
        for (const CaptureMath::TileRead& r : reads) {
            CHECK(r.w <= vw && r.h <= vh && r.w > 0 && r.h > 0);
            for (int y = r.y; y < r.y + r.h; ++y) {
                for (int x = r.x; x < r.x + r.w; ++x) {
                    ++covered[static_cast<size_t>(y) * bw + x];
                }
            }
        }
        CHECK(std::ranges::all_of(covered, [](const int c) { return c == 1; }));
    }

    CaptureMath::PlanReads(512, 512, 0, 300, reads);
    CHECK(reads.empty());
}

TEST_CASE(CaptureBlockSizeIsTheWholeTilesOfTheWindow)
{
    int w = 0, h = 0;
    CaptureMath::BlockSize(512, 1920, 1080, w, h);
    CHECK(w == 3 && h == 2);
    CaptureMath::BlockSize(512, 1280, 720, w, h);
    CHECK(w == 2 && h == 1);
    CaptureMath::BlockSize(512, 512, 512, w, h);
    CHECK(w == 1 && h == 1);
    CaptureMath::BlockSize(512, 1023, 1025, w, h);
    CHECK(w == 1 && h == 2);
    CaptureMath::BlockSize(512, 400, 300, w, h);  // smaller than a tile: one tile in parts
    CHECK(w == 1 && h == 1);
    CaptureMath::BlockSize(512, 0, 0, w, h);
    CHECK(w == 1 && h == 1);
    CaptureMath::BlockSize(512, 3840, 2160, w, h);  // 4K: 7 x 4 fits the memory cap
    CHECK(w == 7 && h == 4);
    CaptureMath::BlockSize(512, 7680, 4320, w, h);  // 8K: 15 x 8, capped per side, then in memory
    CHECK(w <= CaptureMath::kMaxBlockSide && h <= CaptureMath::kMaxBlockSide);
    CHECK(static_cast<int64_t>(w) * h * 512 * 512 <= CaptureMath::kMaxBlockPixels && w * h >= 30);
    CaptureMath::BlockSize(256, 7680, 4320, w, h);  // small tiles: the side cap
    CHECK(w == CaptureMath::kMaxBlockSide && h == CaptureMath::kMaxBlockSide);
}

namespace
{
    // Plans every block of a grid. Checks that each waiting tile is in exactly one block, that
    // each block tile lies inside the grid and inside the read area of its block at the right
    // pixel offset, and that the read area fits the window. Returns the block count.
    int PlanAll(std::vector<uint8_t> pending, const int tilesX, const int tilesY, const int vw, const int vh)
    {
        constexpr int kTile = 512;
        const std::vector<uint8_t> wanted = pending;
        std::vector<int> seen(pending.size(), 0);
        CaptureMath::TileBlock block;
        int blocks = 0;
        while (CaptureMath::NextTileBlock(pending, tilesX, tilesY, kTile, vw, vh, block)) {
            ++blocks;
            CHECK(block.count > 0);
            CHECK(block.count <= block.w * block.h);
            if (vw >= kTile && vh >= kTile) {
                CHECK(block.w * kTile <= vw && block.h * kTile <= vh);
            }
            for (int i = 0; i < block.count; ++i) {
                const CaptureMath::BlockTile& t = block.tiles[i];
                CHECK(t.tx >= 0 && t.tx < tilesX && t.ty >= 0 && t.ty < tilesY);
                CHECK(t.tx >= block.tx && t.tx < block.tx + block.w && t.ty >= block.ty && t.ty < block.ty + block.h);
                CHECK(t.x == (t.tx - block.tx) * kTile && t.y == (t.ty - block.ty) * kTile);
                ++seen[static_cast<size_t>(t.ty) * tilesX + t.tx];
            }
            if (blocks > tilesX * tilesY) {
                CHECK(false);  // no progress
                break;
            }
        }
        for (size_t i = 0; i < wanted.size(); ++i) {
            CHECK(seen[i] == (wanted[i] ? 1 : 0));
        }
        return blocks;
    }
}

TEST_CASE(CaptureBlocksCoverEveryWaitingTileOnce)
{
    // A full 7 x 5 grid (odd sizes: the last column and row are partial blocks).
    CHECK(PlanAll(std::vector<uint8_t>(35, 1), 7, 5, 1920, 1080) == 9);  // 3 x 3 blocks of 3 x 2
    CHECK(PlanAll(std::vector<uint8_t>(35, 1), 7, 5, 512, 512) == 35);   // exactly one tile
    CHECK(PlanAll(std::vector<uint8_t>(35, 1), 7, 5, 400, 300) == 35);   // smaller than a tile
    CHECK(PlanAll(std::vector<uint8_t>(35, 1), 7, 5, 1280, 720) == 20);  // 2 x 1
    CHECK(PlanAll(std::vector<uint8_t>(1, 1), 1, 1, 1920, 1080) == 1);

    // A diamond like a projected city: only the tiles that touch the outline wait.
    const int n = 11;
    std::vector<uint8_t> diamond(static_cast<size_t>(n) * n, 0);
    int waiting = 0;
    for (int ty = 0; ty < n; ++ty) {
        for (int tx = 0; tx < n; ++tx) {
            if (std::abs(tx - n / 2) + std::abs(ty - n / 2) <= n / 2) {
                diamond[static_cast<size_t>(ty) * n + tx] = 1;
                ++waiting;
            }
        }
    }
    const int blocks = PlanAll(diamond, n, n, 1920, 1080);
    CHECK(blocks < waiting / 3);
    CHECK(PlanAll(diamond, n, n, 512, 512) == waiting);

    // Nothing waits.
    std::vector<uint8_t> none(35, 0);
    CaptureMath::TileBlock block;
    CHECK(!CaptureMath::NextTileBlock(none, 7, 5, 512, 1920, 1080, block));
    CHECK(block.count == 0);
}

TEST_CASE(CaptureBlockStartsAtItsFirstWaitingRow)
{
    // Row 0 waits only at column 5, rows 1 and 2 at columns 0..2: the block of 3 x 2 starts at
    // column 0 and moves down to row 1, so it holds the six tiles of rows 1 and 2.
    std::vector<uint8_t> pending(8 * 3, 0);
    pending[5] = 1;
    for (int tx = 0; tx < 3; ++tx) {
        pending[8 + tx] = 1;
        pending[16 + tx] = 1;
    }
    CaptureMath::TileBlock block;
    CHECK(CaptureMath::NextTileBlock(pending, 8, 3, 512, 1920, 1080, block));
    CHECK(block.tx == 0 && block.ty == 1 && block.w == 3 && block.h == 2);
    CHECK(block.count == 6);
    CHECK(pending[5] == 1);
    CHECK(CaptureMath::NextTileBlock(pending, 8, 3, 512, 1920, 1080, block));
    CHECK(block.tx == 5 && block.ty == 0 && block.count == 1);
    CHECK(!CaptureMath::NextTileBlock(pending, 8, 3, 512, 1920, 1080, block));
}

TEST_CASE(CaptureReturnedBlockIsPlannedAgainForANewWindow)
{
    std::vector<uint8_t> pending(6 * 4, 1);
    CaptureMath::TileBlock block;
    CHECK(CaptureMath::NextTileBlock(pending, 6, 4, 512, 1920, 1080, block));
    CHECK(block.count == 6);
    CaptureMath::ReturnTileBlock(pending, 6, block);
    CHECK(block.count == 0);
    CHECK(std::ranges::all_of(pending, [](const uint8_t p) { return p == 1; }));
    // The window got smaller: the same tiles in blocks of 2 x 1.
    CHECK(CaptureMath::NextTileBlock(pending, 6, 4, 512, 1280, 720, block));
    CHECK(block.tx == 0 && block.ty == 0 && block.w == 2 && block.h == 1 && block.count == 2);
}

TEST_CASE(CaptureCropAndCountInsideABlock)
{
    // A 4 x 2 image, the right 2 x 2 half holds the key in one pixel.
    std::vector<uint8_t> image(4 * 2 * 4, 9);
    Put(image, 3, 255, 0, 255);
    CHECK(CaptureMath::CountKeyPixelsIn(image.data(), 4, 2, 0, 2, 2, Rgb{255, 0, 255}) == 1);
    CHECK(CaptureMath::CountKeyPixelsIn(image.data(), 4, 0, 0, 2, 2, Rgb{255, 0, 255}) == 0);
    std::vector<uint8_t> tile(2 * 2 * 4, 0);
    CaptureMath::CropRgba(image.data(), 4, 2, 0, 2, 2, tile.data());
    CHECK(tile[4] == 255 && tile[5] == 0 && tile[6] == 255);
    CHECK(tile[0] == 9 && tile[8] == 9 && tile[12] == 9);
    // Crop is the inverse of Blit.
    std::vector<uint8_t> back(4 * 2 * 4, 0);
    CaptureMath::BlitRgba(tile.data(), 2, 2, back.data(), 4, 2, 0);
    CHECK(std::equal(back.begin() + 8, back.begin() + 16, image.begin() + 8));
}

TEST_CASE(CaptureTargetPutsTheTileOnWholeScreenPixels)
{
    // The snap of the game (ViewMath::GameCanvasToScreen) must put the tile corner exactly on
    // the wanted screen pixel, for every zoom, rotation, tile and window size.
    const float sizeX = 2048.0f, sizeZ = 4096.0f;
    for (int rotation = 0; rotation < 4; ++rotation) {
        for (int zoom = 0; zoom < ViewMath::kZoomCount; ++zoom) {
            const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(zoom, rotation);
            const ViewMath::TileGrid grid = ViewMath::TileGrid::ForCity(p, sizeX, sizeZ, 0.0f, 900.0f, 512);
            const cS3DVector3 base = ViewMath::BaseCorner(rotation, sizeX, sizeZ);
            for (const auto& [w, h] : {std::pair{1920, 1080}, std::pair{1280, 961}, std::pair{801, 600}}) {
                for (const auto& [tx, ty] : {std::pair{0, 0}, std::pair{grid.tilesX - 1, grid.tilesY - 1},
                                             std::pair{grid.tilesX / 2, grid.tilesY / 3}}) {
                    const cS3DVector2 origin = grid.TileOrigin(tx, ty);
                    const int wantX = (w - 512) / 2;
                    const int wantY = (h - 512) / 2;
                    const cS3DVector3 target = CaptureMath::TargetForCanvasAt(p, origin, wantX, wantY, w, h);
                    const cS3DVector2 toScreen =
                        ViewMath::GameCanvasToScreen(p, target, base, static_cast<float>(w), static_cast<float>(h));
                    CHECK_NEAR(origin.fX + toScreen.fX, wantX, 0.01);
                    CHECK_NEAR(origin.fY + toScreen.fY, wantY, 0.01);
                }
            }
        }
    }
}

// The levels of a render per max zoom: rotation outer, zoom inner, zoom 1 up to the max.
TEST_CASE(CaptureLevelOrderPerMaxZoom)
{
    for (int maxZoom = 2; maxZoom <= 4; ++maxZoom) {
        const std::vector<std::array<int, 2>> order = CaptureMath::LevelOrder(maxZoom, {});
        CHECK(order.size() == static_cast<size_t>(4 * (maxZoom + 1)));
        size_t i = 0;
        for (int r = 0; r < 4; ++r) {
            for (int z = 0; z <= maxZoom; ++z, ++i) {
                CHECK(i < order.size() && order[i][0] == r && order[i][1] == z);
            }
        }
    }
    // Only some rotations, and limits.
    const auto two = CaptureMath::LevelOrder(3, {2, 0, 7});
    CHECK(two.size() == 8 && two.front()[0] == 0 && two.back()[0] == 2 && two.back()[1] == 3);
    CHECK(CaptureMath::LevelOrder(9, {1}).size() == 5);
    CHECK(CaptureMath::LevelOrder(0, {1}).size() == 1);
}
