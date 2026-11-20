#include <cstring>
#include <vector>

#include "TestRunner.hpp"
#include "interaction/InteractionMath.hpp"
#include "view/ViewMath.hpp"

using namespace InteractionMath;

namespace
{
// A medium city (2048 m) with a north edge that rises 1 m per sample and flat other edges.
    EdgeProfile SlopedProfile()
    {
        EdgeProfile p;
        p.Reset(2048.0f, 2048.0f, 16.0f, 250.0f);
        for (size_t i = 0; i < p.north.size(); ++i) {
            p.north[i] = 250.0f + static_cast<float>(i);
        }
        p.west.assign(p.west.size(), 300.0f);
    // A corner is one terrain vertex, so both edges that meet there have its height.
        p.west[0] = p.north[0];
        p.east[0] = p.north.back();
        p.UpdateMean();
        return p;
    }

    CityInfo City(const int x, const int z, const int size, const bool current = false)
    {
        CityInfo c;
        c.cellX = x;
        c.cellZ = z;
        c.sizeX = size;
        c.sizeZ = size;
        c.founded = true;
        c.isCurrent = current;
        return c;
    }
}

TEST_CASE(EdgeHeightIsTheNearestEdgeSample)
{
    const EdgeProfile p = SlopedProfile();
    // North of the city: the north edge at the same x, between samples it is linear.
    CHECK_NEAR(p.Height(32.0f, -500.0f), 252.0f, 1e-4);
    CHECK_NEAR(p.Height(40.0f, -1.0f), 252.5f, 1e-4);
    // West of the city: the west edge.
    CHECK_NEAR(p.Height(-10.0f, 1000.0f), 300.0f, 1e-4);
    // Diagonal (north-west corner area): the corner sample.
    CHECK_NEAR(p.Height(-100.0f, -100.0f), p.north[0], 1e-4);
    // North-east corner area: the last north sample.
    CHECK_NEAR(p.Height(3000.0f, -100.0f), p.north.back(), 1e-4);
    // South and east edges keep the reset height.
    CHECK_NEAR(p.Height(1000.0f, 2100.0f), 250.0f, 1e-4);
    CHECK_NEAR(p.Height(2100.0f, 1000.0f), 250.0f, 1e-4);
    // Inside: the nearest edge.
    CHECK_NEAR(p.Height(1000.0f, 5.0f), p.Height(1000.0f, -5.0f), 1e-4);
}

TEST_CASE(PickGroundFindsThePointOnTheEdgeSurface)
{
    const EdgeProfile p = SlopedProfile();
    for (int rotation = 0; rotation < 4; ++rotation) {
        for (int zoom = 0; zoom < 5; ++zoom) {
            const ViewMath::OrthoProjection proj = ViewMath::OrthoProjection::For(zoom, rotation);
            // Points outside the city on the sloped north side and on the flat west side.
            const float points[][2] = {{600.0f, -300.0f}, {1500.0f, -20.0f}, {-700.0f, 900.0f}, {-50.0f, 600.0f}};
            for (const auto& xz : points) {
                const cS3DVector3 truth{xz[0], p.Height(xz[0], xz[1]), xz[1]};
                const cS3DVector2 canvas = proj.WorldToCanvas(truth);
                const cS3DVector3 found =
                    PickGround([&](const float h) { return proj.CanvasToGround(canvas, h); }, p, 8);
                CHECK_NEAR(found.fX, truth.fX, 0.05);
                CHECK_NEAR(found.fZ, truth.fZ, 0.05);
                CHECK_NEAR(found.fY, truth.fY, 0.05);
            }
        }
    }
}

