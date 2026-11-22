#include "runtime/ps2_input.h"
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#if !defined(PLATFORM_VITA)
// raylib's desktop platform is built on the GLFW it bundles; these are not exposed by raylib.h.
extern "C"
{
    struct GLFWgamepadstate
    {
        unsigned char buttons[15];
        float axes[6];
    };
    int glfwJoystickPresent(int jid);
    int glfwJoystickIsGamepad(int jid);
    const char *glfwGetJoystickGUID(int jid);
    const char *glfwGetJoystickName(int jid);
    const float *glfwGetJoystickAxes(int jid, int *count);
    const unsigned char *glfwGetJoystickButtons(int jid, int *count);
    const unsigned char *glfwGetJoystickHats(int jid, int *count);
    int glfwGetGamepadState(int jid, GLFWgamepadstate *state);
}
#endif

namespace ps2input
{
    namespace
    {
        constexpr int kMaxJoysticks = 16;
        constexpr int kKeyCount = 350; // raylib key codes are GLFW's, all below 349
        constexpr float kStickDeadzone = 0.15f;
        constexpr float kPressThreshold = 0.5f;
        // raylib's desktop backend reports L2/R2 down when the raw trigger axis passes 0.1, i.e. trigger value 0.55.
        constexpr float kTriggerPressThreshold = 0.55f;
        // A raw axis rest is learned once the value has stayed within kRestStableRange for kRestStableRefreshes.
        constexpr float kRestStableRange = 0.02f;
        constexpr int kRestStableRefreshes = 30;

        // GLFW gamepad button / axis indices.
        constexpr int kGlfwA = 0, kGlfwB = 1, kGlfwX = 2, kGlfwY = 3, kGlfwLeftBumper = 4, kGlfwRightBumper = 5;
        constexpr int kGlfwBack = 6, kGlfwStart = 7, kGlfwGuide = 8, kGlfwLeftThumb = 9, kGlfwRightThumb = 10;
        constexpr int kGlfwDpadUp = 11, kGlfwDpadRight = 12, kGlfwDpadDown = 13, kGlfwDpadLeft = 14;
        constexpr int kGlfwLeftTrigger = 4, kGlfwRightTrigger = 5;

        // raylib GamepadButton -> GLFW gamepad button; -1 = none, -2/-3 = left/right trigger axis.
        constexpr int kButtonToGlfw[] = {
            -1,             // UNKNOWN
            kGlfwDpadUp,    // LEFT_FACE_UP
            kGlfwDpadRight, // LEFT_FACE_RIGHT
            kGlfwDpadDown,  // LEFT_FACE_DOWN
            kGlfwDpadLeft,  // LEFT_FACE_LEFT
            kGlfwY,         // RIGHT_FACE_UP
            kGlfwB,         // RIGHT_FACE_RIGHT
            kGlfwA,         // RIGHT_FACE_DOWN
            kGlfwX,         // RIGHT_FACE_LEFT
            kGlfwLeftBumper,  // LEFT_TRIGGER_1
            -2,               // LEFT_TRIGGER_2
            kGlfwRightBumper, // RIGHT_TRIGGER_1
            -3,               // RIGHT_TRIGGER_2
            kGlfwBack,      // MIDDLE_LEFT
            kGlfwGuide,     // MIDDLE
            kGlfwStart,     // MIDDLE_RIGHT
            kGlfwLeftThumb, // LEFT_THUMB
            kGlfwRightThumb // RIGHT_THUMB
        };
        constexpr int kButtonCodes = static_cast<int>(sizeof(kButtonToGlfw) / sizeof(kButtonToGlfw[0]));

        const char *const kControlNames[ControlCount] = {
            "Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left", "L2", "R2", "L1", "R1",
            "Triangle", "Circle", "Cross", "Square",
            "Left stick left", "Left stick right", "Left stick up", "Left stick down",
            "Right stick left", "Right stick right", "Right stick up", "Right stick down"};
        const char *const kControlKeys[ControlCount] = {
            "select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1",
            "triangle", "circle", "cross", "square",
            "lstick_left", "lstick_right", "lstick_up", "lstick_down",
            "rstick_left", "rstick_right", "rstick_up", "rstick_down"};
        const char *const kKindNames[] = {"none", "key", "gpbutton", "gpaxis", "joybutton", "joyaxis", "joyhat"};

        // A polled joystick plus what the evaluation needs beyond the public Device.
        struct Polled
        {
            Device dev;
            std::array<uint8_t, 15> gpButtons{};
            std::array<float, 6> gpAxes{};
            std::vector<float> rest; // raw axis rest values (see AxisRest)
        };

        // Rest tracking of one raw axis: wheels and pedals rest at -1 or +1, sticks at 0.
        struct AxisRest
        {
            bool learned = false;
            float rest = 0.0f;
            float anchor = 0.0f; // value the stable run started at
            int stable = 0;      // refreshes the value has stayed within kRestStableRange of anchor
        };

        float snapRest(float a)
        {
            return a < -0.5f ? -1.0f : a > 0.5f ? 1.0f : 0.0f;
        }

