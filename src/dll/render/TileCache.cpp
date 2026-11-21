#include "render/TileCache.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "core/Guard.hpp"
#include "imgui.h"
#include "render/NeighborRenderer.hpp"
#include "render/TileLru.hpp"
#include "store/WebpCodec.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"

namespace
{
    constexpr size_t kMaxRequests = 256;  // more than one 4K screen of tiles
    constexpr int kDecodeBuffers = 8;     // decoded tiles waiting for upload, 1 MB each
    constexpr int kMaxTileEdge = 2048;
    constexpr size_t kDefaultTileBytes = 512 * 512 * 4;
    constexpr int kMaxUploadAttempts = 3;

    // Worker thread. rgba keeps its storage between tiles, so decoding allocates nothing in
    // steady state.
    bool DecodeTile(const std::span<const uint8_t> data, std::vector<uint8_t>& rgba, int& w, int& h)
    {
        if (rgba.size() < kDefaultTileBytes) {
            rgba.resize(kDefaultTileBytes);
        }
        if (WebpCodec::DecodeInto(data, rgba.data(), static_cast<int>(rgba.size()), w, h)) {
            return true;
        }
        // DecodeInto sets w and h when the buffer is too small. Grow it once and try again.
        if (w <= 0 || h <= 0 || w > kMaxTileEdge || h > kMaxTileEdge ||
            static_cast<size_t>(w) * h * 4 <= rgba.size()) {
            return false;
        }
        rgba.resize(static_cast<size_t>(w) * h * 4);
        return WebpCodec::DecodeInto(data, rgba.data(), static_cast<int>(rgba.size()), w, h);
    }

    // Bilinear filtering (zoom 5) needs premultiplied colours, or the colours that the
    // encoder left under transparent pixels bleed into the edges.
    void PremultiplyAlpha(uint8_t* rgba, const size_t pixels)
    {
        for (uint8_t* p = rgba; p != rgba + pixels * 4; p += 4) {
            const uint32_t a = p[3];
            if (a == 255) {
                continue;
            }
            p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
            p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
            p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
        }
    }

    bool IsPowerOfTwo(const int v) { return v > 0 && (v & (v - 1)) == 0; }

    // The texture and the copy cIGZImGuiService keeps for device loss.
    size_t TextureBytes(const int w, const int h) { return static_cast<size_t>(w) * h * 4 * 2; }

    TileCache* sInstance = nullptr;
}

struct TileCache::Impl
{
    struct Request
    {
        TileKey key;
        uint32_t epoch = 0;
    };
    struct Done
    {
        TileKey key;
        int buffer = -1;  // index into buffers, -1 when the tile could not be read or decoded
        int w = 0, h = 0;
        int attempts = 0;
    };

    // Game thread only.
    bool initialized = false;
    bool cityActive = false;
    bool pumpQueued = false;
    TileLru<TileTexture> lru;
    std::unordered_map<TileKey, uint64_t, TileKeyHash, TileKeyEqual> pending;  // key -> pass that asked
    std::unordered_set<TileKey, TileKeyHash, TileKeyEqual> missing;
    std::function<void(const TileKey&)> onReady;
    uint64_t passStamp = 1;
    uint32_t deviceGeneration = 0;
    uint32_t settingsRevision = 0;
    uint64_t hits = 0, misses = 0, uploads = 0, evictions = 0, failures = 0, overBudget = 0, queueDrops = 0;
    bool loggedOverBudget = false, loggedNotPowerOfTwo = false, loggedCreateFailed = false, loggedDecodeFailed = false;

    // Shared with the worker, guarded by mutex.
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    uint32_t epoch = 0;
    std::vector<Request> requests;  // newest at the back, the worker takes the newest first
    std::vector<Done> done;
    std::vector<int> freeBuffers;

