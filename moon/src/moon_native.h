#pragma once

class PS2Runtime;

namespace moon_native
{
    // Replaces FUN_002093c0 with the native Model 2 polygon renderer (moon_native.cpp).
    void install(PS2Runtime &runtime);
}