        // Updates the rest of an axis with this refresh's value and returns the rest to use.
        float updateRest(AxisRest &r, float a)
        {
            if (std::fabs(a - r.anchor) > kRestStableRange)
            {
                r.anchor = a;
                r.stable = 0;
            }
            else if (r.stable < kRestStableRefreshes)
            {
                ++r.stable;
            }
            if (r.learned && r.rest != 0.0f)
            {
                // Travel runs from the rest towards the other end; a value past the rest the other way means
                // the learned rest was wrong.
                const float sign = r.rest < 0.0f ? 1.0f : -1.0f;
                if (sign * (a - r.rest) < -0.5f)
                {
                    r.learned = false;
                    r.anchor = a;
                    r.stable = 0;
                }
            }
            if (!r.learned && r.stable >= kRestStableRefreshes)
            {
                r.learned = true;
                r.rest = snapRest(r.anchor);
            }
            // A new stable resting position replaces the learned one: near 0 (a stick settling at centre fixes a
            // wrong +-1 rest), or at the opposite extreme of a +-1 rest. Never from a centre rest to an extreme: a
            // stick or wheel held fully to one side would otherwise stop reading after half a second.
            if (r.learned && r.stable >= kRestStableRefreshes && snapRest(r.anchor) != r.rest &&
                (std::fabs(r.anchor) < 0.1f || (r.rest != 0.0f && std::fabs(r.anchor) > 0.99f)))
            {
                r.rest = snapRest(r.anchor);
            }
            return r.learned ? r.rest : snapRest(a);
        }

        struct World
        {
            std::vector<Polled> joys;
            std::array<bool, kKeyCount> keys{};
        };

        struct PortSnapshot
        {
            uint16_t buttons = 0xFFFFu;
            uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
            bool connected = false;
        };

        std::string deviceKey(const std::string &guid, int instance)
        {
            return guid + "#" + std::to_string(instance);
        }

        const Polled *findJoy(const World &world, const std::string &guid, int instance)
        {
            for (const Polled &p : world.joys)
            {
                if (p.dev.guid == guid && p.dev.instance == instance)
                    return &p;
            }
            return nullptr;
        }

        bool sameDevice(const Source &s, const Device &d)
        {
            if (d.jid < 0)
                return s.kind == SourceKind::Key;
            return s.kind != SourceKind::Key && s.kind != SourceKind::None && s.guid == d.guid && s.instance == d.instance;
        }

        float triggerValue(float axis)
        {
            // GLFW gamepad triggers rest at -1.
            return std::clamp((axis + 1.0f) * 0.5f, 0.0f, 1.0f);
        }

        float gamepadButtonValue(const Polled &p, int code)
        {
            if (code < 0 || code >= kButtonCodes)
                return 0.0f;
            const int index = kButtonToGlfw[code];
            if (index == -2)
                return triggerValue(p.gpAxes[kGlfwLeftTrigger]);
            if (index == -3)
                return triggerValue(p.gpAxes[kGlfwRightTrigger]);
            if (index < 0)
                return 0.0f;
            return p.gpButtons[index] ? 1.0f : 0.0f;
        }

        float gamepadAxisValue(const Polled &p, int code, int sign)
        {
            if (code < 0 || code >= 6)
                return 0.0f;
            if (code == kGlfwLeftTrigger || code == kGlfwRightTrigger)
                return sign > 0 ? triggerValue(p.gpAxes[code]) : 0.0f;
            return std::clamp(static_cast<float>(sign) * p.gpAxes[code], 0.0f, 1.0f);
        }

        // Travel of a raw axis away from its rest value in the bound direction.
        float joyAxisDelta(const Polled &p, int code, int sign)
        {
            if (code < 0 || code >= static_cast<int>(p.dev.axes.size()))
                return 0.0f;
            const float rest = code < static_cast<int>(p.rest.size()) ? p.rest[code] : 0.0f;
            return static_cast<float>(sign) * (p.dev.axes[code] - rest);
        }

        float joyAxisValue(const Polled &p, int code, int sign)
        {
            const float rest = code >= 0 && code < static_cast<int>(p.rest.size()) ? p.rest[code] : 0.0f;
            const float span = 1.0f - static_cast<float>(sign) * rest;
            if (span < 0.25f)
                return 0.0f;
            return std::clamp(joyAxisDelta(p, code, sign) / span, 0.0f, 1.0f);
        }

        // Analog strength 0..1 of a source.
        float sourceValue(const World &world, const Source &s)
        {
            if (s.kind == SourceKind::Key)
                return s.code > 0 && s.code < kKeyCount && world.keys[s.code] ? 1.0f : 0.0f;
            const Polled *p = findJoy(world, s.guid, s.instance);
            if (!p)
                return 0.0f;
            switch (s.kind)
            {
            case SourceKind::GamepadButton:
                return p->dev.gamepad ? gamepadButtonValue(*p, s.code) : 0.0f;
            case SourceKind::GamepadAxis:
                return p->dev.gamepad ? gamepadAxisValue(*p, s.code, s.sign) : 0.0f;
            case SourceKind::JoyButton:
                return s.code >= 0 && s.code < static_cast<int>(p->dev.buttons.size()) && p->dev.buttons[s.code] ? 1.0f : 0.0f;
            case SourceKind::JoyAxis:
                return joyAxisValue(*p, s.code, s.sign);
            case SourceKind::JoyHat:
                return s.code >= 0 && s.code < static_cast<int>(p->dev.hats.size()) && (p->dev.hats[s.code] & s.sign) ? 1.0f : 0.0f;
            default:
                return 0.0f;
            }
        }

        bool isGamepadTrigger(const Source &s)
        {
            if (s.kind == SourceKind::GamepadButton)
                return s.code >= 0 && s.code < kButtonCodes && (kButtonToGlfw[s.code] == -2 || kButtonToGlfw[s.code] == -3);
            if (s.kind == SourceKind::GamepadAxis)
                return (s.code == kGlfwLeftTrigger || s.code == kGlfwRightTrigger) && s.sign > 0;
            return false;
        }

