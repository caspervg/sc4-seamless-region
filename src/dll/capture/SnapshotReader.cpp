#include "capture/SnapshotReader.hpp"

#include <memory>

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

// SCD3D11's async snapshot extension (IID 0xA5C1D3E7, scd3d11 ext/cIGZGAsyncSnapshotExtension.h).
// A copy is queued behind the draws already made and read later, so the CPU does not wait for
// the GPU at the call. Pixels: packed RGBA8 rows, alpha 255, top row first. A driver with it
// also declares cIGZBuffer::GetColorType as the game implements it, so a `reuse` buffer is safe.
class cIGZGAsyncSnapshotExtension : public cIGZUnknown
{
public:
    virtual uint32_t GetAsyncSnapshotVersion() = 0;
    virtual uint32_t BeginCopy(int32_t x, int32_t y, int32_t width, int32_t height) = 0;
    virtual bool EndCopy(uint32_t ticket, uint8_t* pixels, uint32_t capacity, uint32_t timeoutMs) = 0;
    virtual void CancelCopy(uint32_t ticket) = 0;
};

namespace
{
    constexpr uint32_t kAsyncSnapshotIid = 0xA5C1D3E7;
    constexpr uint32_t kAsyncTimeoutMs = 2000;
}

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

void SnapshotReader::OnCityShutdown() {}

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
    ImGui::Text("Reads: %u (without waiting: %u, failed: %u)", reads_, asyncReads_, failures_);
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

    // A new buffer, never `reuse`, unless the driver has the async extension. SCD3D11 before it
    // checks a passed buffer with GetColorType declared as returning an enum, but the game's slot
    // returns the cGZBufferColorType struct through a hidden pointer (0x008268f0, slots 14 and 15
    // of 0x00ac1400). The game then writes 8 bytes through a stack value: an access violation or
    // quiet memory damage. See docs/re-notes/capture.md, Replacement drivers.
    const bool reuse = async_ && reuse_ && static_cast<int>(reuse_->Width()) == w &&
                       static_cast<int>(reuse_->Height()) == h;
    cIGZBuffer* const result = extension_->CopyColorBuffer(x, y, w, h, reuse ? reuse_ : nullptr);
    if (!result) {
        return Fail_("The graphics driver gave no picture.");
    }
    // A reused buffer comes back without a new reference. A new one is ours: kept for the next
    // read of the same size when the driver allows it, else released after this read.
    if (!reuse && async_) {
        if (reuse_) {
            reuse_->Release();
        }
        reuse_ = result;
    }
    const std::unique_ptr<cIGZBuffer, void (*)(cIGZBuffer*)> owned(
        async_ ? nullptr : result, [](cIGZBuffer* b) { b->Release(); });

    if (static_cast<int>(result->Width()) != w || static_cast<int>(result->Height()) != h) {
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
    // scvk never returns null: a failed read (fence timeout, lost device) is a black picture of
    // the asked size. A real read of the game is never pure black: the clear color is a key
    // color and terrain is never exactly 0, 0, 0.
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (CaptureMath::CountKeyPixels(rgba.data(), pixels, CaptureMath::Rgb{0, 0, 0}) == pixels) {
        return Fail_("The graphics driver gave a black picture (a failed read).");
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
        void* async = nullptr;
        if (driver->QueryInterface(kAsyncSnapshotIid, &async) && async) {
            async_ = static_cast<cIGZGAsyncSnapshotExtension*>(async);
            LOG_INFO("Snapshot: the graphics driver reads the screen back without waiting (version {})",
                     async_->GetAsyncSnapshotVersion());
        }
        return true;
    }
    unavailableLogged_ = true;
    LOG_WARN("Snapshot: the graphics driver has no snapshot extension (0x{:08X}). City renders are not possible.",
             GameAddresses::Renderer::kSnapshotExtensionIID);
    return false;
}

bool SnapshotReader::AsyncAvailable() { return AcquireExtension_() && async_ != nullptr; }

uint32_t SnapshotReader::BeginRead(const int x, const int y, const int w, const int h)
{
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || !AsyncAvailable()) {
        return 0;
    }
    return async_->BeginCopy(x, y, w, h);
}

bool SnapshotReader::EndRead(const uint32_t ticket, const int w, const int h, std::vector<uint8_t>& rgba)
{
    if (!ticket || !async_) {
        return Fail_("The graphics driver has no read on the way.");
    }
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    rgba.resize(bytes);
    if (!async_->EndCopy(ticket, rgba.data(), static_cast<uint32_t>(bytes), kAsyncTimeoutMs)) {
        return Fail_("The graphics driver did not finish a read in time.");
    }
    ++reads_;
    ++asyncReads_;
    lastW_ = w;
    lastH_ = h;
    lastStride_ = static_cast<uint32_t>(w) * 4;
    lastError_.clear();
    return true;
}

void SnapshotReader::CancelRead(const uint32_t ticket)
{
    if (ticket && async_) {
        async_->CancelCopy(ticket);
    }
}

void SnapshotReader::ReleaseAll_()
{
    if (reuse_) {
        reuse_->Release();
        reuse_ = nullptr;
    }
    if (async_) {
        async_->Release();
        async_ = nullptr;
    }
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
