#include "remote/RemoteCommands.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "GZServPtrs.h"
#include "cIGZCommandParameterSet.h"
#include "cIGZCommandServer.h"
#include "cIGZMessage2Standard.h"
#include "cIGZVariant.h"
#include "cRZBaseString.h"
#include "batch/RegionBatchRenderer.hpp"
#include "capture/CityCapturer.hpp"
#include "core/Messages.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "render/TileCache.hpp"
#include "switching/CitySwitcher.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"

namespace
{
    enum : uint32_t
    {
        kStatus = 0x5EA1C001,
        kRenderRegion = 0x5EA1C002,
        kRenderCity = 0x5EA1C003,
        kCancel = 0x5EA1C004,
        kSet = 0x5EA1C005,
        kGet = 0x5EA1C006,
        kSaveSettings = 0x5EA1C007,
        kResumeRegion = 0x5EA1C008,
        kOpenCity = 0x5EA1C009,
    };

    cIGZCommandServer::cGZCommandInfo kCommands[] = {
        {kStatus, "SeamlessStatus", ""},
        {kRenderRegion, "SeamlessRenderRegion", "[force] [transport]"},
        {kRenderCity, "SeamlessRenderCity", ""},
        {kCancel, "SeamlessCancel", ""},
        {kSet, "SeamlessSet", "<INI key> <value>"},
        {kGet, "SeamlessGet", "<INI key>"},
        {kSaveSettings, "SeamlessSaveSettings", ""},
        {kResumeRegion, "SeamlessResumeRegion", ""},
        {kOpenCity, "SeamlessOpenCity", "<city name>"},
    };

    std::vector<std::string> Arguments(cIGZCommandParameterSet* input)
    {
        std::vector<std::string> args;
        for (uint32_t i = 0; input && i < input->GetParameterCount(); ++i) {
            cRZBaseString text;
            if (cIGZVariant* value = input->GetParameter(i); value && value->GetValString(text)) {
                args.emplace_back(text.ToChar() ? text.ToChar() : "");
            }
        }
        return args;
    }

    void Reply(cIGZCommandParameterSet* output, const bool ok, const std::string& text)
    {
        if (!output) {
            return;
        }
        output->SetParameterCount(0);
        output->SetParameterCount(1);
        if (cIGZVariant* value = output->GetParameter(0)) {
            value->SetValString(cRZBaseString(text.c_str()));
        }
        output->SetStatusParameterValue(ok ? 0 : 1);
    }
}

bool RemoteCommands::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    cIGZCommandServerPtr server;
    if (!server || !ctx_.messages || !ctx_.settings) {
        LOG_WARN("Remote: the game's command server is not available, no remote commands");
        return false;
    }
    if (!server->RegisterCommands(kCommands, static_cast<uint32_t>(std::size(kCommands)))) {
        LOG_WARN("Remote: the game did not take the commands");
        return false;
    }
    registered_ = true;
    for (size_t i = 0; i < std::size(kCommands); ++i) {
        tokens_[i] = ctx_.messages->Subscribe(kCommands[i].dwCommandID, [this](cIGZMessage2* m) { OnCommand_(m); });
    }
    LOG_INFO("Remote: {} commands registered (SeamlessStatus, SeamlessRenderRegion, ...)", std::size(kCommands));
    return true;
}

void RemoteCommands::Shutdown()
{
    for (uint32_t& token : tokens_) {
        if (token && ctx_.messages) {
            ctx_.messages->Unsubscribe(token);
        }
        token = 0;
    }
    if (registered_) {
        if (cIGZCommandServerPtr server; server) {
            server->UnregisterCommands(kCommands, static_cast<uint32_t>(std::size(kCommands)));
        }
        registered_ = false;
    }
}

void RemoteCommands::OnCommand_(cIGZMessage2* message)
{
    // A command message carries the input and output parameter sets (cIGZCommandServer).
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    auto* input = static_cast<cIGZCommandParameterSet*>(standard->GetVoid1());
    auto* output = static_cast<cIGZCommandParameterSet*>(standard->GetVoid2());
    std::string reply;
    const bool ok = Run_(message->GetType(), input, reply);
    LOG_INFO("Remote: 0x{:08X} -> {}{}", message->GetType(), ok ? "" : "failed: ", reply);
    Reply(output, ok, reply);
}

