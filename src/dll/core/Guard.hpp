#pragma once

// Keeps C++ exceptions inside the plugin. The game and SC4RenderServices call the plugin
// through plain function pointers and virtual calls that were not built to unwind: an
// exception that leaves a callback ends the game. Wrap the body of every entry point they
// call (draw pass callbacks, ImGui callbacks, input controls, message handlers) in Guarded.

#include <exception>

#include "utils/Logger.h"

namespace Guard
{
    // The first failures are logged, later ones only counted: a callback that fails does so
    // on every frame.
    inline constexpr int kMaxLoggedFailures = 20;
    inline int gLoggedFailures = 0;

    inline void Report(const char* what, const char* why) noexcept
    {
        try {
            if (gLoggedFailures < kMaxLoggedFailures) {
                ++gLoggedFailures;
                LOG_ERROR("{} failed: {}{}", what, why,
                          gLoggedFailures == kMaxLoggedFailures ? ". Later failures are not logged." : "");
            }
        }
        catch (...) {
        }
    }
}

// Runs fn. Returns false when it threw.
template <typename Fn>
bool Guarded(const char* what, Fn&& fn) noexcept
{
    try {
        fn();
        return true;
    }
    catch (const std::exception& e) {
        Guard::Report(what, e.what());
    }
    catch (...) {
        Guard::Report(what, "unknown exception");
    }
    return false;
}