    // A buffer belongs to the worker while it decodes and to the game thread while its Done
    // item waits for upload.
    std::array<std::vector<uint8_t>, kDecodeBuffers> buffers;
    std::vector<uint8_t> fileBytes;  // worker only
    std::thread worker;

    void ReleaseBuffer(const int buffer)
    {
        if (buffer < 0) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            freeBuffers.push_back(buffer);
        }
        wake.notify_one();
    }
};

TileCache::TileCache() : impl_(std::make_unique<Impl>()) {}
TileCache::~TileCache() { StopWorker_(); }

bool TileCache::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.imgui || !ctx_.settings || !ctx_.tileStore) {
        LOG_WARN("Tile cache: not active, it needs the ImGui service of SC4RenderServices for its textures");
        return false;
    }
    Impl& s = *impl_;
    s.requests.reserve(kMaxRequests + 1);
    s.done.reserve(kMaxRequests);
    s.freeBuffers.reserve(kDecodeBuffers);
    for (int i = 0; i < kDecodeBuffers; ++i) {
        s.freeBuffers.push_back(i);
    }
    s.deviceGeneration = ctx_.imgui->GetDeviceGeneration();
    s.settingsRevision = ctx_.settings->Revision();
    s.lru.SetBudget(static_cast<size_t>(ctx_.settings->tileCacheMB) * 1024 * 1024);
    s.initialized = true;
    sInstance = this;
    LOG_INFO("Tile cache: ready, limit {} MB", ctx_.settings->tileCacheMB);
    return true;
}

void TileCache::Shutdown()
{
    StopWorker_();
    if (impl_->initialized) {
        Clear();
    }
    impl_->initialized = false;
    impl_->onReady = nullptr;
    if (sInstance == this) {
        sInstance = nullptr;
    }
}

void TileCache::OnCityInit()
{
    impl_->cityActive = true;
    StartWorker_();
}

void TileCache::OnCityShutdown()
{
    impl_->cityActive = false;
    StopWorker_();
    Clear();
}

void TileCache::StartWorker_()
{
    Impl& s = *impl_;
    if (!s.initialized || s.worker.joinable()) {
        return;
    }
    s.stop = false;
    s.worker = std::thread([this] { WorkerMain_(); });
}

void TileCache::StopWorker_()
{
    Impl& s = *impl_;
    if (!s.worker.joinable()) {
        return;
    }
    {
        std::lock_guard lock(s.mutex);
        s.stop = true;
    }
    s.wake.notify_all();
    s.worker.join();
}

void TileCache::WorkerMain_()
{
    Impl& s = *impl_;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::unique_lock lock(s.mutex);
    for (;;) {
        // Back-pressure: no decode without a free buffer.
        s.wake.wait(lock, [&] { return s.stop || (!s.requests.empty() && !s.freeBuffers.empty()); });
        if (s.stop) {
            return;
        }
        Impl::Request request = std::move(s.requests.back());
        s.requests.pop_back();
        int buffer = s.freeBuffers.back();
        s.freeBuffers.pop_back();
        lock.unlock();

        int w = 0, h = 0;
        bool ok = false;
        try {
            std::vector<uint8_t>& rgba = s.buffers[buffer];
            ok = ctx_.tileStore->ReadTile(request.key, s.fileBytes) &&
                 DecodeTile(std::span<const uint8_t>(s.fileBytes), rgba, w, h) && w > 0 && h > 0 &&
                 w <= kMaxTileEdge && h <= kMaxTileEdge && rgba.size() >= static_cast<size_t>(w) * h * 4;
            if (ok) {
                PremultiplyAlpha(rgba.data(), static_cast<size_t>(w) * h);
            }
        }
        catch (const std::exception&) {
            ok = false;
        }

        lock.lock();
        if (!ok || request.epoch != s.epoch) {
            s.freeBuffers.push_back(buffer);
            buffer = -1;
        }
        if (request.epoch == s.epoch) {
            s.done.push_back({std::move(request.key), buffer, w, h, 0});
        }
    }
}