TEST_CASE(HoverOnlyCountsNeighborsOutsideTheCity)
{
    // Current medium city at cells (2, 2) to (4, 4). A large city north, a small one east.
    std::vector<CityInfo> cities = {City(0, -2, 4) /*z -2..2*/, City(2, 2, 2, true), City(4, 2, 1)};
    RegionLayout::AssignKeys(cities);
    const CityInfo* current = RegionLayout::FindAtCell(cities, 2, 2);
    CHECK(current && current->isCurrent);

    CHECK(NeighborAt(cities, *current, 100.0f, 100.0f) == nullptr);   // inside
    CHECK(NeighborAt(cities, *current, 2047.0f, 2047.0f) == nullptr); // inside, last metre
    const CityInfo* north = NeighborAt(cities, *current, 100.0f, -1.0f);
    CHECK(north && north->cellX == 0 && north->cellZ == -2);
    const CityInfo* east = NeighborAt(cities, *current, 2048.5f, 10.0f);
    CHECK(east && east->cellX == 4);
    // East of the small city, row 3: no city there.
    CHECK(NeighborAt(cities, *current, 2100.0f, 1500.0f) == nullptr);
    // South: empty.
    CHECK(NeighborAt(cities, *current, 500.0f, 2100.0f) == nullptr);
}

TEST_CASE(DragThresholdMatchesTheGame)
{
    CHECK(!IsDrag(100, 100, 100, 100));
    CHECK(!IsDrag(100, 100, 103, 97));
    CHECK(IsDrag(100, 100, 104, 100));
    CHECK(IsDrag(100, 100, 100, 96));
}

TEST_CASE(FootprintFollowsTheEdgeAndSkipsTheCity)
{
    const EdgeProfile p = SlopedProfile();
    // A large neighbor north-west that covers the north-west corner area and half of the
    // north band: x -2048..2048, z -4096..0.
    std::vector<cS3DVector3> vertices;
    Footprint(p, -2048.0f, -4096.0f, 2048.0f, 0.0f,
              [&](const float x, const float y, const float z) { vertices.push_back({x, y, z}); });
    CHECK(vertices.size() % 6 == 0);
    // One flat corner quad and 2048 / 16 strips along the north edge.
    CHECK(vertices.size() == 6 * (1 + 128));
    bool heightsMatch = true;
    for (const cS3DVector3& v : vertices) {
        if (v.fZ > 0.0f || std::fabs(v.fY - p.Height(v.fX, -1.0f)) > 1e-3f) {
            heightsMatch = false;
        }
    }
    CHECK(heightsMatch);

    // A neighbor that is the same as the city area gives nothing.
    vertices.clear();
    Footprint(p, 0.0f, 0.0f, 2048.0f, 2048.0f,
              [&](const float x, const float y, const float z) { vertices.push_back({x, y, z}); });
    CHECK(vertices.empty());
}

TEST_CASE(RenderAgeTextIsFriendly)
{
    char text[64];
    RenderAgeText(10, text, sizeof(text));
    CHECK(std::strcmp(text, "Rendered just now") == 0);
    RenderAgeText(60, text, sizeof(text));
    CHECK(std::strcmp(text, "Rendered 1 minute ago") == 0);
    RenderAgeText(3 * 86400 + 5, text, sizeof(text));
    CHECK(std::strcmp(text, "Rendered 3 days ago") == 0);
    RenderAgeText(7200, text, sizeof(text));
    CHECK(std::strcmp(text, "Rendered 2 hours ago") == 0);
}

TEST_CASE(DoubleClickNeedsTheSameNeighborSoonAndInPlace)
{
    using InteractionMath::Click;
    const Click first{3, 1000, 200, 300};
    CHECK(InteractionMath::IsDoubleClick(first, {3, 1400, 201, 302}, 500));
    CHECK(!InteractionMath::IsDoubleClick(first, {3, 1600, 200, 300}, 500));  // too late
    CHECK(!InteractionMath::IsDoubleClick(first, {4, 1100, 200, 300}, 500));  // another neighbor
    CHECK(!InteractionMath::IsDoubleClick(first, {3, 1100, 210, 300}, 500));  // moved
    CHECK(!InteractionMath::IsDoubleClick(Click(), {3, 1100, 200, 300}, 500));  // no first click
}

TEST_CASE(GroupedNumberLikeTheRegionView)
{
    char text[32];
    InteractionMath::GroupedNumber(0, text, sizeof(text));
    CHECK(std::string(text) == "0");
    InteractionMath::GroupedNumber(999, text, sizeof(text));
    CHECK(std::string(text) == "999");
    InteractionMath::GroupedNumber(1234567, text, sizeof(text));
    CHECK(std::string(text) == "1,234,567");
    InteractionMath::GroupedNumber(-12000, text, sizeof(text));
    CHECK(std::string(text) == "-12,000");
}
