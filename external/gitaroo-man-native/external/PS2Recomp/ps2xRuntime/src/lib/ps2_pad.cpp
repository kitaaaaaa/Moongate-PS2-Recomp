#include "runtime/ps2_pad.h"
#include "runtime/ps2_input.h"
#include "ps2_host_backend.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#if !defined(PLATFORM_VITA)
// raylib's desktop platform is built on the GLFW it bundles; these are not exposed by raylib.h.
extern "C"
{
    int glfwJoystickPresent(int jid);
    int glfwJoystickIsGamepad(int jid);
    const char *glfwGetJoystickGUID(int jid);
    const char *glfwGetJoystickName(int jid);
}
#endif

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;

#if !defined(PLATFORM_VITA)
    // GLFW's built-in database only lists some firmware versions of the Xbox Series controller over
    // Bluetooth (the version is part of the GUID). Newer firmware keeps the same layout, so any
    // Bluetooth 045e:0b13 controller without a mapping gets this one.
    constexpr const char *kXboxSeriesBtPrefix = "050000005e040000130b0000";
    constexpr const char *kXboxSeriesBtMapping =
        "Xbox Series Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,"
        "leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,"
        "righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Linux,";

    // SDL-format mappings from SDL_GAMECONTROLLERCONFIG and the file named by PS2X_GAMECONTROLLERDB
    // (default ./gamecontrollerdb.txt).
    void loadUserMappings()
    {
        if (const char *env = std::getenv("SDL_GAMECONTROLLERCONFIG"); env && *env)
            SetGamepadMappings(env);

        const char *path = std::getenv("PS2X_GAMECONTROLLERDB");
        std::ifstream file(path && *path ? path : "gamecontrollerdb.txt");
        if (!file)
            return;
        std::stringstream contents;
        contents << file.rdbuf();
        if (SetGamepadMappings(contents.str().c_str()))
            std::cout << "[pad] loaded controller mappings from " << (path && *path ? path : "gamecontrollerdb.txt") << std::endl;
    }
#endif
}

void PSPadBackend::updateGamepads()
{
#if !defined(PLATFORM_VITA)
    if (!m_userMappingsLoaded)
    {
        m_userMappingsLoaded = true;
        loadUserMappings();
    }

    for (int jid = 0; jid < kMaxJoysticks; ++jid)
    {
        if (!glfwJoystickPresent(jid))
        {
            m_seenGuids[jid].clear();
            continue;
        }

        const char *guidText = glfwGetJoystickGUID(jid);
        const std::string guid = guidText ? guidText : "";
        if (guid != m_seenGuids[jid])
        {
            m_seenGuids[jid] = guid;
            const char *nameText = glfwGetJoystickName(jid);
            const std::string name = nameText ? nameText : "?";
            if (!glfwJoystickIsGamepad(jid) && guid.rfind(kXboxSeriesBtPrefix, 0) == 0)
                SetGamepadMappings((guid + "," + kXboxSeriesBtMapping).c_str());

            if (glfwJoystickIsGamepad(jid))
                std::cout << "[pad] controller " << jid << ": " << name << " (" << guid << ")" << std::endl;
            else
                std::cout << "[pad] controller " << jid << ": " << name << " (" << guid
                          << ") has no gamepad mapping; add one via SDL_GAMECONTROLLERCONFIG or PS2X_GAMECONTROLLERDB"
                          << std::endl;
        }
    }
#endif
    ps2input::Input::get().refreshDevices();
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;

    uint16_t buttons = 0xFFFFu;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    const bool connected = ps2input::Input::get().readPort(port, buttons, lx, ly, rx, ry);
    data[2] = static_cast<uint8_t>(buttons & 0xFF);
    data[3] = static_cast<uint8_t>(buttons >> 8);
    data[4] = rx;
    data[5] = ry;
    data[6] = lx;
    data[7] = ly;
    return connected;
}

bool PSPadBackend::portConnected(int port) const
{
    return ps2input::Input::get().portConnected(port);
}
