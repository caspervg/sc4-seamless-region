#include "interaction/NeighborPicker.hpp"

#include <chrono>
#include <cmath>
#include <format>

#include <Windows.h>

#include "GZServPtrs.h"
#include "batch/RegionBatchRenderer.hpp"
#include "cIGZWin.h"
#include "cIGZWinMgr.h"
#include "cISC4City.h"
#include "cISC4Occupant.h"
#include "cISC4View3DWin.h"
#include "cISC4ViewInputControl.h"
#include "cISTETerrain.h"
#include "cSC4BaseViewInputControl.h"
#include "capture/CityCapturer.hpp"
#include "core/GameAddresses.hpp"
#include "core/Guard.hpp"
#include "imgui.h"
#include "interaction/InteractionAddresses.hpp"
#include "public/cIGZImGuiService.h"
#include "region/RegionIndex.hpp"
#include "render/NeighborMath.hpp"
#include "store/TileStore.hpp"
#include "switching/CitySwitcher.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"
#include "view/CameraReader.hpp"

// The persistent view input control. It forwards the left button and mouse moves to the
// picker. Everything it does not take returns false, so the tool stack gets it.
class NeighborPickerControl final : public cSC4BaseViewInputControl
{
public:
    explicit NeighborPickerControl(NeighborPicker* owner)
        : cSC4BaseViewInputControl(InteractionAddresses::kPickerControlId)
        , owner_(owner)
    {
    }

    void Detach() { owner_ = nullptr; }

    // The city view calls these: no exception may leave them.
    bool OnMouseDownL(const int32_t x, const int32_t z, uint32_t) override
    {
        bool taken = false;
        Guarded("Interaction: mouse down", [&] { taken = owner_ && owner_->OnMouseDownL(x, z); });
        return taken;
    }

    bool OnMouseUpL(const int32_t x, const int32_t z, uint32_t) override
    {
        bool taken = false;
        Guarded("Interaction: mouse up", [&] { taken = owner_ && owner_->OnMouseUpL(x, z); });
        return taken;
    }

    bool OnMouseMove(const int32_t x, const int32_t z, uint32_t) override
    {
        if (owner_) {
            owner_->OnMouseMove(x, z);
        }
        return false;
    }

    bool OnMouseExit() override
    {
        if (owner_) {
            owner_->OnMouseExit();
        }
        return false;
    }

private:
    NeighborPicker* owner_;
};

namespace
{
    constexpr float kEdgeStepMetres = 16.0f;
    constexpr float kRegionCellMetres = 1024.0f;
    constexpr uint32_t kTintColor = SC4WorldColor(0.55f, 0.80f, 1.0f, 0.2f);
    constexpr size_t kMeshReserve = 2048;
    constexpr const char* kConfirmPopup = "Leave this city?##SeamlessRegionConfirm";

    const SC4WorldStyle kBackStyle{.blend = SC4WorldBlend::Alpha, .depth = SC4WorldDepth::HiddenBehindScene};
    const SC4WorldStyle kFrontStyle{.blend = SC4WorldBlend::Alpha, .depth = SC4WorldDepth::AlwaysOnTop};

    uint64_t NowMs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    bool ImGuiWantsMouse(const ModuleContext& ctx)
    {
        return ctx.imgui && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    }

    const char* SizeLabel(const int size)
    {
        return size >= 4 ? "Large city" : size >= 2 ? "Medium city" : "Small city";
    }
}

NeighborPicker::NeighborPicker() = default;
NeighborPicker::~NeighborPicker() = default;