        bool sourceActive(const World &world, const Source &s)
        {
            if (s.kind == SourceKind::JoyAxis)
            {
                const Polled *p = findJoy(world, s.guid, s.instance);
                return p && joyAxisDelta(*p, s.code, s.sign) > kPressThreshold;
            }
            return sourceValue(world, s) > (isGamepadTrigger(s) ? kTriggerPressThreshold : kPressThreshold);
        }

        bool sourceConnected(const World &world, const Source &s)
        {
            if (s.kind == SourceKind::Key)
                return true;
            return s.kind != SourceKind::None && findJoy(world, s.guid, s.instance) != nullptr;
        }

        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            return static_cast<uint8_t>(std::lround((axis + 1.0f) * 127.5f));
        }

        // Radial deadzone; the remaining travel is rescaled to the full range.
        void applyDeadzone(float &x, float &y)
        {
            const float magnitude = std::sqrt(x * x + y * y);
            if (magnitude <= kStickDeadzone)
            {
                x = y = 0.0f;
                return;
            }
            const float scale = std::min(1.0f, (magnitude - kStickDeadzone) / (1.0f - kStickDeadzone)) / magnitude;
            x *= scale;
            y *= scale;
        }

        PortSnapshot evaluate(const World &world, const Bindings &b)
        {
            PortSnapshot out;
            for (int c = 0; c < ControlCount; ++c)
            {
                for (const Source &s : b.controls[c])
                {
                    if (sourceConnected(world, s))
                        out.connected = true;
                }
            }
            for (int c = 0; c <= Square; ++c)
            {
                for (const Source &s : b.controls[c])
                {
                    if (sourceActive(world, s))
                    {
                        out.buttons &= static_cast<uint16_t>(~(1u << c));
                        break;
                    }
                }
            }
            auto strongest = [&](int control)
            {
                float v = 0.0f;
                for (const Source &s : b.controls[control])
                    v = std::max(v, sourceValue(world, s));
                return v;
            };
            float lx = strongest(LeftStickRight) - strongest(LeftStickLeft);
            float ly = strongest(LeftStickDown) - strongest(LeftStickUp);
            float rx = strongest(RightStickRight) - strongest(RightStickLeft);
            float ry = strongest(RightStickDown) - strongest(RightStickUp);
            applyDeadzone(lx, ly);
            applyDeadzone(rx, ry);
            out.lx = axisToByte(lx);
            out.ly = axisToByte(ly);
            out.rx = axisToByte(rx);
            out.ry = axisToByte(ry);
            return out;
        }

        Source keySource(int key)
        {
            Source s;
            s.kind = SourceKind::Key;
            s.code = key;
            return s;
        }

        Source deviceSource(const Device &d, SourceKind kind, int code, int sign = 0)
        {
            Source s;
            s.kind = kind;
            s.guid = d.guid;
            s.instance = d.instance;
            s.code = code;
            s.sign = sign;
            return s;
        }

        void addKeyboardDefaults(Bindings &b)
        {
            auto add = [&b](int control, int key) { b.controls[control].push_back(keySource(key)); };
            add(Up, KEY_UP);
            add(Right, KEY_RIGHT);
            add(Down, KEY_DOWN);
            add(Left, KEY_LEFT);
            add(Cross, KEY_X);
            add(Cross, KEY_SPACE);
            add(Circle, KEY_C);
            add(Square, KEY_Z);
            add(Square, KEY_KP_0);
            add(Triangle, KEY_V);
            add(Triangle, KEY_KP_1);
            add(L1, KEY_Q);
            add(R1, KEY_E);
            add(L2, KEY_LEFT_SHIFT);
            add(R2, KEY_RIGHT_SHIFT);
            add(Start, KEY_ENTER);
            add(Select, KEY_TAB);
            add(LeftStickLeft, KEY_A);
            add(LeftStickRight, KEY_D);
            add(LeftStickUp, KEY_W);
            add(LeftStickDown, KEY_S);
            add(RightStickLeft, KEY_J);
            add(RightStickRight, KEY_L);
            add(RightStickUp, KEY_I);
            add(RightStickDown, KEY_K);
        }

