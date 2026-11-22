#ifndef PS2_CONTROLS_PANEL_H
#define PS2_CONTROLS_PANEL_H

// The "Controls" window (F2): edits the ps2input bindings of both PS2 pad ports.
namespace ps2controls
{
    // Host thread, once per frame before any ImGui frame is begun: handles F2 and a pending press-to-bind.
    // Returns true when the window should be drawn this frame.
    bool update();
    // Host thread, inside rlImGuiBegin/End.
    void draw();
}

#endif
