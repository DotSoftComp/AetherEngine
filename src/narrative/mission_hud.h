// Aether Engine — in-game mission/objective HUD (Detroit-style): active
// mission name, diamond-marked objective list, and completion toasts.
// Drawn with the engine's own immediate-mode UI so it renders identically in
// --game mode and inside the editor's Play viewport.
#pragma once
#include "../ui/ui.h"
#include "../engine/mission.h"

namespace ae {

void drawMissionHUD(UI& ui, const MissionSystem& missions, const Rect& area);

} // namespace ae
