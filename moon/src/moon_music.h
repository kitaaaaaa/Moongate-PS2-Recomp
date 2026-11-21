// Lossless background music from the user's CD rips (the PC version's audio CD), in place of the ADX stream.
// See moon_music.cpp.
#pragma once

class PS2Runtime;

namespace moon_music
{
// Active when <disc folder>/../music/<cue>.flac files exist (moon/music/, made by the CD music tool);
// PS2X_MOON_MUSIC=0 turns it off.
void install(PS2Runtime &runtime);
}
