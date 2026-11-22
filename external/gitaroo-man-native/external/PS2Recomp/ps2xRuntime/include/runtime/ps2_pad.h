#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <string>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    // EE thread: the pad state of a port from ps2input::Input; false when the port reads as unplugged.
    bool readState(int port, int slot, uint8_t *data, size_t size);
    bool portConnected(int port) const;

    // Main thread only (GLFW joystick calls). Registers mappings for controllers GLFW does not know
    // and polls all input devices; call every frame so hot-plugged controllers work.
    void updateGamepads();

private:
    static constexpr int kMaxJoysticks = 16;
    bool m_userMappingsLoaded = false;
    std::string m_seenGuids[kMaxJoysticks];
};

#endif