const TileTexture* TileCache::Acquire(const TileKey& key)
{
    Impl& s = *impl_;
    if (!s.initialized || !s.cityActive) {
        return nullptr;
    }
    if (const TileTexture* texture = s.lru.Find(key, s.passStamp)) {
        ++s.hits;
        return texture;
    }
    if (s.missing.contains(key)) {
        return nullptr;
    }
    ++s.misses;
    if (s.pending.contains(key)) {
        return nullptr;
    }

    Impl::Request request{key, 0};
    std::optional<TileKey> dropped;
    {
        std::lock_guard lock(s.mutex);
        request.epoch = s.epoch;
        if (s.requests.size() >= kMaxRequests) {
            // The oldest request is the one most likely off screen by now. Forget it.
            dropped = std::move(s.requests.front().key);
            s.requests.erase(s.requests.begin());
            ++s.queueDrops;
        }
        s.requests.push_back(std::move(request));
    }
    s.wake.notify_one();
    if (dropped) {
        s.pending.erase(*dropped);
        // The backing store keeps its spot empty until that area is drawn again. The renderer
        // redraws it when it is on screen, and that draw asks again.
        if (s.onReady) {
            s.onReady(*dropped);
        }
    }
    s.pending.emplace(key, s.passStamp);
    PumpUploads();
    return nullptr;
}

const TileTexture* TileCache::Peek(const TileKey& key)
{
    Impl& s = *impl_;
    if (!s.initialized || !s.cityActive) {
        return nullptr;
    }
    return s.lru.Find(key, s.passStamp);
}

void TileCache::DropQueued()
{
    Impl& s = *impl_;
    if (!s.initialized) {
        return;
    }
    {
        std::lock_guard lock(s.mutex);
        ++s.epoch;  // the tile being decoded right now is dropped when it finishes
        s.requests.clear();
        for (const Impl::Done& d : s.done) {
            if (d.buffer >= 0) {
                s.freeBuffers.push_back(d.buffer);
            }
        }
        s.done.clear();
    }
    s.wake.notify_one();
    s.pending.clear();
}

void TileCache::BeginDrawPass()
{
    if (impl_->initialized) {
        CheckDeviceAndSettings_();
        ++impl_->passStamp;
    }
}

uint32_t TileCache::DriverTexture(const TileTexture& texture) const
{
    return impl_->initialized
               ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx_.imgui->GetTextureID(texture.texture)))
               : 0;
}

size_t TileCache::PendingCount() const { return impl_->pending.size(); }

void TileCache::PumpUploads()
{
    Impl& s = *impl_;
    if (!s.initialized || s.pumpQueued || s.pending.empty()) {
        return;
    }
    // cIGZImGuiService creates driver textures only inside its frame, so the work runs there.
    s.pumpQueued = ctx_.imgui->QueueRender(&TileCache::PumpThunk_, nullptr);
}

void TileCache::PumpThunk_(void*)
{
    if (sInstance) {
        Guarded("Tile cache: the texture upload", [] { sInstance->PumpInFrame_(); });
    }
}

