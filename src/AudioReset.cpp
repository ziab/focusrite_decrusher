#include "AudioReset.h"
#include "FC2HttpClient.h"
#include <thread>
#include <chrono>

// Focusrite Control 2 - V2 only.
// Legacy ControlServer.exe XML (v1) has been removed. See README for migration.
// V2 uses the Focusrite Control 2 local service via its phone-app channel:
// the unofficial sidecar at http://127.0.0.1:41780 (alexanderdalesio/focusrite-control-2-api)
// which speaks AES70/WebSocket to FC2 (Bonjour _ocaws._tcp, X25519 + XChaCha20-Poly1305).
// This avoids reimplementing secretstream/AES70 in C++ and reuses the tested Node sidecar.

bool ReclockFocusriteDevice() {
    Log("ReclockFocusriteDevice (FC2 V2): Starting...");

    // Primary: FC2 via sidecar HTTP API (phone-app method)
    // The sidecar must be running: `npx focusrite-control-2-api` or `focusrite api`
    // Pairing is one-time: `focusrite pair` approves in FC2 UI with QR.
    // See https://github.com/alexanderdalesio/focusrite-control-2-api
    if (ReclockViaFC2Http("127.0.0.1", 41780)) {
        Log("ReclockFocusriteDevice (FC2 V2): FC2 batch succeeded");
        return true;
    }

    Log("ReclockFocusriteDevice (FC2 V2): sidecar not available or batch failed");
    Log("Hint: install/configure FC2 sidecar:");
    Log("  npm install -g focusrite-control-2-api");
    Log("  focusrite backend fc2");
    Log("  dns-sd -B _ocaws._tcp local  # discover FC2, or use lsof -iTCP -sTCP:LISTEN");
    Log("  focusrite config set serverPublicKey <64-hex from TXT>");
    Log("  focusrite pair  # approve in Focusrite Control 2, then QR");
    Log("  focusrite api   # keep running on 127.0.0.1:41780");
    Log("Alternative: run FC2 with backend usb (FC2 closed) and use direct USB FCP - not yet in V2 build, see docs/PROTOCOL.md");
    return false;
}