bool NeighborPicker::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.settings || !ctx_.region || !ctx_.cameraReader || !ctx_.switcher || !ctx_.capturer || !ctx_.tileStore) {
        LOG_WARN("Interaction: not active, a foundation object is missing");
        return false;
    }

    control_ = new NeighborPickerControl(this);
    control_->AddRef();
    mesh_.reserve(kMeshReserve);

    if (ctx_.draw) {
        if (!ctx_.draw->RegisterDrawPassCallback(DrawServicePass::PreDynamic, &NeighborPicker::PassCallback_, this,
                                                 &passToken_)) {
            passToken_ = 0;
        }
    }
    if (passToken_ == 0) {
        LOG_WARN("Interaction: no draw service callback. Hover, tint and tooltip are off, clicks still work.");
    }

    if (ctx_.worldDraw) {
        layer_ = ctx_.worldDraw->CreateLayer(DrawServicePass::PostDynamic, kBackStyle, 0);
        if (layer_ != 0) {
            ctx_.worldDraw->SetLayerVisible(layer_, false);
        }
    }
    if (layer_ == 0) {
        LOG_WARN("Interaction: no world draw layer. The hover tint is off.");
    }

    if (ctx_.imgui) {
        ImGuiPanelDesc desc{};
        desc.id = InteractionAddresses::kOverlayPanelId;
        desc.order = 1000;
        desc.visible = true;
        desc.on_render = &NeighborPicker::OverlayRender_;
        desc.data = this;
        overlayRegistered_ = ctx_.imgui->RegisterPanel(desc);
    }
    if (!overlayRegistered_) {
        LOG_WARN(
            "Interaction: no ImGui overlay. No tooltip, and a city that cannot be saved is never left by a click.");
    }

    initialized_ = true;
    LOG_INFO("Interaction: ready (view input control, hover in PreDynamic, tint layer in PostDynamic)");
    return true;
}

void NeighborPicker::Shutdown()
{
    if (cityActive_) {
        OnCityShutdown();
    }
    initialized_ = false;
    if (ctx_.draw && passToken_ != 0) {
        ctx_.draw->UnregisterDrawPassCallback(passToken_);
        passToken_ = 0;
    }
    if (ctx_.worldDraw && layer_ != 0) {
        ctx_.worldDraw->DestroyLayer(layer_);
        layer_ = 0;
    }
    if (ctx_.imgui && overlayRegistered_) {
        ctx_.imgui->UnregisterPanel(InteractionAddresses::kOverlayPanelId);
        overlayRegistered_ = false;
    }
    if (control_) {
        control_->Detach();
        control_->Release();
        control_ = nullptr;
    }
}

void NeighborPicker::OnCityInit()
{
    cityActive_ = true;
    SampleEdges_();
    Rebuild_();
    cISC4View3DWin* view = GameAccess::View3D();
    if (view && control_ && view->AddPersistentViewInputControl(control_)) {
        controlAdded_ = true;
        LOG_INFO("Interaction: click control added to the city view");
    }
    else {
        LOG_WARN("Interaction: could not add the click control to the city view. Clicks on neighbors are off.");
    }
}

void NeighborPicker::OnCityShutdown()
{
    if (controlAdded_) {
        if (cISC4View3DWin* view = GameAccess::View3D()) {
            view->RemovePersistentViewInputControl(control_);
        }
        controlAdded_ = false;
    }
    cityActive_ = false;
    press_ = {};
    firstClick_ = {};
    viewPointer_ = false;
    viewMovesLogged_ = false;
    confirmNeighbor_ = -1;
    confirmOpenRequested_ = false;
    SetHover_(-1);
    UpdateLayer_();
    neighbors_.clear();
    cursorInView_ = false;
}

bool NeighborPicker::Active_() const
{
    return initialized_ && cityActive_ && ctx_.settings->showNeighbors && !ctx_.switcher->IsBusy() &&
           !ctx_.capturer->IsRunning() && !(ctx_.batch && ctx_.batch->IsRunning());
}

const char* NeighborPicker::ClickBlocker_() const
{
    if (!Active_() || !controlAdded_ || !ctx_.settings->clickToSwitch || ImGuiWantsMouse(ctx_)) {
        return "";
    }
    if (confirmNeighbor_ >= 0 || confirmOpenRequested_) {
        return "Answer the question first.";
    }
    cISC4View3DWin* view = GameAccess::View3D();
    if (!view) {
        return "";
    }
    if (cISC4ViewInputControl* tool = view->GetCurrentViewInputControl()) {
        const uint32_t id = tool->GetID();
        if (id != InteractionAddresses::kQueryToolId && id != InteractionAddresses::kQueryToolIdAlt) {
            return "Close the tool you use to play this city.";
        }
    }
    return nullptr;
}