        void addDeviceDefaults(Bindings &b, const Device &d)
        {
            if (d.jid < 0)
            {
                addKeyboardDefaults(b);
                return;
            }
            if (d.gamepad)
            {
                auto button = [&](int control, int code)
                { b.controls[control].push_back(deviceSource(d, SourceKind::GamepadButton, code)); };
                auto axis = [&](int control, int code, int sign)
                { b.controls[control].push_back(deviceSource(d, SourceKind::GamepadAxis, code, sign)); };
                button(Cross, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
                button(Circle, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
                button(Square, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
                button(Triangle, GAMEPAD_BUTTON_RIGHT_FACE_UP);
                button(L1, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
                button(R1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
                axis(L2, GAMEPAD_AXIS_LEFT_TRIGGER, 1);
                axis(R2, GAMEPAD_AXIS_RIGHT_TRIGGER, 1);
                button(Start, GAMEPAD_BUTTON_MIDDLE_RIGHT);
                button(Select, GAMEPAD_BUTTON_MIDDLE_LEFT);
                button(L3, GAMEPAD_BUTTON_LEFT_THUMB);
                button(R3, GAMEPAD_BUTTON_RIGHT_THUMB);
                button(Up, GAMEPAD_BUTTON_LEFT_FACE_UP);
                button(Right, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
                button(Down, GAMEPAD_BUTTON_LEFT_FACE_DOWN);
                button(Left, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
                axis(LeftStickLeft, GAMEPAD_AXIS_LEFT_X, -1);
                axis(LeftStickRight, GAMEPAD_AXIS_LEFT_X, 1);
                axis(LeftStickUp, GAMEPAD_AXIS_LEFT_Y, -1);
                axis(LeftStickDown, GAMEPAD_AXIS_LEFT_Y, 1);
                axis(RightStickLeft, GAMEPAD_AXIS_RIGHT_X, -1);
                axis(RightStickRight, GAMEPAD_AXIS_RIGHT_X, 1);
                axis(RightStickUp, GAMEPAD_AXIS_RIGHT_Y, -1);
                axis(RightStickDown, GAMEPAD_AXIS_RIGHT_Y, 1);
                return;
            }
            const int axes = static_cast<int>(d.axes.size());
            const int buttons = static_cast<int>(d.buttons.size());
            auto axis = [&](int control, int code, int sign)
            {
                if (code < axes)
                    b.controls[control].push_back(deviceSource(d, SourceKind::JoyAxis, code, sign));
            };
            axis(LeftStickLeft, 0, -1);
            axis(LeftStickRight, 0, 1);
            axis(LeftStickUp, 1, -1);
            axis(LeftStickDown, 1, 1);
            axis(RightStickLeft, 2, -1);
            axis(RightStickRight, 2, 1);
            axis(RightStickUp, 3, -1);
            axis(RightStickDown, 3, 1);
            if (!d.hats.empty())
            {
                b.controls[Up].push_back(deviceSource(d, SourceKind::JoyHat, 0, 1));
                b.controls[Right].push_back(deviceSource(d, SourceKind::JoyHat, 0, 2));
                b.controls[Down].push_back(deviceSource(d, SourceKind::JoyHat, 0, 4));
                b.controls[Left].push_back(deviceSource(d, SourceKind::JoyHat, 0, 8));
            }
            const int order[] = {Cross, Circle, Square, Triangle, L1, R1, L2, R2, Select, Start, L3, R3};
            for (int i = 0; i < 12 && i < buttons; ++i)
                b.controls[order[i]].push_back(deviceSource(d, SourceKind::JoyButton, i));
        }

        // Devices that may become a player on their own: mapped gamepads, and raw joysticks (wheels, sticks) with
        // at least 2 axes and 4 buttons; RGB/vendor HID interfaces that also show up as joysticks do not.
        bool autoAssignable(const Device &d)
        {
            return d.gamepad || (d.axes.size() >= 2 && d.buttons.size() >= 4);
        }

        // Default order: mapped gamepads first (as the old backend picked the first gamepad), then other devices.
        std::vector<Device> defaultOrder(const World &world)
        {
            std::vector<Device> result;
            for (int pass = 0; pass < 2; ++pass)
            {
                for (const Polled &p : world.joys)
                {
                    if (p.dev.gamepad == (pass == 0) && autoAssignable(p.dev))
                        result.push_back(p.dev);
                }
            }
            return result;
        }

        std::array<Bindings, kPorts> buildDefaults(const World &world)
        {
            std::array<Bindings, kPorts> result;
            addKeyboardDefaults(result[0]);
            const std::vector<Device> order = defaultOrder(world);
            for (int port = 0; port < kPorts && port < static_cast<int>(order.size()); ++port)
                addDeviceDefaults(result[port], order[port]);
            return result;
        }

        // Every source that is active right now; mapped pads report gamepad sources only.
        std::vector<Source> activeSources(const World &world)
        {
            std::vector<Source> result;
            for (int key = 1; key < kKeyCount; ++key)
            {
                if (world.keys[key])
                    result.push_back(keySource(key));
            }
            for (const Polled &p : world.joys)
            {
                const Device &d = p.dev;
                if (d.gamepad)
                {
                    for (int code = 1; code < kButtonCodes; ++code)
                    {
                        if (kButtonToGlfw[code] >= 0 && gamepadButtonValue(p, code) > kPressThreshold)
                            result.push_back(deviceSource(d, SourceKind::GamepadButton, code));
                    }
                    for (int code = 0; code < 6; ++code)
                    {
                        for (int sign : {-1, 1})
                        {
                            if (gamepadAxisValue(p, code, sign) > kPressThreshold)
                                result.push_back(deviceSource(d, SourceKind::GamepadAxis, code, sign));
                        }
                    }
                    continue;
                }
                for (int i = 0; i < static_cast<int>(d.buttons.size()); ++i)
                {
                    if (d.buttons[i])
                        result.push_back(deviceSource(d, SourceKind::JoyButton, i));
                }
                for (int i = 0; i < static_cast<int>(d.axes.size()); ++i)
                {
                    for (int sign : {-1, 1})
                    {
                        if (joyAxisDelta(p, i, sign) > kPressThreshold)
                            result.push_back(deviceSource(d, SourceKind::JoyAxis, i, sign));
                    }
                }
                for (int i = 0; i < static_cast<int>(d.hats.size()); ++i)
                {
                    for (int bit : {1, 2, 4, 8})
                    {
                        if (d.hats[i] & bit)
                            result.push_back(deviceSource(d, SourceKind::JoyHat, i, bit));
                    }
                }
            }
            return result;
        }

        const char *keyName(int key)
        {
            static char buffer[16];
            if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_ZERO && key <= KEY_NINE))
            {
                buffer[0] = static_cast<char>(key);
                buffer[1] = 0;
                return buffer;
            }
            if (key >= KEY_F1 && key <= KEY_F12)
            {
                std::snprintf(buffer, sizeof(buffer), "F%d", key - KEY_F1 + 1);
                return buffer;
            }
            if (key >= KEY_KP_0 && key <= KEY_KP_9)
            {
                std::snprintf(buffer, sizeof(buffer), "Keypad %d", key - KEY_KP_0);
                return buffer;
            }
            switch (key)
            {
            case KEY_SPACE: return "Space";
            case KEY_APOSTROPHE: return "'";
            case KEY_COMMA: return ",";
            case KEY_MINUS: return "-";
            case KEY_PERIOD: return ".";
            case KEY_SLASH: return "/";
            case KEY_SEMICOLON: return ";";
            case KEY_EQUAL: return "=";
            case KEY_LEFT_BRACKET: return "[";
            case KEY_BACKSLASH: return "\\";
            case KEY_RIGHT_BRACKET: return "]";
            case KEY_GRAVE: return "`";
            case KEY_ESCAPE: return "Escape";
            case KEY_ENTER: return "Enter";
            case KEY_TAB: return "Tab";
            case KEY_BACKSPACE: return "Backspace";
            case KEY_INSERT: return "Insert";
            case KEY_DELETE: return "Delete";
            case KEY_RIGHT: return "Right";
            case KEY_LEFT: return "Left";
            case KEY_DOWN: return "Down";
            case KEY_UP: return "Up";
            case KEY_PAGE_UP: return "Page Up";
            case KEY_PAGE_DOWN: return "Page Down";
            case KEY_HOME: return "Home";
            case KEY_END: return "End";
            case KEY_CAPS_LOCK: return "Caps Lock";
            case KEY_SCROLL_LOCK: return "Scroll Lock";
            case KEY_NUM_LOCK: return "Num Lock";
            case KEY_PRINT_SCREEN: return "Print Screen";
            case KEY_PAUSE: return "Pause";
            case KEY_KP_DECIMAL: return "Keypad .";
            case KEY_KP_DIVIDE: return "Keypad /";
            case KEY_KP_MULTIPLY: return "Keypad *";
            case KEY_KP_SUBTRACT: return "Keypad -";
            case KEY_KP_ADD: return "Keypad +";
            case KEY_KP_ENTER: return "Keypad Enter";
            case KEY_KP_EQUAL: return "Keypad =";
            case KEY_LEFT_SHIFT: return "Left Shift";
            case KEY_LEFT_CONTROL: return "Left Ctrl";
            case KEY_LEFT_ALT: return "Left Alt";
            case KEY_LEFT_SUPER: return "Left Super";
            case KEY_RIGHT_SHIFT: return "Right Shift";
            case KEY_RIGHT_CONTROL: return "Right Ctrl";
            case KEY_RIGHT_ALT: return "Right Alt";
            case KEY_RIGHT_SUPER: return "Right Super";
            case KEY_KB_MENU: return "Menu";
            default:
                std::snprintf(buffer, sizeof(buffer), "%d", key);
                return buffer;
            }
        }

        const char *const kGamepadButtonNames[] = {
            "?", "D-pad up", "D-pad right", "D-pad down", "D-pad left", "Y", "B", "A", "X",
            "LB", "LT", "RB", "RT", "Back", "Guide", "Start", "LS", "RS"};
        const char *const kGamepadAxisNames[] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "LT", "RT"};

        int controlFromKey(const std::string &key)
        {
            for (int c = 0; c < ControlCount; ++c)
            {
                if (key == kControlKeys[c])
                    return c;
            }
            return -1;
        }

        int kindFromName(const std::string &name)
        {
            for (int k = 1; k < static_cast<int>(sizeof(kKindNames) / sizeof(kKindNames[0])); ++k)
            {
                if (name == kKindNames[k])
                    return k;
            }
            return -1;
        }
    }

    const char *controlName(int control)
    {
        return control >= 0 && control < ControlCount ? kControlNames[control] : "?";
    }

    const char *controlKey(int control)
    {
        return control >= 0 && control < ControlCount ? kControlKeys[control] : "?";
    }

    std::string describe(const Source &source)
    {
        if (source.kind == SourceKind::None)
            return "None";
        if (source.kind == SourceKind::Key)
            return std::string("Key ") + keyName(source.code);

        std::string device = "Unplugged device";
        for (const Device &d : Input::get().devices())
        {
            if (d.jid >= 0 && d.guid == source.guid && d.instance == source.instance)
            {
                device = d.name;
                break;
            }
        }
        if (source.instance > 0)
            device += " #" + std::to_string(source.instance + 1);

        const char *sign = source.sign < 0 ? "-" : "+";
        switch (source.kind)
        {
        case SourceKind::GamepadButton:
            return device + ": " + (source.code >= 0 && source.code < kButtonCodes ? kGamepadButtonNames[source.code] : "?");
        case SourceKind::GamepadAxis:
            if (source.code == GAMEPAD_AXIS_LEFT_TRIGGER || source.code == GAMEPAD_AXIS_RIGHT_TRIGGER)
                return device + ": " + kGamepadAxisNames[source.code];
            return device + ": " + (source.code >= 0 && source.code < 6 ? kGamepadAxisNames[source.code] : "?") + sign;
        case SourceKind::JoyButton:
            return device + ": button " + std::to_string(source.code);
        case SourceKind::JoyAxis:
            return device + ": axis " + std::to_string(source.code) + sign;
        case SourceKind::JoyHat:
        {
            const char *dir = source.sign == 1 ? "up" : source.sign == 2 ? "right" : source.sign == 4 ? "down" : "left";
            return device + ": hat " + std::to_string(source.code) + " " + dir;
        }
        default:
            return device;
        }
    }

    struct Input::State
    {
        bool loaded = false;
        bool fileExists = false;  // once it exists, the file is the only source of bindings
        bool defaultsBuilt = false;
        std::set<std::string> known; // devices (guid#instance) present at the last refresh
        std::string path;
        std::array<Bindings, kPorts> bindings;
        World world;
        std::map<std::string, std::vector<AxisRest>> rest;
        // Host thread only (refreshDevices, outside m_mutex): jid -> (guid, instance), kept while the jid stays
        // present with the same GUID so a device's instance does not change when another one comes or goes.
        std::map<int, std::pair<std::string, int>> sticky;
        std::array<PortSnapshot, kPorts> snapshot;

        bool capturing = false;
        std::vector<Source> baseline;
        std::set<std::string> baselineDevices;

        void load();
    };

    namespace
    {
        // Writes the bindings file; call without m_mutex held.
        void writeBindings(const std::array<Bindings, kPorts> &bindings, const std::string &path)
        {
            static std::mutex s_saveMutex;
            std::lock_guard<std::mutex> lock(s_saveMutex);
            const std::string tmp = path + ".tmp";
            {
                std::ofstream file(tmp, std::ios::trunc);
                if (!file)
                {
                    std::cout << "[input] cannot write " << tmp << std::endl;
                    return;
                }
                file << "# moongate controls v1\n";
                file << "# <port> <control> <kind> <guid|-> <instance> <code> <sign>\n";
                for (int port = 0; port < kPorts; ++port)
                {
                    for (int c = 0; c < ControlCount; ++c)
                    {
                        for (const Source &s : bindings[port].controls[c])
                        {
                            if (s.kind == SourceKind::None)
                                continue;
                            file << port << ' ' << kControlKeys[c] << ' ' << kKindNames[static_cast<int>(s.kind)] << ' '
                                 << (s.guid.empty() ? "-" : s.guid) << ' ' << s.instance << ' ' << s.code << ' ' << s.sign << '\n';
                        }
                    }
                }
                if (!file.flush())
                {
                    std::cout << "[input] cannot write " << tmp << std::endl;
                    return;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                std::filesystem::rename(tmp, path, ec);
                if (ec)
                    std::cout << "[input] cannot replace " << path << std::endl;
            }
        }
    }

    void Input::State::load()
    {
        const char *env = std::getenv("PS2X_CONTROLS");
        path = env && *env ? env : "controls.ini";
        std::ifstream file(path);
        if (!file)
        {
            std::cout << "[input] no " << path << "; using default controls" << std::endl;
            return;
        }
        fileExists = true;
        std::string line;
        int count = 0;
        while (std::getline(file, line))
        {
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            std::istringstream in(line);
            int port = -1, instance = 0, code = 0, sign = 0;
            std::string control, kind, guid;
            if (!(in >> port >> control >> kind >> guid >> instance >> code >> sign))
                continue;
            const int c = controlFromKey(control);
            const int k = kindFromName(kind);
            if (port < 0 || port >= kPorts || c < 0 || k < 0)
                continue;
            Source s;
            s.kind = static_cast<SourceKind>(k);
            s.guid = guid == "-" ? "" : guid;
            s.instance = instance;
            s.code = code;
            s.sign = sign;
            bindings[port].controls[c].push_back(s);
            ++count;
        }
        std::cout << "[input] loaded " << count << " bindings from " << path << std::endl;
    }

    Input &Input::get()
    {
        // Never destroyed: the EE thread may still read it during shutdown.
        static Input *s_input = []()
        {
            Input *input = new Input();
            input->m_state = new State();
            return input;
        }();
        return *s_input;
    }

    Input::State &Input::state() const
    {
        return *m_state;
    }

    void Input::refreshDevices()
    {
        World world;
#if !defined(PLATFORM_VITA)
        // Sticky instances: a jid keeps its instance while it stays present with the same GUID; a new jid gets the
        // smallest instance not used by a present device with that GUID.
        std::map<int, std::string> presentGuids;
        for (int jid = 0; jid < kMaxJoysticks; ++jid)
        {
            if (!glfwJoystickPresent(jid))
                continue;
            const char *guid = glfwGetJoystickGUID(jid);
            presentGuids[jid] = guid ? guid : "";
        }
        auto &sticky = state().sticky;
        std::erase_if(sticky, [&presentGuids](const auto &entry)
                      {
                          const auto it = presentGuids.find(entry.first);
                          return it == presentGuids.end() || it->second != entry.second.first;
                      });
        for (const auto &[jid, guid] : presentGuids)
        {
            if (sticky.count(jid))
                continue;
            std::set<int> used;
            for (const auto &entry : sticky)
            {
                if (entry.second.first == guid)
                    used.insert(entry.second.second);
            }
            int instance = 0;
            while (used.count(instance))
                ++instance;
            sticky[jid] = {guid, instance};
        }

        for (const auto &[jid, guid] : presentGuids)
        {
            Polled p;
            p.dev.jid = jid;
            const char *name = glfwGetJoystickName(jid);
            p.dev.guid = guid;
            p.dev.name = name ? name : "?";
            p.dev.instance = sticky[jid].second;
            p.dev.gamepad = glfwJoystickIsGamepad(jid) != 0;
            int count = 0;
            if (const float *axes = glfwGetJoystickAxes(jid, &count))
                p.dev.axes.assign(axes, axes + count);
            int hatCount = 0;
            if (const unsigned char *hats = glfwGetJoystickHats(jid, &hatCount))
                p.dev.hats.assign(hats, hats + hatCount);
            // GLFW_JOYSTICK_HAT_BUTTONS (on by default) appends 4 buttons per hat; keep the real buttons only.
            if (const unsigned char *buttons = glfwGetJoystickButtons(jid, &count))
                p.dev.buttons.assign(buttons, buttons + std::max(0, count - 4 * static_cast<int>(p.dev.hats.size())));
            GLFWgamepadstate gp{};
            if (p.dev.gamepad && glfwGetGamepadState(jid, &gp))
            {
                std::copy(std::begin(gp.buttons), std::end(gp.buttons), p.gpButtons.begin());
                std::copy(std::begin(gp.axes), std::end(gp.axes), p.gpAxes.begin());
            }
            else
            {
                p.gpAxes[kGlfwLeftTrigger] = p.gpAxes[kGlfwRightTrigger] = -1.0f;
            }
            world.joys.push_back(std::move(p));
        }
#else
        // Vita: raylib's gamepad 0 is the built-in pad.
        if (IsGamepadAvailable(0))
        {
            Polled p;
            p.dev.jid = 0;
            p.dev.guid = "vita";
            const char *name = GetGamepadName(0);
            p.dev.name = name ? name : "Vita";
            p.dev.gamepad = true;
            for (int code = 1; code < kButtonCodes; ++code)
            {
                if (kButtonToGlfw[code] >= 0)
                    p.gpButtons[kButtonToGlfw[code]] = IsGamepadButtonDown(0, code) ? 1 : 0;
            }
            for (int axis = 0; axis < 4; ++axis)
                p.gpAxes[axis] = GetGamepadAxisMovement(0, axis);
            // L2/R2 as buttons, as the old backend read them.
            p.gpAxes[kGlfwLeftTrigger] = IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_TRIGGER_2) ? 1.0f : -1.0f;
            p.gpAxes[kGlfwRightTrigger] = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_2) ? 1.0f : -1.0f;
            world.joys.push_back(std::move(p));
        }
#endif
        for (int key = 1; key < kKeyCount; ++key)
            world.keys[key] = IsKeyDown(key);

        std::lock_guard<std::mutex> lock(m_mutex);
        State &s = state();
        if (!s.loaded)
        {
            s.loaded = true;
            s.load();
        }

        // Raw axis rest values, learned per axis once the value is stable (see updateRest).
        std::set<std::string> present;
        for (Polled &p : world.joys)
        {
            const std::string key = deviceKey(p.dev.guid, p.dev.instance);
            present.insert(key);
            std::vector<AxisRest> &rest = s.rest[key];
            rest.resize(p.dev.axes.size());
            p.rest.clear();
            for (size_t a = 0; a < p.dev.axes.size(); ++a)
                p.rest.push_back(updateRest(rest[a], p.dev.axes[a]));
        }
        std::erase_if(s.rest, [&present](const auto &entry) { return present.count(entry.first) == 0; });

        // Without a file, the first defaults follow the connected devices; afterwards a newly appeared device that
        // is not bound anywhere gets its default layout on a port with no present joystick bound (port 0 first), so a pad
        // plugged in later still gets bound. Present devices are never moved between ports.
        if (!s.fileExists)
        {
            if (!s.defaultsBuilt)
            {
                s.defaultsBuilt = true;
                s.bindings = buildDefaults(world);
            }
            else
            {
                for (const Device &d : defaultOrder(world))
                {
                    if (s.known.count(deviceKey(d.guid, d.instance)))
                        continue;
                    bool bound = false;
                    for (const Bindings &b : s.bindings)
                    {
                        for (const auto &list : b.controls)
                        {
                            for (const Source &src : list)
                                bound = bound || sameDevice(src, d);
                        }
                    }
                    if (bound)
                        continue;
                    for (int port = 0; port < kPorts; ++port)
                    {
                        // Only joystick bindings of a present device take the port.
                        bool hasJoystick = false;
                        for (const auto &list : s.bindings[port].controls)
                        {
                            for (const Source &src : list)
                                hasJoystick = hasJoystick || (src.kind != SourceKind::None && src.kind != SourceKind::Key &&
                                                              findJoy(world, src.guid, src.instance) != nullptr);
                        }
                        if (!hasJoystick)
                        {
                            addDeviceDefaults(s.bindings[port], d);
                            break;
                        }
                    }
                }
            }
        }
        s.known = std::move(present);

        s.world = std::move(world);
        for (int port = 0; port < kPorts; ++port)
            s.snapshot[port] = evaluate(s.world, s.bindings[port]);
    }

    bool Input::readPort(int port, uint16_t &buttons, uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry) const
    {
        PortSnapshot snap;
        if (port >= 0 && port < kPorts)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snap = state().snapshot[port];
        }
        buttons = snap.buttons;
        lx = snap.lx;
        ly = snap.ly;
        rx = snap.rx;
        ry = snap.ry;
        return port == 0 || snap.connected;
    }

    bool Input::portConnected(int port) const
    {
        if (port == 0)
            return true;
        if (port < 0 || port >= kPorts)
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        return state().snapshot[port].connected;
    }

    std::vector<Device> Input::devices() const
    {
        std::vector<Device> result;
        Device keyboard;
        keyboard.name = "Keyboard";
        result.push_back(keyboard);
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Polled &p : state().world.joys)
            result.push_back(p.dev);
        return result;
    }

