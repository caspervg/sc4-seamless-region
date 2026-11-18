#pragma once

// Interaction (W3-F): hover and click on neighbors outside the current city.
//
// - Input: a persistent cISC4ViewInputControl (cISC4View3DWin::AddPersistentViewInputControl).
//   The city view gives persistent controls the left button before the tool stack, in view
//   window pixels. The control only takes a click that starts on a neighbor while no tool
//   (or the query tool) is active, and acts on the button up when the mouse moved less than
//   4 pixels. Everything else goes on to the game untouched.
// - Hover: every frame (draw service, start of PreDynamic) the cursor position from the
//   window manager goes through CameraReader::ScreenToGround onto a surface at the height of
//   the nearest current city edge (InteractionMath::EdgeProfile), then to the region cell.
//   The game's own terrain and occupant picks reject points where the current city is in
//   front.
// - Tint: a cIGZWorldDrawService layer in PostDynamic holds the hovered neighbor's ground
//   footprint on that same surface, so the tint covers exactly the area a click takes.
// - Tooltip and confirm popup: a separate ImGui panel that only draws at the cursor.
// Facts and choices: docs/re-notes/interaction.md.

#include <cstdint>
#include <string>
#include <vector>

#include "core/GameAccess.hpp"
#include "interaction/InteractionMath.hpp"
#include "public/cIGZDrawService.h"
#include "public/cIGZWorldDrawService.h"

class NeighborPickerControl;

class NeighborPicker final : public Module
{
public:
    NeighborPicker();
    ~NeighborPicker() override;

    [[nodiscard]] const char* Name() const override { return "Interaction"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // From the view input control, in view window pixels. Return true to take the event.
    bool OnMouseDownL(int x, int z);
    void OnMouseMove(int x, int z);
    bool OnMouseUpL(int x, int z);
    void OnMouseExit();

private:
    struct Neighbor
    {
        int cellX = 0, cellZ = 0, sizeX = 1, sizeZ = 1;
        std::string name;
        const char* sizeLabel = "";
        bool founded = false;
        bool hasRender = false;
        bool stale = false;
        int64_t renderTime = 0;
        uint8_t frontMask = 0; // bit r: drawn in front of the current city at rotation r
    };

    struct Press
    {
        bool active = false;
        int x = 0, z = 0;
        int neighbor = -1;
        bool dragged = false;
    };

    static void PassCallback_(DrawServicePass pass, bool begin, void* userData);
    static void OverlayRender_(void* data);

    [[nodiscard]] bool Active_() const;
    // nullptr when a click may switch now, else why not (for the tooltip and the panel).
    [[nodiscard]] const char* ClickBlocker_() const;
    void Tick_();
    void Rebuild_();
    void SampleEdges_();
    // The neighbor index under a view position, or -1.
    [[nodiscard]] int Pick_(int x, int z);
    void SetHover_(int neighbor);
    void UpdateLayer_();
    void Activate_(int neighbor);
    void DrawOverlay_();
    void DrawTooltip_(const Neighbor& n);

    ModuleContext ctx_;
    bool initialized_ = false;
    bool cityActive_ = false;
    NeighborPickerControl* control_ = nullptr;
    bool controlAdded_ = false;
    uint32_t passToken_ = 0;
    uint32_t layer_ = 0;
    bool layerVisible_ = false;
    bool layerFront_ = false;
    bool overlayRegistered_ = false;

    InteractionMath::EdgeProfile profile_;
    std::vector<Neighbor> neighbors_;
    uint32_t regionGeneration_ = 0;
    uint32_t storeGeneration_ = 0;
    uint64_t lastRebuildMs_ = 0;

    // Hover. The pick only runs again when the cursor or the view changes.
    int hovered_ = -1;
    int meshFor_ = -1;
    bool cursorInView_ = false;
    int cursorX_ = 0, cursorZ_ = 0;
    float pickKey_[7] = {};
    std::vector<SC4WorldVertex> mesh_;

    Press press_;
    // Confirm popup for a city that cannot be saved.
    int confirmNeighbor_ = -1;
    bool confirmOpenRequested_ = false;

    // Panel.
    std::string lastHovered_;
    std::string lastClick_;
    const char* windowUnderCursor_ = "none";
};
