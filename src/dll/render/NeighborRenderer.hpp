#pragma once

// Render (W2-E): draws the neighbor cities from their tiles.
//
// Back neighbors at the start of PreStatic, front neighbors at the end of PostStatic with
// depth always passing and written (docs/plan.md "Draw order"). Neighbors sorted back to
// front. Marks a dirty rectangle when a tile arrives. Each zoom draws the neighbor's level of
// that zoom. A zoom without one takes a level of the same pitch drawn larger
// (CityMeta::ChooseLevel): zoom 5 without zoom 5 tiles shows zoom 4 at 2x. Else nothing.
// Placement: neighbor canvas = ViewMath canvas shifted by
// projection.WorldToCanvas(RegionIndex::OriginOffset(city)), screen = canvas +
// CameraReader::CanvasToScreenOffset(), rounded to the game's pixel grid.
//
// Tiles are drawn as world-space quads on a plane that faces the camera, through the game's
// own draw context and camera, so partial view updates (dirty rectangles, pan strips) need
// no special case. The math and the reverse engineering are in docs/re-notes/render.md.

#include <array>
#include <string>
#include <vector>

#include "cS3DVector3.h"
#include "core/GameAccess.hpp"
#include "public/cIGZDrawService.h"
#include "public/cIGZWorldDrawService.h"
#include "render/NeighborMath.hpp"
#include "store/TileStore.hpp"
#include "view/ViewMath.hpp"

struct CameraFrame;

class NeighborRenderer final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Neighbors"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // false stops all neighbor drawing at once and redraws the view without neighbors
    // (the capture module uses it while it renders).
    void SetEnabled(bool enabled);
    [[nodiscard]] bool Enabled() const { return enabled_; }
    // true: no neighbor drawing and no tile cache while a region render runs, whatever
    // SetEnabled says. Each city of the batch would load neighbor tiles for nothing.
    void SetHeld(bool held);
    // Reads every meta.json again, drops the cached tiles and redraws everything (new
    // renders, settings change).
    void InvalidateAll();

private:
    // One slot per view: rotation * kZoomCount + zoom index of the view.
    static constexpr int kLevelSlots = 4 * ViewMath::kZoomCount;

    struct Neighbor
    {
        std::string name;
        NeighborMath::CellRect cells;
        cS3DVector3 originOffset;  // metres, relative to the current city
        int tileSize = 512;
        // Per view: the level drawn there and its scale (screen pixels per tile pixel at
        // magnification 1). Scale 0: nothing to draw in that view.
        std::array<CityRenderMeta::Level, kLevelSlots> levels{};
        std::array<double, kLevelSlots> scale{};
        uint8_t frontMask = 0;     // bit r: drawn in front of the current city at rotation r
        TileKey key;               // cityKey is fixed, the rest changes per tile (no allocation)
        int visibleTiles = 0;      // last draw, for the panel
        int readyTiles = 0;
    };

    struct SavedState
    {
        uint32_t packed = 0;
        uint32_t texture = 0;
        int wrapU = 0, wrapV = 0, filterA = 0, filterB = 0;
        int depthOffset = 0;
        bool fog = false;
        bool lighting = false;
    };

    static void PassCallback_(DrawServicePass pass, bool begin, void* userData);
    [[nodiscard]] bool ShouldDraw_() const;
    void Tick_();
    void DrawGroup_(bool front);
    void BeginState_(SC4DrawContextHandle context, bool front, bool linear, SavedState& saved) const;
    void EndState_(SC4DrawContextHandle context, const SavedState& saved) const;
    void Rebuild_();
    bool Place_(const Neighbor& neighbor, const CameraFrame& frame, NeighborMath::LevelPlacement& placement,
                const CityRenderMeta::Level*& level) const;
    void OnTileReady_(const TileKey& key);
    // Adds the screen rectangle of a tile that became ready to dirty_.
    void MarkReadyTile_(const TileKey& key, const CameraFrame& frame);
    void FlushDirty_();
    void RequestFullRedraw_() const;

    ModuleContext ctx_;
    bool initialized_ = false;
    bool enabled_ = true;
    bool held_ = false;
    bool cityActive_ = false;
    bool outlineTiles_ = false;
    bool shownNeighbors_ = true;
    bool shownFront_ = true;
    bool indexReady_ = false;
    uint32_t settingsRevision_ = 0;
    uint32_t regionGeneration_ = 0;
    Season season_ = Season::Main;  // of the open city's date, at the last Rebuild_
    uint32_t deviceGeneration_ = 0;
    std::array<uint32_t, 3> passTokens_{};

    std::vector<Neighbor> neighbors_;
    // Per rotation: indices into neighbors_, back to front.
    std::array<std::vector<int>, 4> back_;
    std::array<std::vector<int>, 4> front_;
    NeighborMath::DirtyRects dirty_;
    std::vector<TileKey> ready_;  // tiles that became ready since the last PreDynamic
    std::vector<SC4WorldVertex> outline_;  // reserved once
};
