#include "AudioReset.h"
#include "PalReset.h"
#include "FcpReset.h"

// Scarlett 6i6 Gen2 - sample-rate flip via Driver PAL (FocusritePal64.dll) / Direct USB FCP (SD3 safe).

bool ReclockFocusriteDevice() {
    Log("ReclockFocusriteDevice: Starting sample-rate flip 48<->44.1kHz (no driver restart)...");

    if (ReclockViaFocusritePal()) {
        Log("ReclockFocusriteDevice: Driver PAL sample-rate flip succeeded (PLL re-locked)");
        return true;
    }

    Log("Driver PAL reset not available, falling back to direct USB FCP...");
    if (ReclockViaFcpSampleRate()) {
        Log("ReclockFocusriteDevice: FCP sample-rate flip succeeded (direct USB, PLL re-locked)");
        return true;
    }

    Log("ReclockFocusriteDevice: All sample-rate reset mechanisms failed.");
    return false;
}