bool RemoteCommands::Run_(const uint32_t id, cIGZCommandParameterSet* input, std::string& reply)
{
    const std::vector<std::string> args = Arguments(input);
    Settings& settings = *ctx_.settings;
    switch (id) {
    case kStatus: {
        const CityInfo* city = ctx_.region ? ctx_.region->Current() : nullptr;
        const CaptureProgress capture = ctx_.capturer ? ctx_.capturer->Progress() : CaptureProgress();
        reply = std::format("view={} city={} batch={} render={} {}/{} eta={:.0f}s date={} zoom={}",
                            GameAccess::City() ? "city" : "region", city ? city->name : std::string("-"),
                            ctx_.batch ? ctx_.batch->StatusText() : std::string("-"),
                            ctx_.capturer && ctx_.capturer->IsRunning() ? capture.phase : std::string("idle"),
                            capture.done, capture.total, capture.etaSeconds,
                            settings.captureAtDate
                                ? std::format("{}/{}", settings.captureMonth, settings.captureDay)
                                : std::string("off"),
                            settings.maxRenderZoom);
        if (ctx_.tileCache) {
            reply += " " + ctx_.tileCache->StatsText();
        }
        return true;
    }
    case kRenderRegion: {
        const bool force = std::ranges::find(args, "force") != args.end();
        const bool transport = std::ranges::find(args, "transport") != args.end();
        if (!ctx_.batch || !ctx_.batch->Start(force, transport)) {
            reply = ctx_.batch ? ctx_.batch->Message() : std::string("no region render");
            return false;
        }
        reply = force ? "the region render started for every city" : "the region render started";
        return true;
    }
    case kResumeRegion:
        if (!ctx_.batch || !ctx_.batch->ResumeLast()) {
            reply = ctx_.batch ? ctx_.batch->Message() : std::string("no region render");
            return false;
        }
        reply = "the region render continues";
        return true;
    case kOpenCity: {
        // A city switch without a save (or the city opens from the region view).
        std::string name;
        for (const std::string& part : args) {
            name += (name.empty() ? "" : " ") + part;
        }
        const CityInfo* city = nullptr;
        for (const CityInfo& c : ctx_.region ? ctx_.region->Cities() : std::vector<CityInfo>()) {
            if (c.name == name && c.founded) {
                city = &c;
            }
        }
        if (!city || !ctx_.switcher) {
            reply = "no founded city named " + name;
            return false;
        }
        const bool started = GameAccess::City() ? ctx_.switcher->RequestSwitch(city->cellX, city->cellZ, true, nullptr, false)
                                                : ctx_.switcher->RequestOpen(city->cellX, city->cellZ);
        reply = started ? "opening " + name : ctx_.switcher->LastError();
        return started;
    }
    case kRenderCity:
        if (!ctx_.capturer || !ctx_.capturer->Start(CaptureRequest())) {
            reply = ctx_.capturer ? ctx_.capturer->Progress().error : std::string("no capture");
            return false;
        }
        reply = "the render of this city started";
        return true;
    case kCancel:
        if (ctx_.batch && ctx_.batch->IsRunning()) {
            ctx_.batch->Cancel();
            reply = "the region render stops after this step";
        }
        else if (ctx_.capturer && ctx_.capturer->IsRunning()) {
            ctx_.capturer->Cancel();
            reply = "the render stops";
        }
        else {
            reply = "nothing runs";
        }
        return true;
    case kSet: {
        if (args.size() < 2) {
            reply = "usage: SeamlessSet <INI key> <value>";
            return false;
        }
        std::string value = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            value += " " + args[i];
        }
        if (!settings.Set(args[0], value)) {
            reply = "unknown key " + args[0];
            return false;
        }
        reply = args[0] + "=" + settings.Get(args[0]);
        return true;
    }
    case kGet:
        if (args.empty() || settings.Get(args[0]).empty()) {
            reply = args.empty() ? "usage: SeamlessGet <INI key>" : "unknown key " + args[0];
            return false;
        }
        reply = args[0] + "=" + settings.Get(args[0]);
        return true;
    case kSaveSettings:
        if (!settings.Save(ctx_.settingsPath)) {
            reply = "the settings could not be written";
            return false;
        }
        reply = "saved";
        return true;
    default:
        reply = "unknown command";
        return false;
    }
}

void RemoteCommands::DrawPanelSection()
{
    ImGui::TextUnformatted("Game commands (cheat box, Lua scripts, -NetCommandGenerator:enabled on port 50020):");
    for (const auto& command : kCommands) {
        ImGui::BulletText("%s %s", command.pszCommandName, command.pszParameterInfo);
    }
}