void TileCache::PumpInFrame_()
{
    Impl& s = *impl_;
    s.pumpQueued = false;
    if (!s.initialized) {
        return;
    }
    CheckDeviceAndSettings_();

    const int maxTiles = std::max(1, ctx_.settings->uploadTilesPerFrame);
    const auto maxTime = std::chrono::milliseconds(std::max(1, ctx_.settings->uploadMsPerFrame));
    const auto start = std::chrono::steady_clock::now();
    auto release = [&](TileTexture& texture) {
        ctx_.imgui->ReleaseTexture(texture.texture);
        ++s.evictions;
    };

    for (int uploaded = 0; uploaded < maxTiles && std::chrono::steady_clock::now() - start < maxTime;) {
        Impl::Done d;
        {
            std::lock_guard lock(s.mutex);
            if (s.done.empty()) {
                break;
            }
            d = std::move(s.done.front());
            s.done.erase(s.done.begin());
        }

        const auto pendingIt = s.pending.find(d.key);
        if (pendingIt == s.pending.end()) {
            s.ReleaseBuffer(d.buffer);  // dropped from the queue in the meantime
            continue;
        }
        const uint64_t requestStamp = pendingIt->second;

        if (d.buffer < 0) {
            s.pending.erase(pendingIt);
            s.missing.insert(d.key);
            ++s.failures;
            if (!s.loggedDecodeFailed) {
                s.loggedDecodeFailed = true;
                LOG_WARN("Tile cache: could not read or decode tile {} r{}z{} {}_{}. It is skipped until the "
                         "renders are reloaded. Later failures are not logged.",
                         d.key.cityKey, d.key.rotation, d.key.zoom, d.key.tx, d.key.ty);
            }
            continue;
        }

        if (!IsPowerOfTwo(d.w) || !IsPowerOfTwo(d.h)) {
            // The built-in DirectX driver pads other sizes and the draw context does not
            // know the padding.
            s.pending.erase(pendingIt);
            s.missing.insert(d.key);
            ++s.failures;
            s.ReleaseBuffer(d.buffer);
            if (!s.loggedNotPowerOfTwo) {
                s.loggedNotPowerOfTwo = true;
                LOG_WARN("Tile cache: tile size {}x{} is not a power of two. Such tiles are not shown.", d.w, d.h);
            }
            continue;
        }

        if (!s.lru.MakeRoom(TextureBytes(d.w, d.h), requestStamp, release)) {
            // Everything drawn since the tile was asked for fills the cache. Drop the tile.
            s.pending.erase(pendingIt);
            ++s.overBudget;
            s.ReleaseBuffer(d.buffer);
            if (requestStamp < s.passStamp && s.onReady) {
                // Asked for in an older pass (a fast scroll): the renderer redraws its spot and
                // that draw asks again, protecting less. Asked for in the latest pass: the view
                // does not fit the cache, asking again would loop.
                // ponytail: can still repeat while other passes keep coming and the view is over
                // budget; the warning below tells the player to raise TileCacheMB.
                s.onReady(d.key);
            }
            if (!s.loggedOverBudget) {
                s.loggedOverBudget = true;
                LOG_WARN("Tile cache: the limit of {} MB is too small for this view. Some neighbor tiles are "
                         "not shown. Raise TileCacheMB.",
                         s.lru.Budget() / (1024 * 1024));
            }
            continue;
        }

        ImGuiTextureDesc desc{};
        desc.width = static_cast<uint32_t>(d.w);
        desc.height = static_cast<uint32_t>(d.h);
        desc.pixels = s.buffers[d.buffer].data();
        TileTexture texture{ctx_.imgui->CreateTexture(desc), d.w, d.h};
        // This call makes the driver texture: the real upload.
        const bool created = texture.texture.id != 0 && ctx_.imgui->GetTextureID(texture.texture) != nullptr;
        if (!created) {
            if (texture.texture.id != 0) {
                ctx_.imgui->ReleaseTexture(texture.texture);
            }
            if (++d.attempts < kMaxUploadAttempts) {
                // Device lost or not ready. Keep the pixels and try on a later frame.
                std::lock_guard lock(s.mutex);
                s.done.insert(s.done.begin(), std::move(d));
                break;
            }
            s.pending.erase(pendingIt);
            s.missing.insert(d.key);
            ++s.failures;
            s.ReleaseBuffer(d.buffer);
            if (!s.loggedCreateFailed) {
                s.loggedCreateFailed = true;
                LOG_WARN("Tile cache: the graphics driver did not create a {}x{} texture. The tile is skipped.",
                         d.w, d.h);
            }
            continue;
        }

        s.lru.Insert(d.key, texture, TextureBytes(d.w, d.h), requestStamp);
        s.pending.erase(pendingIt);
        s.ReleaseBuffer(d.buffer);
        ++s.uploads;
        ++uploaded;
        if (s.onReady) {
            s.onReady(d.key);
        }
    }

    PumpUploads();  // again next frame while tiles are on their way
}