    Bindings Input::bindings(int port) const
    {
        if (port < 0 || port >= kPorts)
            return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        return state().bindings[port];
    }

    void Input::setBindings(int port, const Bindings &b)
    {
        if (port < 0 || port >= kPorts)
            return;
        std::array<Bindings, kPorts> bindings;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            State &s = state();
            s.bindings[port] = b;
            s.fileExists = true; // the user's edit counts even if the write fails
            bindings = s.bindings;
            path = s.path;
        }
        writeBindings(bindings, path);
    }

    void Input::resetDefaults()
    {
        std::array<Bindings, kPorts> bindings;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            State &s = state();
            s.bindings = buildDefaults(s.world);
            s.fileExists = true;
            bindings = s.bindings;
            path = s.path;
        }
        writeBindings(bindings, path);
    }

    void Input::applyDeviceDefaults(int port, const Device &device)
    {
        if (port < 0 || port >= kPorts)
            return;
        std::array<Bindings, kPorts> bindings;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            State &s = state();
            Bindings &b = s.bindings[port];
            for (auto &list : b.controls)
                std::erase_if(list, [&device](const Source &src) { return sameDevice(src, device); });
            addDeviceDefaults(b, device);
            s.fileExists = true;
            bindings = s.bindings;
            path = s.path;
        }
        writeBindings(bindings, path);
    }

    void Input::applyTwinStick(const Device &a, const Device &b)
    {
        Bindings result;
        // One lever: its stick, its trigger button and its second button.
        auto lever = [&result](const Device &d, int stickLeft, int first, int second)
        {
            const int dirs[4][2] = {{0, -1}, {0, 1}, {1, -1}, {1, 1}}; // left, right, up, down
            for (int i = 0; i < 4; ++i)
            {
                if (d.gamepad)
                    result.controls[stickLeft + i].push_back(deviceSource(d, SourceKind::GamepadAxis, dirs[i][0], dirs[i][1]));
                else if (dirs[i][0] < static_cast<int>(d.axes.size()))
                    result.controls[stickLeft + i].push_back(deviceSource(d, SourceKind::JoyAxis, dirs[i][0], dirs[i][1]));
            }
            if (d.gamepad)
            {
                result.controls[first].push_back(deviceSource(d, SourceKind::GamepadButton, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
                result.controls[second].push_back(deviceSource(d, SourceKind::GamepadButton, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
                result.controls[Start].push_back(deviceSource(d, SourceKind::GamepadButton, GAMEPAD_BUTTON_MIDDLE_RIGHT));
                return;
            }
            const int buttons = static_cast<int>(d.buttons.size());
            if (buttons > 0)
                result.controls[first].push_back(deviceSource(d, SourceKind::JoyButton, 0));
            if (buttons > 1)
                result.controls[second].push_back(deviceSource(d, SourceKind::JoyButton, 1));
            // Start: the default layout's start button, else the last button past the two lever buttons.
            if (buttons > 9)
                result.controls[Start].push_back(deviceSource(d, SourceKind::JoyButton, 9));
            else if (buttons > 2)
                result.controls[Start].push_back(deviceSource(d, SourceKind::JoyButton, buttons - 1));
        };
        lever(a, LeftStickLeft, L1, L2);
        lever(b, RightStickLeft, R1, R2);

        std::array<Bindings, kPorts> bindings;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            State &s = state();
            s.bindings[0] = result;
            s.fileExists = true;
            bindings = s.bindings;
            path = s.path;
        }
        writeBindings(bindings, path);
    }

    void Input::beginCapture()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        State &s = state();
        s.capturing = true;
        s.baseline = activeSources(s.world);
        s.baselineDevices.clear();
        for (const Polled &p : s.world.joys)
            s.baselineDevices.insert(deviceKey(p.dev.guid, p.dev.instance));
    }

    bool Input::pollCapture(Source &out, bool &cancelled)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        State &s = state();
        if (!s.capturing)
            return false;
        const std::vector<Source> active = activeSources(s.world);

        // A device plugged in during capture: whatever it reports at first sight is its rest state.
        for (const Polled &p : s.world.joys)
        {
            const std::string key = deviceKey(p.dev.guid, p.dev.instance);
            if (s.baselineDevices.insert(key).second)
            {
                for (const Source &src : active)
                {
                    if (sameDevice(src, p.dev))
                        s.baseline.push_back(src);
                }
            }
        }
        // Inputs held at the start count once they have been released.
        std::erase_if(s.baseline, [&active](const Source &src)
                      { return std::find(active.begin(), active.end(), src) == active.end(); });

        for (const Source &src : active)
        {
            if (std::find(s.baseline.begin(), s.baseline.end(), src) != s.baseline.end())
                continue;
            s.capturing = false;
            if (src.kind == SourceKind::Key && src.code == KEY_ESCAPE)
            {
                out = Source{};
                cancelled = true;
                return true;
            }
            out = src;
            cancelled = false;
            return true;
        }
        return false;
    }

    bool Input::capturing() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return state().capturing;
    }

    std::string Input::path() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const State &s = *m_state;
        if (!s.path.empty())
            return s.path;
        const char *env = std::getenv("PS2X_CONTROLS");
        return env && *env ? env : "controls.ini";
    }
}
