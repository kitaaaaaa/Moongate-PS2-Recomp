#ifndef PS2_INPUT_H
#define PS2_INPUT_H

// Host input for the emulated PS2 pads: any keyboard, gamepad, wheel or joystick can drive either PS2 pad port.
// Each PS2 control of each port (16 buttons, the four directions of both analog sticks) has a list of bindings
// to host inputs; the port reads the OR of its button bindings and the strongest of its axis bindings.
// Bindings are kept in a text file (PS2X_CONTROLS, default "controls.ini" in the working directory).
//
// Threads: refreshDevices(), capture and everything that edits bindings run on the host (window) thread; readPort()
// runs on the EE thread and only reads a snapshot published by refreshDevices() (no GLFW or raylib calls).

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ps2input
{
    constexpr int kPorts = 2;

    // PS2 controls, in the order of the digital button bits of the pad's data[2..3] (bit = 1 << index), then
    // the stick directions.
    enum Control : int
    {
        Select = 0, L3, R3, Start, Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square,
        LeftStickLeft, LeftStickRight, LeftStickUp, LeftStickDown,
        RightStickLeft, RightStickRight, RightStickUp, RightStickDown,
        ControlCount
    };
    const char *controlName(int control); // "Cross", "Left stick up", ...
    const char *controlKey(int control);   // file key: "cross", "lstick_up", ...

    // A host input. Devices are named by kind + GUID + the index among connected devices with that GUID, so two
    // identical pads stay apart and a binding follows its device when the GLFW joystick id changes.
    enum class SourceKind : uint8_t
    {
        None,
        Key,           // code = raylib KeyboardKey
        GamepadButton, // code = raylib GamepadButton; device has a gamepad mapping
        GamepadAxis,   // code = raylib GamepadAxis, sign = -1 or +1 (direction that counts as pressed)
        JoyButton,     // code = raw GLFW button index
        JoyAxis,       // code = raw GLFW axis index, sign = -1 or +1
        JoyHat,        // code = hat index, sign = GLFW hat bit (1 up, 2 right, 4 down, 8 left)
    };

    struct Source
    {
        SourceKind kind = SourceKind::None;
        std::string guid; // empty for Key
        int instance = 0; // n-th connected device with this GUID
        int code = 0;
        int sign = 0;
        bool operator==(const Source &) const = default;
    };
    std::string describe(const Source &source); // "Key X", "Xbox Controller: A", "T300RS: axis 0+", ...

    // One host device as seen by the last refreshDevices().
    struct Device
    {
        int jid = -1;            // GLFW joystick id; -1 for the keyboard
        std::string guid;        // empty for the keyboard
        int instance = 0;
        std::string name;        // "Keyboard", or the GLFW name
        bool gamepad = false;    // GLFW has a gamepad mapping for it
        std::vector<float> axes; // raw values -1..1
        std::vector<uint8_t> buttons;
        std::vector<uint8_t> hats;
    };

    struct Bindings
    {
        std::array<std::vector<Source>, ControlCount> controls;
    };

    // The whole input state. One instance: get().
    class Input
    {
    public:
        static Input &get();

        // Host thread, once per presented frame (PSPadBackend::updateGamepads): loads the bindings file on first
        // use, registers extra gamepad mappings, polls all devices, and publishes the per-port pad state.
        void refreshDevices();

        // EE thread: the port's pad state in the PS2 layout. buttons: active-low bits (bit = 1 << Control);
        // sticks: 0x00..0xFF, 0x80 centred. Returns false when nothing is bound to the port or none of its bound
        // devices is connected (the port then reads as unplugged); port 0 always returns true.
        bool readPort(int port, uint16_t &buttons, uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry) const;
        bool portConnected(int port) const;

        // Host thread: editing (the controls window).
        std::vector<Device> devices() const;      // keyboard first, then joysticks by jid
        Bindings bindings(int port) const;
        void setBindings(int port, const Bindings &b); // also saves the file
        void resetDefaults();                      // default layout (below), saved

        // Presets. Defaults: port 0 = keyboard + the first connected device; port 1 = the second connected device.
        // A gamepad gets the usual layout (face buttons, shoulders, triggers, sticks, d-pad, start/back); any other
        // device gets axis 0/1 -> left stick, axis 2/3 -> right stick, hat 0 -> d-pad, buttons 0..11 -> Cross,
        // Circle, Square, Triangle, L1, R1, L2, R2, Select, Start, L3, R3.
        void applyDeviceDefaults(int port, const Device &device);
        // Twin-stick: port 0's left lever from device a (its left stick or axes 0/1 -> left stick, its trigger
        // button -> L1, second button -> L2), right lever from device b (-> right stick, R1, R2), Start from
        // either; replaces port 0's bindings and saves.
        void applyTwinStick(const Device &a, const Device &b);

        // Press-to-bind: the next host input that goes from rest to active (key down, button down, axis past 0.5
        // from its rest value, hat direction) is returned once; capture is cancelled by Escape (returns a Source of
        // kind None with cancelled = true).
        void beginCapture();
        bool pollCapture(Source &out, bool &cancelled); // host thread; true when finished
        bool capturing() const;

        std::string path() const; // the bindings file

    private:
        Input() = default;
        mutable std::mutex m_mutex;
        // ... implementation state (snapshots, bindings, capture baseline) lives in ps2_input.cpp
        struct State;
        State *m_state = nullptr;
        State &state() const;
    };
}

#endif