void NeighborPicker::SampleEdges_()
{
    cISC4City* city = GameAccess::City();
    const float sizeX = city ? city->SizeX() : 0.0f;
    const float sizeZ = city ? city->SizeZ() : 0.0f;
    cISTETerrain* terrain = city ? city->GetTerrain() : nullptr;
    float sea = terrain ? terrain->GetSeaLevel() : 0.0f;
    if (!std::isfinite(sea)) {
        sea = 0.0f;
    }
    profile_.Reset(std::max(sizeX, kEdgeStepMetres), std::max(sizeZ, kEdgeStepMetres), kEdgeStepMetres, sea);
    if (!ctx_.worldDraw || !(sizeX > 0.0f) || !(sizeZ > 0.0f)) {
        LOG_WARN("Interaction: no terrain heights, the edge height is the sea level ({})", sea);
        return;
    }

    // Just inside the edge: GetTerrainHeight is false outside the city.
    constexpr float kInside = 0.01f;
    int missing = 0;
    auto sample = [&](const float x, const float z) {
        float h = 0.0f;
        if (!ctx_.worldDraw->GetTerrainHeight(std::clamp(x, kInside, sizeX - kInside),
                                              std::clamp(z, kInside, sizeZ - kInside), h) ||
            !std::isfinite(h)) {
            ++missing;
            return sea;
        }
        return std::max(h, sea);
    };
    for (size_t i = 0; i < profile_.north.size(); ++i) {
        const float t = static_cast<float>(i) * kEdgeStepMetres;
        profile_.north[i] = sample(t, 0.0f);
        profile_.south[i] = sample(t, sizeZ);
    }
    for (size_t i = 0; i < profile_.west.size(); ++i) {
        const float t = static_cast<float>(i) * kEdgeStepMetres;
        profile_.west[i] = sample(0.0f, t);
        profile_.east[i] = sample(sizeX, t);
    }
    profile_.UpdateMean();
    if (missing > 0) {
        LOG_WARN("Interaction: {} edge heights were not available, the sea level is used there", missing);
    }
    LOG_DEBUG("Interaction: edge heights sampled, mean {:.1f} m", profile_.mean);
}

void NeighborPicker::Rebuild_()
{
    neighbors_.clear();
    regionGeneration_ = ctx_.region->Generation();
    storeGeneration_ = ctx_.tileStore->Generation();
    lastRebuildMs_ = NowMs();
    pickKey_[0] = NAN;
    meshFor_ = -1;
    hovered_ = -1;
    press_ = {};
    confirmNeighbor_ = -1;
    confirmOpenRequested_ = false;

    const CityInfo* current = ctx_.region->Current();
    if (!current) {
        return;
    }
    std::vector<NeighborMath::CellRect> rects;
    std::vector<int> owners;
    for (const CityInfo& city : ctx_.region->Cities()) {
        if (city.isCurrent) {
            continue;
        }
        Neighbor n;
        n.cellX = city.cellX;
        n.cellZ = city.cellZ;
        n.sizeX = city.sizeX;
        n.sizeZ = city.sizeZ;
        n.name = city.name;
        n.mayor = city.mayor;
        n.population = city.population;
        n.commercialJobs = city.commercialJobs;
        n.industrialJobs = city.industrialJobs;
        n.mayorRating = city.mayorRating;
        n.difficulty = city.difficulty;
        n.budget = city.budget;
        n.sizeLabel = SizeLabel(std::max(city.sizeX, city.sizeZ));
        n.founded = city.founded;
        if (city.founded) {
            // Like the neighbor renderer: per view the first render (season, then the others) with
            // a level for it. The age and staleness are those of the first.
            const std::vector<std::string> keys =
                ctx_.tileStore->StorageKeysInOrder(city.key, Seasons::OfMonth(GameAccess::CityMonth()));
            for (size_t k = 0; k < keys.size(); ++k) {
                const std::optional<CityRenderMeta> meta = ctx_.tileStore->LoadMeta(keys[k]);
                if (!meta || meta->sizeX != city.sizeX || meta->sizeZ != city.sizeZ || meta->levels.empty()) {
                    continue;
                }
                if (!n.hasRender) {
                    n.hasRender = true;
                    n.renderTime = meta->renderTime;
                    n.stale = ctx_.tileStore->IsStale(city, keys[k]);
                }
                for (int r = 0; r < ViewMath::kRotationCount; ++r) {
                    for (int z = 0; z < ViewMath::kZoomCount; ++z) {
                        if (CityMeta::ChooseLevel(meta->levels, r, z)) {
                            n.drawnViews |= 1u << (r * ViewMath::kZoomCount + z);
                        }
                    }
                }
            }
        }
        if (n.hasRender) {
            rects.push_back({city.cellX, city.cellZ, city.cellX + city.sizeX, city.cellZ + city.sizeZ});
            owners.push_back(static_cast<int>(neighbors_.size()));
        }
        neighbors_.push_back(std::move(n));
    }

    // The same order the render module draws in: only front neighbors are drawn over the city.
    const NeighborMath::CellRect currentRect{current->cellX, current->cellZ, current->cellX + current->sizeX,
                                             current->cellZ + current->sizeZ};
    std::vector<int> back, front;
    for (int r = 0; r < 4; ++r) {
        NeighborMath::SortBackToFront(rects, currentRect, NeighborMath::SignsForRotation(r), back, front);
        for (const int index : front) {
            neighbors_[owners[index]].frontMask |= static_cast<uint8_t>(1u << r);
        }
    }
    LOG_DEBUG("Interaction: {} neighbor cities, {} with a render", neighbors_.size(), rects.size());
}

