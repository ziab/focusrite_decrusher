# Project: Focusrite De-Crusher

## 1. Background & Context

Many audio professionals and hobbyists (specifically e-kit drummers) leave their workstations and ASIO hosts running 24/7. However, a common issue arises with **Focusrite Scarlett** interfaces (and potentially others) during Windows power state transitions.

* **The Conflict:** When the Windows display turns off or enters sleep, a Power State change (D3) is broadcast over the PCIe bus. This frequently triggers a DPC latency spike that desynchronizes the Focusrite hardware clock from the active ASIO stream.
* **The Symptoms:** Audio becomes "robotic," "decimated," or "bit-crushed." This affects system-wide audio and, critically, low-latency ASIO outputs, persisting until a manual hardware reset occurs.
* **The Manual Fix:** Toggling the sample rate (e.g., 44.1kHz <-> 48kHz) in the Focusrite Control software forces a hardware PLL re-lock and resets the DMA pointers, immediately restoring clean audio.
* **The Objective:** Automate this "sample rate flip" seamlessly in the background when monitor power-down events are detected, ensuring the audio rig remains perfectly synchronized and ready to play at all times.

---

## 2. Technical Architecture & The "Strategy C" Solution (Focusrite Control 2)

Initial attempts to automate this by manipulating the Windows Core Audio `PKEY_AudioEngine_DeviceFormat` property failed. Because the ASIO host holds an exclusive hardware lock, the Focusrite driver ignores WDM-level sample rate changes.

**Focusrite Control 1** used a local `ControlServer.exe` TCP/XML protocol (`<client-details>` / `<set id="905">` sample-rate flip on `127.0.0.1` ephemeral port discovered via `GetExtendedTcpTable`). **Focusrite Control 2** removed that server; the desktop app now exposes the **phone-app channel**: Bonjour `_ocaws._tcp.local` advertising an onboarding + secure WebSocket (`AES70/OCP.1` over `X25519` + `XChaCha20-Poly1305` `secretstream`, `sharedRx` per `v1.1081`, see `alexanderdalesio/focusrite-control-2-api` `docs/PROTOCOL.md`).

This build is **V2-only** (no Control 1 fallback). It reuses the tested sidecar at `http://127.0.0.1:41780` instead of reimplementing the crypto in C++.

### Core Components
1.  **Monitor State Detection:** 
    * A lightweight Win32 Tray Application uses `RegisterPowerSettingNotification` with `GUID_MONITOR_POWER_ON`.
    * It processes `WM_POWERBROADCAST` to detect when the monitor turns off.
    * Implements a 5-second sleep/timer after the "Off" event to allow the GPU and PCIe bus to settle into their idle power states before executing the audio reset.
2.  **Focusrite Control 2 Discovery & Pairing (phone-app):**
    * FC2 advertises `_ocaws._tcp.local` via Bonjour/DNS-SD (`dns-sd -B _ocaws._tcp local.`, `dns-sd -L <instance> _ocaws._tcp local.`). TXT `serverPublicKey` (64-hex) + dynamic `securePort`/`onboardingPort` (defaults `58322`/`58323` but ephemeral).
    * Sidecar handles `X25519` identity (`%APPDATA%\focusrite-control-2-api\client-key.json`), `crypto_box_easy` approval payload, and QR verification (`focusrite pair` - approve in FC2 UI, screenshot QR). See `pair.js`/`secure-transport.js`.
3.  **Re-clock via Sidecar HTTP API:**
    * DeCrusher `src/FC2HttpClient.cpp` uses `WinHTTP` `POST http://127.0.0.1:41780/api/v1/batch` `{"operations":[{"control":"dim","value":true},{"control":"dim","value":false}]}` (`monitor-mute`/`monitor-gain` also available - `controls.js` objectNumbers `4117`/`4116`/`4118`). Two AES70 writes force the control path and re-sync the hardware (FC2 does not expose sample-rate; dim/mute toggle is used as the V2 analogue of the old `905` flip). Verified via `GET /api/v1/health`.
    * Alternative for full `FCP` routing/mixer/sample-rate via direct USB (`focusrite backend usb`, requires FC2 **closed** - exclusive `libusb` vendor interface, `fcp-protocol.js` opcodes `0x80000c` device-map) is documented but not bundled in this V2 WinHTTP build.
4.  **Tray UI & Utilities:**
    * Standard `Shell_NotifyIcon` for background operation.
    * Provides a right-click context menu for "Manual De-Crush" and an option to enable verbose `-logs` via a spawned console window for troubleshooting.
    * Runs at `ABOVE_NORMAL_PRIORITY_CLASS` to prevent throttling during system idle transitions.

---

## 3. Usage & Setup (Focusrite Control 2)

This V2 build requires the FC2 sidecar (`alexanderdalesio/focusrite-control-2-api`):

1. Install Node.js 20+ and the sidecar: `npm install -g focusrite-control-2-api`
2. Pair once with the desktop app:
   ```bat
   focusrite backend fc2
   dns-sd -B _ocaws._tcp local.
   dns-sd -L <INSTANCE> _ocaws._tcp local.
   focusrite config set serverPublicKey <64-hex from TXT>
   focusrite pair   :: approve in Focusrite Control 2 when prompted, then drag QR PNG screenshot
   ```
3. Keep the sidecar running: `focusrite api` (listens `127.0.0.1:41780`, verify `curl http://127.0.0.1:41780/api/v1/health`)
4. Run `DeCrusher.exe` (tray icon, `-logs` for verbose). On monitor off (5s delay) or **Manual De-Crush** it `POST /api/v1/batch` `dim` toggle via `WinHTTP`; failures show balloon `NIIF_ERROR` and log.
   
   Legacy Control 1 `approval.png` / `ControlServer.exe` flow is removed in V2.
---

## 4. Building from Source

This project requires **CMake** and **Visual Studio / MSVC**.

1. Clone the repository.
2. Double-click the `build.bat` file.
3. The compiled standalone executable will be generated at `build\Release\DeCrusher.exe`.
