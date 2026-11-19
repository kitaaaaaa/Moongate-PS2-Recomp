// PC display options in the game's own DISPLAY SETTING menu. See moon_display.cpp.
#pragma once

class PS2Runtime;

namespace moon_display
{
// PS2X_MOON_DISPLAY_MENU=0 leaves the game's menu as it is.
void install(PS2Runtime &runtime);
}
