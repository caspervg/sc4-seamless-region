#pragma once

// Remote control: game commands for the cheat box, Lua test scripts and the command socket of
// the game (-NetCommandGenerator:enabled, port 50020, one command per connection). They are
// registered with the game's command server and arrive as messages through the MessageBus.
// docs/remote.md lists them.

#include <array>
#include <cstdint>

#include "core/GameAccess.hpp"

class cIGZCommandParameterSet;
class cIGZMessage2;

class RemoteCommands final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Remote"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override {}
    void OnCityShutdown() override {}
    void DrawPanelSection() override;

private:
    void OnCommand_(cIGZMessage2* message);
    // Runs one command. False with the reason in reply when it failed.
    bool Run_(uint32_t id, cIGZCommandParameterSet* input, std::string& reply);

    ModuleContext ctx_;
    bool registered_ = false;
    std::array<uint32_t, 8> tokens_{};
};