void TileCache::CheckDeviceAndSettings_()
{
    Impl& s = *impl_;
    const uint32_t generation = ctx_.imgui->GetDeviceGeneration();
    if (generation != s.deviceGeneration) {
        s.deviceGeneration = generation;
        LOG_INFO("Tile cache: the graphics device was reset, dropping {} tile textures", s.lru.Count());
        Clear();
    }
    if (ctx_.settings->Revision() != s.settingsRevision) {
        s.settingsRevision = ctx_.settings->Revision();
        SetBudget(static_cast<size_t>(ctx_.settings->tileCacheMB) * 1024 * 1024);
    }
}

void TileCache::SetBudget(const size_t bytes)
{
    Impl& s = *impl_;
    s.lru.SetBudget(bytes);
    if (s.initialized) {
        s.lru.MakeRoom(0, UINT64_MAX, [&](TileTexture& texture) {
            ctx_.imgui->ReleaseTexture(texture.texture);
            ++s.evictions;
        });
    }
}

void TileCache::Clear()
{
    Impl& s = *impl_;
    {
        std::lock_guard lock(s.mutex);
        ++s.epoch;  // the tile being decoded right now is dropped when it finishes
        s.requests.clear();
        for (const Impl::Done& d : s.done) {
            if (d.buffer >= 0) {
                s.freeBuffers.push_back(d.buffer);
            }
        }
        s.done.clear();
    }
    s.wake.notify_one();
    s.pending.clear();
    s.missing.clear();
    s.lru.Clear([&](TileTexture& texture) {
        if (ctx_.imgui) {
            ctx_.imgui->ReleaseTexture(texture.texture);
        }
    });
}

void TileCache::SetOnTileReady(std::function<void(const TileKey&)> callback)
{
    impl_->onReady = std::move(callback);
}

void TileCache::DrawPanelSection()
{
    Impl& s = *impl_;
    size_t waitingDecode = 0, waitingUpload = 0;
    {
        std::lock_guard lock(s.mutex);
        waitingDecode = s.requests.size();
        waitingUpload = s.done.size();
    }
    constexpr double kMB = 1024.0 * 1024.0;
    ImGui::Text("Textures: %zu", s.lru.Count());
    ImGui::Text("Memory: %.1f of %.0f MB", static_cast<double>(s.lru.Bytes()) / kMB,
                static_cast<double>(s.lru.Budget()) / kMB);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Each tile counts twice. SC4RenderServices keeps a copy of every texture.");
    }
    const uint64_t lookups = s.hits + s.misses;
    ImGui::Text("Hit rate: %.1f %% of %llu lookups", lookups ? 100.0 * static_cast<double>(s.hits) / lookups : 0.0,
                static_cast<unsigned long long>(lookups));
    ImGui::Text("Waiting: %zu to decode, %zu to upload, %zu in total", waitingDecode, waitingUpload,
                s.pending.size());
    ImGui::Text("Uploaded: %llu. Evicted: %llu. Failed: %llu. Missing: %zu",
                static_cast<unsigned long long>(s.uploads), static_cast<unsigned long long>(s.evictions),
                static_cast<unsigned long long>(s.failures), s.missing.size());
    if (s.overBudget || s.queueDrops) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Dropped: %llu over the memory limit, %llu from a full queue",
                           static_cast<unsigned long long>(s.overBudget),
                           static_cast<unsigned long long>(s.queueDrops));
    }
    if (ImGui::Button("Reload all tiles")) {
        Clear();
        if (ctx_.neighborRenderer) {
            ctx_.neighborRenderer->InvalidateAll();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drop every tile texture and read the renders from disk again.");
    }
}
