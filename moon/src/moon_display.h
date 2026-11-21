// PC display options in the game's own DISPLAY SETTING menu. See moon_display.cpp.
#pragma once

class PS2Runtime;

namespace moon_display
{
// PS2X_MOON_DISPLAY_MENU=0 leaves the game's menu as it is.
void install(PS2Runtime &runtime);
// HUD ASPECT 4:3 with SCREEN MODE WIDE: the factor (< 1) by which the HUD and menus are squeezed horizontally towards
// the centre of the wide picture; 1 otherwise.
float hudSqueeze();
}