void NeighborPicker::PassCallback_(const DrawServicePass pass, const bool begin, void* userData)
{
    if (pass == DrawServicePass::PreDynamic && begin) {
        Guarded("Interaction: the draw callback", [&] { static_cast<NeighborPicker*>(userData)->Tick_(); });
    }
}

void NeighborPicker::Tick_()
{
    if (!initialized_ || !cityActive_) {
        return;
    }
    if (!Active_()) {
        SetHover_(-1);
        UpdateLayer_();
        return;
    }
    // The store bumps its generation for every index change while it scans, so at most one
    // rebuild per second for it.
    if (ctx_.region->Generation() != regionGeneration_ ||
        (ctx_.tileStore->Generation() != storeGeneration_ && NowMs() - lastRebuildMs_ >= 1000)) {
        Rebuild_();
    }

    // The cursor counts when the city view itself had it last: its input control got a mouse
    // move or button, the same events a click uses, and no ImGui window wants the mouse now.
    // Without those events (none yet in this city), only when the window under the cursor is
    // the city view itself. That test alone missed the city view in the game.
    cursorInView_ = false;
    cISC4View3DWin* view = GameAccess::View3D();
    cIGZWin* viewWin = view ? view->AsIGZWin() : nullptr;
    cIGZWinMgrPtr winMgr;
    const bool imguiMouse = ImGuiWantsMouse(ctx_);
    const cIGZWin* under = nullptr;
    if (viewWin && winMgr) {
        int32_t sx = 0, sz = 0;
        winMgr->GetCursorScreenPosition(sx, sz);
        under = winMgr->GetWindowFromPoint(sx, sz);
        windowUnderCursor_ = under == viewWin ? "the city view" : under ? "a game window" : "none";
    }
    if (viewPointer_ && !imguiMouse) {
        cursorInView_ = true;
        cursorX_ = viewX_;
        cursorZ_ = viewZ_;
    }
    else if (!viewPointer_ && !imguiMouse && under && under == viewWin) {
        int32_t x = 0, z = 0;
        if (winMgr->GetCursorRelativePosition(viewWin, x, z)) {
            cursorInView_ = true;
            cursorX_ = x;
            cursorZ_ = z;
        }
    }

    int hover = -1;
    bool picked = false;
    if (cursorInView_ && ctx_.cameraReader->Update()) {
        const CameraFrame& f = ctx_.cameraReader->Frame();
        // The region thumbnail writer draws at magnification 0.25: no tint in that picture.
        // Game zoom 6 is zoom 5 at magnification 2.
        if (f.magnification >= 1.0f) {
            picked = true;
            const float key[7] = {
                static_cast<float>(cursorX_),    static_cast<float>(cursorZ_),  f.target.fX, f.target.fY, f.target.fZ,
                static_cast<float>(f.zoomIndex), static_cast<float>(f.rotation)};
            if (std::equal(std::begin(key), std::end(key), std::begin(pickKey_))) {
                hover = hovered_;
            }
            else {
                std::copy(std::begin(key), std::end(key), std::begin(pickKey_));
                hover = Pick_(cursorX_, cursorZ_);
            }
        }
    }
    if (!picked) {
        pickKey_[0] = NAN; // pick again when the cursor comes back
    }
    SetHover_(hover);
    UpdateLayer_();
}

