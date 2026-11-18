#include <cstdint>
#include <vector>

#include "TestRunner.hpp"
#include "overlay/OverlayMath.hpp"

using namespace OverlayMath;

namespace
{
    struct Vert
    {
        float x, y, z;
        uint32_t color;
        float u, v;
    };

    double MeshArea(const std::vector<Vert>& verts)
    {
        double area = 0;
        for (size_t i = 0; i + 2 < verts.size(); i += 3) {
            const Vert &a = verts[i], &b = verts[i + 1], &c = verts[i + 2];
            area += std::abs((b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z)) * 0.5;
        }
        return area;
    }

    HeightGrid FlatGrid(const int cells, const float height)
    {
        HeightGrid g;
        g.cellsX = g.cellsZ = cells;
        g.flat = true;
        g.flatHeight = height;
        return g;
    }
}

TEST_CASE(OverlaySplitSmallImageIsOnePaddedPiece)
{
    const std::vector<Piece> pieces = SplitImage(1000, 600);
    CHECK(pieces.size() == 1);
    CHECK(pieces[0].texW == 1024 && pieces[0].texH == 1024);
    CHECK_NEAR(pieces[0].u0, 1.0 / 1024, 1e-7);
    CHECK_NEAR(pieces[0].u1, 1001.0 / 1024, 1e-7);
    CHECK_NEAR(pieces[0].v1, 601.0 / 1024, 1e-7);
    CHECK(TextureBytes(pieces) == size_t{1024} * 1024 * 8);
}

TEST_CASE(OverlaySplitLargeImageCoversEveryPixelOnce)
{
    const int w = 5000, h = 2100;
    const std::vector<Piece> pieces = SplitImage(w, h);
    CHECK(pieces.size() == 3 * 2);
    long long covered = 0;
    for (const Piece& p : pieces) {
        CHECK(p.texW <= kMaxTexture && p.texH <= kMaxTexture);
        CHECK((p.texW & (p.texW - 1)) == 0 && (p.texH & (p.texH - 1)) == 0);
        CHECK(p.w + 2 * kGutter <= p.texW && p.h + 2 * kGutter <= p.texH);
        covered += static_cast<long long>(p.w) * p.h;
    }
    CHECK(covered == static_cast<long long>(w) * h);
    CHECK(pieces[1].x0 == kMaxContent && pieces[3].y0 == kMaxContent);
    CHECK(pieces[2].w == w - 2 * kMaxContent);
}

TEST_CASE(OverlayBudgetScalesDownAndKeepsAspect)
{
    int w = 0, h = 0;
    FitBudget(1000, 500, kTextureBudgetBytes, w, h);
    CHECK(w == 1000 && h == 500);
    FitBudget(8000, 6000, kTextureBudgetBytes, w, h);
    CHECK(TextureBytes(SplitImage(w, h)) <= kTextureBudgetBytes);
    CHECK(w < 8000 && w > 1000);
    CHECK_NEAR(static_cast<double>(h) / w, 0.75, 0.01);
}

TEST_CASE(OverlayReplicateEdgesFillsPadding)
{
    std::vector<uint8_t> rgba(4 * 4 * 4, 0);
    // Filled 2 x 1 at (1, 1): pixel values 10 and 20.
    rgba[(1 * 4 + 1) * 4] = 10;
    rgba[(1 * 4 + 2) * 4] = 20;
    ReplicateEdges(rgba.data(), 4, 4, 1, 1, 3, 2);
    CHECK(rgba[(0 * 4 + 0) * 4] == 10);
    CHECK(rgba[(3 * 4 + 3) * 4] == 20);
    CHECK(rgba[(2 * 4 + 1) * 4] == 10);
}

TEST_CASE(OverlayPlacementRoundTripWithRotation)
{
    const Placement p{500, 700, 300, 200, 30};
    for (const double a : {0.0, 0.25, 1.0}) {
        for (const double b : {0.0, 0.6, 1.0}) {
            double x, z, a2, b2;
            LocalToWorld(p, a, b, x, z);
            WorldToLocal(p, x, z, a2, b2);
            CHECK_NEAR(a2, a, 1e-9);
            CHECK_NEAR(b2, b, 1e-9);
        }
    }
    // Rotation 90: the image's +a direction runs along world +Z.
    double x, z;
    LocalToWorld({0, 0, 100, 50, 90}, 1.0, 0.5, x, z);
    CHECK_NEAR(x, 0, 1e-9);
    CHECK_NEAR(z, 50, 1e-9);
}

TEST_CASE(OverlayFitToAreaKeepsAspectAndRotation)
{
    Placement p{0, 0, 400, 200, 0};
    p = FitToArea(p, 1024, 1024);
    CHECK_NEAR(p.cx, 512, 1e-9);
    CHECK_NEAR(p.width, 1024, 1e-9);
    CHECK_NEAR(p.height, 512, 1e-9);
    p.rotationDeg = 45;
    p = FitToArea(p, 1024, 1024);
    double minX, minZ, maxX, maxZ;
    Bounds(p, 0, 0, 1, 1, minX, minZ, maxX, maxZ);
    CHECK(minX >= -1e-6 && maxX <= 1024 + 1e-6 && minZ >= -1e-6 && maxZ <= 1024 + 1e-6);
    CHECK_NEAR(p.width / p.height, 2.0, 1e-9);
}

