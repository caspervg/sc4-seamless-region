#include "capture/SnapshotReader.hpp"

#include "capture/CaptureAddresses.hpp"
#include "capture/CaptureMath.hpp"
#include "cIGZBuffer.h"
#include "cIGZGDriver.h"
#include "core/GameAddresses.hpp"
#include "imgui.h"
#include "utils/Logger.h"

// The snapshot extension of the driver (IID 0xE69BFE2A). One method after cIGZUnknown.
class cIGZGSnapshotExtension : public cIGZUnknown
{
public:
    // Copies a rectangle of the back buffer into a 32 bit A8R8G8B8 buffer, top row first.
    // reuse = nullptr makes a new buffer that the caller owns. A ready `reuse` is filled and
    // returned without a new reference.
    virtual cIGZBuffer* CopyColorBuffer(int32_t x, int32_t y, int32_t w, int32_t h, cIGZBuffer* reuse) = 0;
};

bool SnapshotReader::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (Available()) {
        LOG_INFO("Snapshot: the graphics driver can read the screen back");
    }
    // Stays active without the extension: Available() and the panel tell the user.
    return true;
}

void SnapshotReader::Shutdown() { ReleaseAll_(); }

void SnapshotReader::OnCityInit() {}

void SnapshotReader::OnCityShutdown()
{
    // Frees the megabyte of the reused buffer between cities.
    ReleaseBuffer_();
}

void SnapshotReader::DrawPanelSection()
{
    if (Available()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "The screen can be read back.");
    }
    else {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "The screen cannot be read back.");
        ImGui::TextWrapped("This graphics driver does not give its picture to the plugin. Use the DirectX "
                           "driver of the game, or SCD3D11.");
    }
    ImGui::Text("Reads: %u (failed: %u)", reads_, failures_);
    if (reads_ > 0) {
        ImGui::Text("Last read: %d x %d pixels, row length %u bytes", lastW_, lastH_, lastStride_);
    }
    if (!lastError_.empty()) {
        ImGui::TextWrapped("Last problem: %s", lastError_.c_str());
    }
}

bool SnapshotReader::Available() { return AcquireExtension_(); }

bool SnapshotReader::Read(const int x, const int y, const int w, const int h, std::vector<uint8_t>& rgba)
{
    if (w <= 0 || h <= 0 || x < 0 || y < 0) {
        return Fail_("The rectangle to read is not valid.");
    }
    if (!AcquireExtension_()) {
        return Fail_("The graphics driver cannot read the screen back.");
    }

    // Only reuse a buffer of exactly this size. With a reused buffer the DX7 driver grows the
    // rectangle to the buffer size and the OpenGL driver shrinks it.
    if (buffer_ && (bufferW_ != w || bufferH_ != h)) {
        ReleaseBuffer_();
    }

    cIGZBuffer* const passed = buffer_;
    cIGZBuffer* const result = extension_->CopyColorBuffer(x, y, w, h, passed);
    if (!result) {
        // The software and OpenGL drivers release `reuse` on some of their failure paths and
        // the DX7 driver never does. Dropping the pointer without a Release can leak one
        // buffer. A Release could free it twice.
        buffer_ = nullptr;
        return Fail_("The graphics driver gave no picture.");
    }
    if (passed && result != passed) {
        passed->Release();
    }
    buffer_ = result;
    bufferW_ = result->Width();
    bufferH_ = result->Height();

    if (bufferW_ != w || bufferH_ != h) {
        // The driver clamped the rectangle to the back buffer. The picture then starts at the
        // clamped corner, so it cannot be used.
        return Fail_("The rectangle to read is larger than the game window.");
    }
    if (result->GetBitsPerPixel() != 32) {
        return Fail_("The graphics driver gave a picture that is not 32 bit.");
    }
    if (!result->Lock(CaptureAddresses::Snapshot::kBufferLockFlags)) {
        return Fail_("The picture of the graphics driver cannot be locked.");
    }
    const auto* bits = static_cast<const uint8_t*>(result->GetColorSurfaceBits());
    const uint32_t stride = result->GetColorSurfaceStride();
    const bool usable = bits && stride >= static_cast<uint32_t>(w) * 4;
    if (usable) {
        rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        CaptureMath::ConvertBgraToRgba(bits, stride, w, h, rgba.data());
    }
    result->Unlock(CaptureAddresses::Snapshot::kBufferLockFlags);
    if (!usable) {
        return Fail_("The picture of the graphics driver has no pixels.");
    }

    ++reads_;
    lastW_ = w;
    lastH_ = h;
    lastStride_ = stride;
    lastError_.clear();
    return true;
}

bool SnapshotReader::AcquireExtension_()
{
    cIGZGDriver* const driver = GameAccess::GDriver();
    if (extension_ && driver == driver_) {
        return true;
    }
    if (driver && driver == driver_ && unavailableLogged_) {
        return false;  // this driver was asked before
    }
    // A new driver (or none): the old extension and its buffer belong to the old one.
    ReleaseAll_();
    if (!driver) {
        return false;
    }
    driver_ = driver;
    void* extension = nullptr;
    if (driver->QueryInterface(GameAddresses::Renderer::kSnapshotExtensionIID, &extension) && extension) {
        extension_ = static_cast<cIGZGSnapshotExtension*>(extension);
        return true;
    }
    unavailableLogged_ = true;
    LOG_WARN("Snapshot: the graphics driver has no snapshot extension (0x{:08X}). City renders are not possible.",
             GameAddresses::Renderer::kSnapshotExtensionIID);
    return false;
}

void SnapshotReader::ReleaseBuffer_()
{
    if (buffer_) {
        buffer_->Release();
        buffer_ = nullptr;
    }
    bufferW_ = 0;
    bufferH_ = 0;
}

void SnapshotReader::ReleaseAll_()
{
    ReleaseBuffer_();
    if (extension_) {
        extension_->Release();
        extension_ = nullptr;
    }
    driver_ = nullptr;
    unavailableLogged_ = false;
}

bool SnapshotReader::Fail_(const char* why)
{
    ++failures_;
    if (lastError_ != why) {
        lastError_ = why;
        LOG_WARN("Snapshot: {}", why);
    }
    return false;
}