int NeighborPicker::Pick_(const int x, const int z)
{
    const CityInfo* current = ctx_.region->Current();
    if (!current || !ctx_.cameraReader->Valid()) {
        return -1;
    }
    // The centre of the pixel under the cursor.
    const cS3DVector2 screen{static_cast<float>(x) + 0.5f, static_cast<float>(z) + 0.5f};
    const CameraReader& reader = *ctx_.cameraReader;
    pickGround_ = InteractionMath::PickGround(
        [&](const float h) {
            cS3DVector3 p;
            reader.ScreenToGround(screen, h, p);
            return p;
        },
        profile_);
    const CityInfo* city =
        InteractionMath::NeighborAt(ctx_.region->Cities(), *current, pickGround_.fX, pickGround_.fZ);
    if (!city) {
        return -1;
    }

    // The current city can be in front of the neighbor's ground: its own terrain near the
    // edge, or a building or tree. Then the click belongs to the city.
    if (cISC4View3DWin* view = GameAccess::View3D()) {
        float hit[3] = {};
        constexpr float kMargin = 1.0f;
        if (view->PickTerrain(x, z, hit, false) && hit[0] > kMargin && hit[0] < profile_.sizeX - kMargin &&
            hit[2] > kMargin && hit[2] < profile_.sizeZ - kMargin) {
            return -1;
        }
        cISC4Occupant* occupant = nullptr;
        if (view->PickOccupant(x, z, occupant) && occupant) {
            occupant->Release();
            return -1;
        }
    }

    for (size_t i = 0; i < neighbors_.size(); ++i) {
        if (neighbors_[i].cellX == city->cellX && neighbors_[i].cellZ == city->cellZ) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void NeighborPicker::SetHover_(const int neighbor)
{
    if (neighbor == hovered_) {
        return;
    }
    hovered_ = neighbor;
    if (neighbor >= 0) {
        const Neighbor& n = neighbors_[neighbor];
        lastHovered_ = n.name.empty() ? "an unnamed city" : n.name;
        LOG_DEBUG("Interaction: pointer over '{}' at cell ({}, {})", n.name, n.cellX, n.cellZ);
    }
}

void NeighborPicker::UpdateLayer_()
{
    if (!ctx_.worldDraw || layer_ == 0) {
        return;
    }
    const bool want = hovered_ >= 0 && ctx_.settings->hoverTint && Active_();
    if (want && meshFor_ != hovered_) {
        const CityInfo* current = ctx_.region->Current();
        const Neighbor& n = neighbors_[hovered_];
        mesh_.clear();
        if (current) {
            const float x0 = static_cast<float>(n.cellX - current->cellX) * kRegionCellMetres;
            const float z0 = static_cast<float>(n.cellZ - current->cellZ) * kRegionCellMetres;
            InteractionMath::Footprint(profile_, x0, z0, x0 + static_cast<float>(n.sizeX) * kRegionCellMetres,
                                       z0 + static_cast<float>(n.sizeZ) * kRegionCellMetres,
                                       [this](const float x, const float y, const float z) {
                                           mesh_.push_back({x, y, z, kTintColor, 0.0f, 0.0f});
                                       });
        }
        ctx_.worldDraw->SetLayerTriangles(layer_, mesh_.data(), static_cast<uint32_t>(mesh_.size()));
        meshFor_ = hovered_;
    }
    if (want) {
        // A neighbor drawn in front of the city covers everything, so its tint must too.
        // Behind the city, the city's terrain and buildings hide the tint where they hide
        // the neighbor.
        // A neighbor that is not drawn at this zoom (no render up to it) covers nothing.
        const int rotation = GameAccess::Rotation();
        const int zoom = GameAccess::Zoom();
        const Neighbor& n = neighbors_[hovered_];
        const bool front = ViewMath::IsValidRotation(rotation) && ViewMath::IsValidZoom(zoom) &&
                           ((n.frontMask >> rotation) & 1) != 0 &&
                           ((n.drawnViews >> (rotation * ViewMath::kZoomCount + zoom)) & 1) != 0;
        if (front != layerFront_) {
            ctx_.worldDraw->SetLayerStyle(layer_, front ? kFrontStyle : kBackStyle);
            layerFront_ = front;
        }
    }
    if (want != layerVisible_) {
        ctx_.worldDraw->SetLayerVisible(layer_, want);
        layerVisible_ = want;
    }
}

bool NeighborPicker::OnMouseDownL(const int x, const int z)
{
    press_ = {};
    viewPointer_ = true;
    viewX_ = x;
    viewZ_ = z;
    if (ClickBlocker_() != nullptr || !ctx_.cameraReader->Update() ||
        ctx_.cameraReader->Frame().magnification < 1.0f) {
        return false;
    }
    const int neighbor = Pick_(x, z);
    if (neighbor < 0) {
        return false;
    }
    press_ = {true, x, z, neighbor, false, pickGround_};
    return true;
}

void NeighborPicker::OnMouseMove(const int x, const int z)
{
    viewPointer_ = true;
    viewX_ = x;
    viewZ_ = z;
    if (!viewMovesLogged_) {
        viewMovesLogged_ = true;
        LOG_INFO("Interaction: the city view sends mouse moves (window under the pointer: {})", windowUnderCursor_);
    }
    if (press_.active && InteractionMath::IsDrag(press_.x, press_.z, x, z)) {
        press_.dragged = true;
    }
}

bool NeighborPicker::OnMouseUpL(const int x, const int z)
{
    if (!press_.active) {
        return false;
    }
    const Press press = press_;
    press_ = {};
    // The down was ours, so the up is too, also when nothing happens.
    if (press.dragged || InteractionMath::IsDrag(press.x, press.z, x, z)) {
        LOG_DEBUG("Interaction: the mouse moved, no switch");
        return true;
    }
    if (ClickBlocker_() != nullptr || press.neighbor < 0 || press.neighbor >= static_cast<int>(neighbors_.size())) {
        return true;
    }
    // A double click switches: a single click is too easy to make by accident.
    const InteractionMath::Click click{press.neighbor, NowMs(), x, z};
    if (InteractionMath::IsDoubleClick(firstClick_, click, GetDoubleClickTime())) {
        firstClick_ = {};
        Activate_(press.neighbor, press.ground);
    }
    else {
        firstClick_ = click;
    }
    return true;
}

void NeighborPicker::OnMouseExit()
{
    press_ = {};
    viewPointer_ = false;
}

void NeighborPicker::Activate_(const int neighbor, const cS3DVector3& ground)
{
    const Neighbor& n = neighbors_[neighbor];
    // The camera of the new city opens where the player clicked.
    if (const CityInfo* current = ctx_.region->Current()) {
        constexpr float kUnit = GameAddresses::Region::kGridUnitMetres;
        arrival_ = {ground.fX + static_cast<float>(current->cellX - n.cellX) * kUnit,
                    ground.fZ + static_cast<float>(current->cellZ - n.cellZ) * kUnit};
    }
    LOG_INFO("Interaction: click on '{}' at cell ({}, {})", n.name, n.cellX, n.cellZ);
    if (!ctx_.switcher->CanSaveCurrentCity()) {
        if (overlayRegistered_) {
            confirmNeighbor_ = neighbor;
            confirmOpenRequested_ = true;
            lastClick_ = std::format("Asked to leave without saving for {}.", n.name);
            LOG_INFO("Interaction: this city cannot be saved, asking first");
        }
        else {
            lastClick_ = "This city cannot be saved and there is no window to confirm. Nothing done.";
            LOG_WARN("Interaction: this city cannot be saved and there is no ImGui to confirm, no switch");
        }
        return;
    }
    if (ctx_.switcher->RequestSwitch(n.cellX, n.cellZ, false, &arrival_)) {
        lastClick_ = std::format("Switching to {}.", n.name);
        LOG_INFO("Interaction: switch to '{}' requested", n.name);
    }
    else {
        lastClick_ = std::format("Could not switch to {}: {}", n.name, ctx_.switcher->LastError());
        LOG_WARN("Interaction: switch to '{}' refused: {}", n.name, ctx_.switcher->LastError());
    }
}

void NeighborPicker::OverlayRender_(void* data)
{
    auto* self = static_cast<NeighborPicker*>(data);
    if (self->initialized_) {
        Guarded("Interaction: the tooltip", [&] { self->DrawOverlay_(); });
    }
}

void NeighborPicker::DrawOverlay_()
{
    if (confirmOpenRequested_) {
        ImGui::OpenPopup(kConfirmPopup);
        confirmOpenRequested_ = false;
    }
    if (ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (confirmNeighbor_ < 0 || confirmNeighbor_ >= static_cast<int>(neighbors_.size()) || !Active_()) {
            confirmNeighbor_ = -1;
            ImGui::CloseCurrentPopup();
        }
        else {
            const Neighbor& n = neighbors_[confirmNeighbor_];
            ImGui::TextUnformatted("This city cannot be saved. Leave it without saving?");
            ImGui::Text("You go to %s.", n.name.empty() ? "an unnamed city" : n.name.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Leave without saving")) {
                if (ctx_.switcher->RequestSwitch(n.cellX, n.cellZ, true, &arrival_)) {
                    lastClick_ = std::format("Switching to {} without saving.", n.name);
                    LOG_INFO("Interaction: switch to '{}' without saving requested", n.name);
                }
                else {
                    lastClick_ = std::format("Could not switch to {}: {}", n.name, ctx_.switcher->LastError());
                    LOG_WARN("Interaction: switch to '{}' refused: {}", n.name, ctx_.switcher->LastError());
                }
                confirmNeighbor_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Stay here")) {
                lastClick_ = "You stayed in this city.";
                LOG_INFO("Interaction: switch without saving cancelled");
                confirmNeighbor_ = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
        return;
    }
    confirmNeighbor_ = -1;

    if (hovered_ >= 0 && hovered_ < static_cast<int>(neighbors_.size()) && cursorInView_ && Active_()) {
        DrawTooltip_(neighbors_[hovered_]);
    }
}

void NeighborPicker::DrawTooltip_(const Neighbor& n)
{
    ImGui::BeginTooltip();
    // As the region view shows a city: name, mayor and difficulty stars, then the mayor rating,
    // the treasury and the residents, commercial jobs and industrial jobs in green, blue and yellow.
    ImGui::TextUnformatted(n.name.empty() ? "Unnamed city" : n.name.c_str());
    if (n.founded) {
        if (!n.mayor.empty()) {
            ImGui::TextUnformatted(n.mayor.c_str());
            ImGui::SameLine();
        }
        static constexpr const char* kStars[] = {"*", "**", "***"};
        ImGui::TextDisabled("%s", kStars[std::clamp(n.difficulty, 0, 2)]);
        ImGui::TextDisabled("%s", n.sizeLabel);
        ImGui::Separator();

        ImGui::TextUnformatted("Mayor rating");
        const float rating = static_cast<float>(std::clamp(n.mayorRating, -100, 100));
        const float t = (rating + 100.0f) / 200.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f - t, 0.35f + 0.5f * t, 0.25f, 1.0f));
        ImGui::ProgressBar(t, ImVec2(ImGui::GetFontSize() * 10.0f, ImGui::GetFontSize() * 0.6f), "");
        ImGui::PopStyleColor();

        char number[32];
        InteractionMath::GroupedNumber(static_cast<int>(std::lround(std::clamp(n.budget, -2e9, 2e9))), number,
                                       sizeof(number));
        ImGui::Text("\xC2\xA7%s", number);  // the simoleon sign
        InteractionMath::GroupedNumber(n.population, number, sizeof(number));
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.4f, 1.0f), "R  %s", number);
        InteractionMath::GroupedNumber(n.commercialJobs, number, sizeof(number));
        ImGui::TextColored(ImVec4(0.45f, 0.65f, 1.0f, 1.0f), "C  %s", number);
        InteractionMath::GroupedNumber(n.industrialJobs, number, sizeof(number));
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "I  %s", number);
        ImGui::Separator();
    }
    else {
        ImGui::TextDisabled("%s", n.sizeLabel);
    }
    const char* blocker = ClickBlocker_();
    if (!n.founded) {
        ImGui::TextUnformatted("Not founded");
        if (!blocker) {
            ImGui::TextUnformatted("Double-click: opens the region view to found this city");
        }
    }
    else {
        if (n.hasRender) {
            char age[64];
            const int64_t seconds = (TileStore::NowFileTime() - n.renderTime) / 10'000'000;
            InteractionMath::RenderAgeText(std::max<int64_t>(seconds, 0), age, sizeof(age));
            ImGui::TextUnformatted(age);
            if (n.stale) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Out of date. The city changed after the render.");
            }
            const int rotation = GameAccess::Rotation();
            const int zoom = GameAccess::Zoom();
            if (ViewMath::IsValidRotation(rotation) && ViewMath::IsValidZoom(zoom) &&
                ((n.drawnViews >> (rotation * ViewMath::kZoomCount + zoom)) & 1) == 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "Not shown: no render in rotation %d at this zoom. Render it in this rotation.",
                                   rotation + 1);
            }
        }
        else {
            ImGui::TextUnformatted("No render yet");
        }
        if (!blocker) {
            ImGui::TextUnformatted("Double-click to play this city");
        }
    }
    if (blocker && blocker[0] != '\0') {
        ImGui::TextDisabled("%s", blocker);
    }
    ImGui::EndTooltip();
}