TEST_CASE(OverlayScaleAboutKeepsThePointFixed)
{
    const Placement p{100, 100, 50, 50, 20};
    double a, b;
    WorldToLocal(p, 90, 110, a, b);
    const Placement q = ScaleAbout(p, 90, 110, 3.0);
    double x, z;
    LocalToWorld(q, a, b, x, z);
    CHECK_NEAR(x, 90, 1e-9);
    CHECK_NEAR(z, 110, 1e-9);
    CHECK_NEAR(q.width, 150, 1e-9);
}

TEST_CASE(OverlayCellRangeClipsToTheCity)
{
    int cx0, cz0, cx1, cz1;
    CHECK(CellRange({0, 0, 100, 100, 0}, 64, 64, cx0, cz0, cx1, cz1));
    CHECK(cx0 == 0 && cz0 == 0 && cx1 == 4 && cz1 == 4);
    CHECK(!CellRange({-500, -500, 100, 100, 0}, 64, 64, cx0, cz0, cx1, cz1));
}

TEST_CASE(OverlayMeshCoversTheRectangleExactly)
{
    // A 100 x 60 m image turned by 30 degrees, well inside a 64 cell city.
    const Placement p{512, 512, 100, 60, 30};
    const std::vector<Piece> pieces = SplitImage(400, 240);
    HeightGrid grid = FlatGrid(64, 12.0f);
    std::vector<Vert> verts;
    std::vector<Chunk> chunks;
    BuildMesh(p, 400, 240, pieces, grid, 0.05f, 0x80FFFFFFu, verts, chunks);
    CHECK(!verts.empty() && verts.size() % 3 == 0);
    CHECK_NEAR(MeshArea(verts), 100.0 * 60.0, 0.5);
    for (const Vert& v : verts) {
        double a, b;
        WorldToLocal(p, v.x, v.z, a, b);
        CHECK(a > -1e-4 && a < 1 + 1e-4 && b > -1e-4 && b < 1 + 1e-4);
        CHECK(v.u >= pieces[0].u0 - 1e-5f && v.u <= pieces[0].u1 + 1e-5f);
        CHECK_NEAR(v.y, 12.05, 1e-4);
        CHECK(v.color == 0x80FFFFFFu);
    }
    uint32_t total = 0;
    for (const Chunk& c : chunks) {
        total += c.count;
        CHECK(c.count <= 2046);
    }
    CHECK(total == verts.size());
}

TEST_CASE(OverlayMeshIsClippedAtTheCityEdge)
{
    // Half the image hangs over the west edge (x < 0).
    const Placement p{0, 100, 64, 32, 0};
    HeightGrid grid = FlatGrid(16, 0.0f);
    std::vector<Vert> verts;
    std::vector<Chunk> chunks;
    BuildMesh(p, 64, 32, SplitImage(64, 32), grid, 0.0f, 0xFFFFFFFFu, verts, chunks);
    CHECK_NEAR(MeshArea(verts), 32.0 * 32.0, 1e-3);
}

TEST_CASE(OverlayMeshFollowsTheTerrainTriangles)
{
    // One cell, heights 0 at (0,0), 0 at (1,0), 0 at (0,1), 16 at (1,1). Not flipped, the
    // diagonal runs from (0,0) to (1,1), so the point (8, 4) is on the triangle (0,0) (1,1)
    // (1,0) with height 4.
    HeightGrid grid;
    grid.cellsX = grid.cellsZ = 1;
    grid.heights = {0, 0, 0, 16};
    grid.flipped = {0};
    const Placement p{8, 4, 0.5, 0.5, 0};
    std::vector<Vert> verts;
    std::vector<Chunk> chunks;
    BuildMesh(p, 4, 4, SplitImage(4, 4), grid, 0.0f, 0xFFFFFFFFu, verts, chunks);
    CHECK(!verts.empty());
    for (const Vert& v : verts) {
        CHECK_NEAR(v.y, v.z, 1e-4);  // that triangle is the plane y = z
    }
    // Flipped: the diagonal runs from (1,0) to (0,1). x + z < 16, so the point is on the
    // triangle (0,0) (0,1) (1,0).
    grid.flipped = {1};
    BuildMesh(p, 4, 4, SplitImage(4, 4), grid, 0.0f, 0xFFFFFFFFu, verts, chunks);
    for (const Vert& v : verts) {
        CHECK_NEAR(v.y, 0.0, 1e-4);  // the triangle (0,0) (0,1) (1,0) is flat at 0
    }
}

TEST_CASE(OverlayJsonRoundTrip)
{
    State s;
    s.imagePath = "C:\\Maps\\K\xC3\xB6ln.png";
    s.relativePath = "maps/plan.png";
    s.placement = {1234.5, 678.25, 2000, 1500, -45};
    s.keepAspect = false;
    s.opacity = 0.35f;
    s.visible = false;
    s.followTerrain = false;
    s.flatHeight = 271.5f;
    s.overBuildings = true;
    const std::optional<State> r = FromJson(ToJson(s));
    CHECK(r.has_value());
    CHECK(r->imagePath == s.imagePath && r->relativePath == s.relativePath);
    CHECK_NEAR(r->placement.cx, 1234.5, 1e-9);
    CHECK_NEAR(r->placement.rotationDeg, -45, 1e-9);
    CHECK_NEAR(r->opacity, 0.35, 1e-6);
    CHECK(!r->keepAspect && !r->visible && !r->followTerrain && r->overBuildings);
    CHECK_NEAR(r->flatHeight, 271.5, 1e-6);

    CHECK(!FromJson("not json").has_value());
    CHECK(!FromJson(R"({"formatVersion": 2})").has_value());
    CHECK(!FromJson(R"({"formatVersion": 1, "width": "wide"})").has_value());
    CHECK(FromJson(R"({"formatVersion": 1})").has_value());
}
