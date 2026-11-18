#pragma once

// Game constants the interaction module (W3-F) uses, for the Windows build 1.1.641. Evidence
// in docs/re-notes/interaction.md.

#include <cstdint>

namespace InteractionAddresses {
// cISC4ViewInputControl::GetID of the game's query tool. cSC4View3DWin::ProcessCommand
// (0x007f1c10) compares the current control with these before it creates a new
// cSC4ViewInputControlQuery (constructor 0x004c4590): 0xc7af928e at 0x007f278e (query type
// 0) and 0xc7af928f at 0x007f2730 (query type 1).
constexpr uint32_t kQueryToolId = 0xC7AF928E;
constexpr uint32_t kQueryToolIdAlt = 0xC7AF928F;

// The id of the plugin's own persistent view input control. Random, only used to tell it
// apart in the game's lists.
constexpr uint32_t kPickerControlId = 0x3B9E61D4;

// The ImGui panel that draws the tooltip and the confirm popup. Not a window, it only
// draws at the cursor.
constexpr uint32_t kOverlayPanelId = 0x57F1F26C;
} // namespace InteractionAddresses
