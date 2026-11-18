#pragma once

// Game message IDs, and a small bus that hands them to the modules.
//
// The director is the only cIGZMessageTarget2. It listens to every ID in
// kDirectorMessages and to every ID a module subscribes to, and dispatches them through
// MessageBus on the game thread. Modules never register with the message server themselves.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class cIGZMessage2;

namespace Messages
{
    // Sent or posted: cSC4App::PostCityMessage (0x00449af0) sends a message at once
    // (MessageSend) when its last argument is 0 and queues it (MessagePost) when it is 1. A
    // sent message reaches the director inside the game call that sends it. A posted message
    // arrives on a later message pump, after that call has returned.

    // City load, in this order from cSC4App::LoadCity (0x004555f0).
    constexpr uint32_t kPreLoad = 0x26C63340;
    constexpr uint32_t kLoad = 0x26C63341;
    constexpr uint32_t kPostLoad = 0x26C63342;
    // Sent: inside LoadCity.
    constexpr uint32_t kPreCityInit = 0x26D31EC0;
    // Posted: after LoadCity returned, so after a city switch task has ended.
    constexpr uint32_t kPostCityInit = 0x26D31EC1;
    // Posted, after kPostCityInit: every simulator finished its post-city-init work.
    constexpr uint32_t kPostCityInitComplete = 0xEA8AE29A;

    // City close, from cSC4App::PartialCloseCity (0x00454a60). It pumps both message queues
    // first, so a kPostCityInit that still waits is delivered before kPreCityShutdown.
    // Sent: inside CloseCity, while the city and its view are complete.
    constexpr uint32_t kPreCityShutdown = 0x26D31EC2;
    // Posted: arrives when the city is gone.
    constexpr uint32_t kPostCityShutdown = 0x26D31EC3;

    // City save. Sent in this order by cSC4App::SaveCity(path) (0x00455e20):
    // kPreSave, kSavePrepare, then the city data is written, then kSave, kPostSave.
    constexpr uint32_t kPreSave = 0x26C63343;
    constexpr uint32_t kSavePrepare = 0x26C63346;
    constexpr uint32_t kSave = 0x26C63344;
    constexpr uint32_t kPostSave = 0x26C63345;

    // Sent by the region thumbnail writer (0x005ddec0) around its camera change.
    constexpr uint32_t kThumbnailViewChangeBegin = 0x4AB04CBE;
    constexpr uint32_t kThumbnailViewChangeEnd = 0x4AB04CBF;

    // Posted by cSTETerrain::RedisplayTerrain (0x00749850) after the altitudes in a rectangle
    // changed. data1 = x0 << 16 | x1, data2 = z0 << 16 | z1, in cells, inclusive. See
    // docs/re-notes/overlay.md.
    constexpr uint32_t kTerrainAltitudeChanged = 0x278E6003;

    // Camera view change messages from cSC4CameraControl (0x007cc000, 0x007cc0b0).
    // The meaning of each post message is not confirmed. See docs/re-notes/foundation.md.
    constexpr uint32_t kCameraPreViewChange = 0xA6B79600;
    constexpr uint32_t kCameraPostViewChange = 0xA6B79620;

    // Every message the director always listens to.
    constexpr std::array kDirectorMessages{
        kPostCityInit,
        kPostCityInitComplete,
        kPreCityShutdown,
        kPostCityShutdown,
        kPreSave,
        kSave,
        kPostSave,
    };
}

// Fans game messages out to module callbacks. Game thread only.
class MessageBus
{
public:
    using Handler = std::function<void(cIGZMessage2* message)>;
    // Called once for each message ID that gets its first subscriber, so the director can
    // register for it with the message server.
    using RegisterFn = std::function<void(uint32_t messageId)>;

    void SetRegisterFn(RegisterFn fn) { register_ = std::move(fn); }

    // Returns a token for Unsubscribe, never 0.
    uint32_t Subscribe(const uint32_t messageId, Handler handler)
    {
        bool known = false;
        for (const auto& entry : entries_) {
            if (entry.messageId == messageId) {
                known = true;
                break;
            }
        }
        const uint32_t token = ++lastToken_;
        entries_.push_back({token, messageId, std::move(handler)});
        if (!known && register_) {
            register_(messageId);
        }
        return token;
    }

    // Safe to call from inside a handler.
    void Unsubscribe(const uint32_t token)
    {
        for (auto& entry : entries_) {
            if (entry.token == token) {
                entry.handler = nullptr;
            }
        }
        if (dispatchDepth_ == 0) {
            Compact_();
        }
    }

    void Dispatch(const uint32_t messageId, cIGZMessage2* message)
    {
        ++dispatchDepth_;
        // Index loop: handlers may subscribe (push_back) while this runs.
        const size_t count = entries_.size();
        for (size_t i = 0; i < count; ++i) {
            if (entries_[i].messageId == messageId && entries_[i].handler) {
                // Copy is cheap compared to message traffic and keeps the handler alive if it
                // unsubscribes itself.
                const Handler handler = entries_[i].handler;
                handler(message);
            }
        }
        if (--dispatchDepth_ == 0) {
            Compact_();
        }
    }

    // Every ID that has at least one subscriber.
    template <typename Fn>
    void ForEachSubscribedId(Fn&& fn) const
    {
        std::vector<uint32_t> seen;
        for (const auto& entry : entries_) {
            if (entry.handler && std::find(seen.begin(), seen.end(), entry.messageId) == seen.end()) {
                seen.push_back(entry.messageId);
                fn(entry.messageId);
            }
        }
    }

    void Clear() { entries_.clear(); }

private:
    struct Entry
    {
        uint32_t token;
        uint32_t messageId;
        Handler handler;
    };

    void Compact_()
    {
        std::erase_if(entries_, [](const Entry& e) { return !e.handler; });
    }

    std::vector<Entry> entries_;
    RegisterFn register_;
    uint32_t lastToken_ = 0;
    int dispatchDepth_ = 0;
};
