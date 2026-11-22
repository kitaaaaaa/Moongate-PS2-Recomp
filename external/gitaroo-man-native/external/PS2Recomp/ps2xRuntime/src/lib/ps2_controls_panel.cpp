#include "ps2_controls_panel.h"
#include "runtime/ps2_input.h"

#include "imgui.h"
#include "raylib.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ps2controls
{
    namespace
    {
        bool g_open = false;
        // The control waiting for a press-to-bind, or -1.
        int g_capturePort = -1;
        int g_captureControl = -1;
        // Twin-stick picks, kept by identity so they survive devices coming and going.
        std::string g_twinGuid[2];
        int g_twinInstance[2] = {0, 0};
        bool g_twinSet[2] = {false, false};

        std::string deviceLabel(const ps2input::Device &device)
        {
            std::string label = device.name;
            if (device.jid >= 0)
            {
                label += device.gamepad ? " (gamepad)" : "";
                label += " #" + std::to_string(device.jid);
            }
            return label;
        }

        bool sameDevice(const ps2input::Device &device, const std::string &guid, int instance)
        {
            return device.jid >= 0 && device.guid == guid && device.instance == instance;
        }

        void startCapture(int port, int control)
        {
            ps2input::Input::get().beginCapture();
            g_capturePort = port;
            g_captureControl = control;
            // Keys go to the capture, not to ImGui (Space/Enter would press a focused button, Tab moves focus),
            // and Escape cancels instead of closing the window.
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
            SetExitKey(KEY_NULL);
        }

        void endCapture()
        {
            g_capturePort = -1;
            g_captureControl = -1;
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
            SetExitKey(KEY_ESCAPE);
        }

        void pollPendingCapture()
        {
            if (g_capturePort < 0)
            {
                return;
            }
            ps2input::Input &input = ps2input::Input::get();
            ps2input::Source source;
            bool cancelled = false;
            if (!input.pollCapture(source, cancelled))
            {
                return;
            }
            if (!cancelled && source.kind != ps2input::SourceKind::None)
            {
                ps2input::Bindings b = input.bindings(g_capturePort);
                std::vector<ps2input::Source> &list = b.controls[g_captureControl];
                bool present = false;
                for (const ps2input::Source &s : list)
                {
                    present = present || s == source;
                }
                if (!present)
                {
                    list.push_back(source);
                    input.setBindings(g_capturePort, b);
                }
            }
            endCapture();
        }

        void drawPortTab(int port, const std::vector<ps2input::Device> &devices)
        {
            ps2input::Input &input = ps2input::Input::get();

            if (ImGui::BeginCombo("Add a device's default layout", "Choose a device..."))
            {
                for (size_t i = 0; i < devices.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(deviceLabel(devices[i]).c_str()))
                    {
                        input.applyDeviceDefaults(port, devices[i]);
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset all to defaults"))
            {
                input.resetDefaults();
            }
            ImGui::TextDisabled("Port %d: %s", port + 1, input.portConnected(port) ? "connected" : "unplugged");

            ps2input::Bindings b = input.bindings(port);
            bool changed = false;
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("bindings", 3, flags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();
                for (int c = 0; c < ps2input::ControlCount; ++c)
                {
                    ImGui::PushID(c);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ps2input::controlName(c));

                    ImGui::TableNextColumn();
                    std::vector<ps2input::Source> &list = b.controls[c];
                    if (g_capturePort == port && g_captureControl == c)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press an input... (Esc cancels)");
                    }
                    else if (list.empty())
                    {
                        ImGui::TextDisabled("-");
                    }
                    for (size_t i = 0; i < list.size(); ++i)
                    {
                        ImGui::PushID(static_cast<int>(i));
                        if (i > 0 || (g_capturePort == port && g_captureControl == c))
                        {
                            ImGui::SameLine(0.0f, 12.0f);
                        }
                        ImGui::TextUnformatted(ps2input::describe(list[i]).c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("x"))
                        {
                            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                            changed = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }

                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(g_capturePort >= 0);
                    if (ImGui::SmallButton("Bind"))
                    {
                        startCapture(port, c);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear") && !list.empty())
                    {
                        list.clear();
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (changed)
            {
                input.setBindings(port, b);
            }
        }

        // Combo over the joysticks; the pick is stored by GUID + instance.
        void drawTwinCombo(const char *label, int which, const std::vector<ps2input::Device> &devices)
        {
            const ps2input::Device *current = nullptr;
            for (const ps2input::Device &d : devices)
            {
                if (g_twinSet[which] && sameDevice(d, g_twinGuid[which], g_twinInstance[which]))
                {
                    current = &d;
                }
            }
            const std::string preview = current ? deviceLabel(*current) : std::string("Choose a device...");
            if (ImGui::BeginCombo(label, preview.c_str()))
            {
                for (size_t i = 0; i < devices.size(); ++i)
                {
                    const ps2input::Device &d = devices[i];
                    if (d.jid < 0)
                    {
                        continue;
                    }
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(deviceLabel(d).c_str(), current == &d))
                    {
                        g_twinGuid[which] = d.guid;
                        g_twinInstance[which] = d.instance;
                        g_twinSet[which] = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }

        void drawTwinStickTab(const std::vector<ps2input::Device> &devices)
        {
            ImGui::TextWrapped("Twin-stick setup for two joysticks as the left and right levers, on Player 1. "
                               "Left lever (device A): its stick -> left stick, trigger -> L1, second button -> L2. "
                               "Right lever (device B): its stick -> right stick, trigger -> R1, second button -> R2. "
                               "Start comes from either. This replaces Player 1's bindings.");
            ImGui::Spacing();
            drawTwinCombo("Device A (left lever)", 0, devices);
            drawTwinCombo("Device B (right lever)", 1, devices);

            const ps2input::Device *a = nullptr;
            const ps2input::Device *b = nullptr;
            for (const ps2input::Device &d : devices)
            {
                if (g_twinSet[0] && sameDevice(d, g_twinGuid[0], g_twinInstance[0]))
                {
                    a = &d;
                }
                if (g_twinSet[1] && sameDevice(d, g_twinGuid[1], g_twinInstance[1]))
                {
                    b = &d;
                }
            }
            ImGui::BeginDisabled(!a || !b);
            if (ImGui::Button("Apply to Player 1"))
            {
                ps2input::Input::get().applyTwinStick(*a, *b);
            }
            ImGui::EndDisabled();
        }

        void drawDevicesTab(const std::vector<ps2input::Device> &devices)
        {
            ImGui::TextWrapped("Connected devices and their live raw values. Move an axis or press a button to find "
                               "its number.");
            for (size_t i = 0; i < devices.size(); ++i)
            {
                const ps2input::Device &d = devices[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::CollapsingHeader(deviceLabel(d).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (d.jid < 0)
                    {
                        ImGui::TextDisabled("Keyboard: use Bind to see key names.");
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::TextDisabled("GUID %s  instance %d", d.guid.c_str(), d.instance);
                    for (size_t a = 0; a < d.axes.size(); ++a)
                    {
                        char text[32];
                        std::snprintf(text, sizeof(text), "%+.3f", d.axes[a]);
                        ImGui::ProgressBar((d.axes[a] + 1.0f) * 0.5f, ImVec2(220.0f, 0.0f), text);
                        ImGui::SameLine();
                        ImGui::Text("axis %zu", a);
                    }
                    if (!d.buttons.empty())
                    {
                        ImGui::TextUnformatted("Buttons:");
                        for (size_t k = 0; k < d.buttons.size(); ++k)
                        {
                            ImGui::SameLine();
                            if (d.buttons[k])
                            {
                                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%zu", k);
                            }
                            else
                            {
                                ImGui::TextDisabled("%zu", k);
                            }
                        }
                    }
                    for (size_t h = 0; h < d.hats.size(); ++h)
                    {
                        const uint8_t v = d.hats[h];
                        ImGui::Text("hat %zu: %s%s%s%s", h, (v & 1) ? "up " : "", (v & 2) ? "right " : "",
                                    (v & 4) ? "down " : "", (v & 8) ? "left " : "");
                    }
                }
                ImGui::PopID();
            }
        }
    }

    bool update()
    {
        if (IsKeyPressed(KEY_F2) && g_capturePort < 0)
        {
            g_open = !g_open;
        }
        // Capture keeps running while the window is closed so the exit key and ImGui keyboard get restored.
        pollPendingCapture();
        return g_open;
    }

    void draw()
    {
        ps2input::Input &input = ps2input::Input::get();
        const std::vector<ps2input::Device> devices = input.devices();

        ImGui::SetNextWindowSize(ImVec2(640.0f, 600.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls", &g_open))
        {
            ImGui::TextDisabled("Bindings file: %s", input.path().c_str());
            if (ImGui::BeginTabBar("controls-tabs"))
            {
                if (ImGui::BeginTabItem("Player 1"))
                {
                    drawPortTab(0, devices);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Player 2"))
                {
                    drawPortTab(1, devices);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Twin-stick"))
                {
                    drawTwinStickTab(devices);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Devices"))
                {
                    drawDevicesTab(devices);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }
}