void NeighborPicker::DrawPanelSection()
{
    Settings* settings = ctx_.settings;
    bool changed = ImGui::Checkbox("Tint the neighbor under the pointer", &settings->hoverTint);
    changed |= ImGui::Checkbox("Click a neighbor to play it", &settings->clickToSwitch);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Double-click a neighbor city to save this city and open the neighbor.");
    }
    if (changed) {
        settings->MarkChanged();
    }
    if (!settings->showNeighbors) {
        ImGui::TextDisabled("Turn on the neighbor cities to use this.");
    }
    else if (ctx_.capturer->IsRunning()) {
        ImGui::TextDisabled("Off while a render runs.");
    }
    else if (ctx_.batch && ctx_.batch->IsRunning()) {
        ImGui::TextDisabled("Off while the region render runs.");
    }
    else if (ctx_.switcher->IsBusy()) {
        ImGui::TextDisabled("Off while the city changes.");
    }
    ImGui::Text("Last neighbor under the pointer: %s", lastHovered_.empty() ? "none" : lastHovered_.c_str());
    ImGui::Text("Last click: %s", lastClick_.empty() ? "none" : lastClick_.c_str());
    if (ImGui::TreeNode("Interaction diagnostics")) {
        ImGui::Text("Click control in the city view: %s", controlAdded_ ? "yes" : "no");
        ImGui::Text("Window under the pointer: %s", windowUnderCursor_);
        ImGui::Text("Pointer in the city view: %s (%d, %d)", cursorInView_ ? "yes" : "no", cursorX_, cursorZ_);
        ImGui::Text("Mouse moves from the city view: %s", viewPointer_ ? "yes" : "no");
        ImGui::Text("Mean edge height: %.1f m", profile_.mean);
        ImGui::Text("Tint triangles: %d", static_cast<int>(mesh_.size() / 3));
        const char* blocker = ClickBlocker_();
        ImGui::Text("Click now: %s", !blocker ? "yes" : blocker[0] ? blocker : "no");
        ImGui::TreePop();
    }
}
