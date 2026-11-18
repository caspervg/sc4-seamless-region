#include "capture/ModelLoadMonitor.hpp"

#include "capture/CaptureAddresses.hpp"
#include "cIGZUnknown.h"
#include "cISC4View3DWin.h"
#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    namespace MM = CaptureAddresses::ModelMaker;

    uint8_t Flag(const void* modelMaker, const uint32_t offset)
    {
        return GameAccess::ReadField<uint8_t>(modelMaker, offset);
    }
}

bool ModelLoadMonitor::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    return true;
}

void ModelLoadMonitor::Shutdown() { SetSynchronous(false); }

void ModelLoadMonitor::OnCityInit()
{
    forced_ = false;
    mismatchLogged_ = false;
    if (Known()) {
        const void* mm = ModelMaker_();
        LOG_INFO("Model loading: model maker found, background loading {}",
                 Flag(mm, MM::kBackgroundLoadingEnabled) ? "on" : "off");
    }
}

void ModelLoadMonitor::OnCityShutdown()
{
    // The model maker goes away with the city view. Put the flag back while it still exists.
    SetSynchronous(false);
}

void ModelLoadMonitor::DrawPanelSection()
{
    const void* mm = ModelMaker_();
    if (!mm) {
        ImGui::TextDisabled("%s", GameAccess::View3D() ? "The model loader of the game was not found."
                                                       : "Open a city to see the model loader.");
        return;
    }
    const bool background = Flag(mm, MM::kBackgroundLoadingEnabled) != 0;
    ImGui::Text("Models: %s", IsIdle() ? "all loaded for this view" : "loading");
    ImGui::Text("Background loading: %s", background ? "on" : "off");
    ImGui::Text("Load at once: %s%s", IsSynchronous() ? "yes" : "no", forced_ ? " (set by the render)" : "");
    if (Flag(mm, MM::kViewChangesSuspended)) {
        ImGui::TextDisabled("The game holds model loading while the view scrolls.");
    }
}

bool ModelLoadMonitor::Known() const { return ModelMaker_() != nullptr; }

bool ModelLoadMonitor::IsIdle() const
{
    const void* mm = ModelMaker_();
    if (!mm) {
        return false;
    }
    if (Flag(mm, MM::kViewChangesSuspended)) {
        return false;
    }
    // Without background loading every load is done before the view change returns.
    return !Flag(mm, MM::kBackgroundLoadingEnabled) || !Flag(mm, MM::kLoadingInBackground);
}

int ModelLoadMonitor::PendingCount() const
{
    const void* mm = ModelMaker_();
    if (!mm) {
        return -1;
    }
    return IsIdle() ? 0 : 1;
}

bool ModelLoadMonitor::SetSynchronous(const bool on)
{
    void* mm = ModelMaker_();
    if (!mm) {
        forced_ = false;
        return false;
    }
    uint8_t* flag = GameAccess::FieldPtr<uint8_t>(mm, MM::kSynchronousLoading);
    if (on) {
        if (!forced_) {
            valueBeforeForce_ = *flag;
            forced_ = true;
        }
        *flag = 1;
    }
    else if (forced_) {
        *flag = valueBeforeForce_;
        forced_ = false;
    }
    return true;
}

bool ModelLoadMonitor::IsSynchronous() const
{
    const void* mm = ModelMaker_();
    return mm && (Flag(mm, MM::kSynchronousLoading) || !Flag(mm, MM::kBackgroundLoadingEnabled));
}

void* ModelLoadMonitor::ModelMaker_() const
{
    cISC4View3DWin* view = GameAccess::View3D();
    if (!view) {
        return nullptr;
    }
    // cSC4View3DWin::GetModelMaker (0x007f0fe0) returns the cSC4ModelMaker object itself.
    cIGZUnknown* modelMaker = view->GetModelMaker();
    if (!modelMaker) {
        return nullptr;
    }
    if (GameAccess::ReadField<uintptr_t>(modelMaker, 0) != MM::kVtable) {
        if (!mismatchLogged_) {
            mismatchLogged_ = true;
            LOG_WARN("Model loading: the model maker is not the expected object (vtable 0x{:08X}). "
                     "The render waits a fixed time for models instead.",
                     GameAccess::ReadField<uintptr_t>(modelMaker, 0));
        }
        return nullptr;
    }
    return modelMaker;
}
