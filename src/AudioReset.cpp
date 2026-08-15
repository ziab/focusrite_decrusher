#include "AudioReset.h"
#include "LegacyControlServer.h"
#include <thread>
#include <chrono>

// Scarlett 6i6 Gen2 only - sample-rate flip via ControlServer, no driver restart (SD3 safe).

bool ReclockFocusriteDevice() {
    Log("ReclockFocusriteDevice (6i6 Gen2): Starting sample-rate flip 48<->44.1kHz...");

    if (ReclockViaLegacyControlServer()) {
        Log("ReclockFocusriteDevice: Legacy sample-rate flip succeeded (PLL re-locked, no driver restart)");
        return true;
    }

    Log("ReclockFocusriteDevice: ControlServer.exe not found");
    Log("Hint: run Focusrite Control (v1 for 6i6 Gen2, not Control 2), approve De-Crusher in Remote Devices");
    return false;
}
