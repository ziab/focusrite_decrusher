# Project: Focusrite De-Crusher

## 1. Background & Context

Many audio professionals and hobbyists leave their workstations and ASIO hosts running 24/7. However, a common issue arises with **Focusrite Scarlett** interfaces (and potentially others) during Windows power state transitions.

* **The Conflict:** When the Windows display turns off or enters sleep, a Power State change (D3) is broadcast over the PCIe bus. This frequently triggers a DPC latency spike that desynchronizes the Focusrite hardware clock from the active ASIO stream.
* **The Symptoms:** Audio becomes "robotic," "decimated," or "bit-crushed." This affects system-wide audio and, critically, low-latency ASIO outputs, persisting until a manual hardware reset occurs.
* **The Manual Fix:** Toggling the sample rate (e.g., 44.1kHz <-> 48kHz) in the Focusrite Control software forces a hardware PLL re-lock and resets the DMA pointers, immediately restoring clean audio.
* **The Objective:** Automate this "sample rate flip" seamlessly in the background when monitor power-down events are detected, ensuring the audio rig remains perfectly synchronized and ready to play at all times.

---

## 2. Technical Architecture & The "Strategy B" Solution

Initial attempts to automate this by manipulating the Windows Core Audio `PKEY_AudioEngine_DeviceFormat` property failed. Because the ASIO host holds an exclusive hardware lock, the Focusrite driver ignores WDM-level sample rate changes. 

The successful solution requires bypassing Windows Audio entirely and speaking directly to the hardware via the **Focusrite ControlServer**.

### Core Components
1.  **Monitor State Detection:** 
    * A lightweight Win32 Tray Application uses `RegisterPowerSettingNotification` with `GUID_MONITOR_POWER_ON`.
    * It processes `WM_POWERBROADCAST` to detect when the monitor turns off.
    * Implements a 5-second sleep/timer after the "Off" event to allow the GPU and PCIe bus to settle into their idle power states before executing the audio reset.
2.  **Dynamic Port Discovery:**
    * Focusrite's background service (`ControlServer.exe`) listens on a dynamic, ephemeral TCP port (often in the `49152+` range).
    * The application uses the Windows IP Helper API (`GetExtendedTcpTable`) and `CreateToolhelp32Snapshot` to dynamically locate the active PID and its corresponding listening port on `127.0.0.1`.
3.  **The TCP/XML Protocol:**
    * The app connects to the `ControlServer` via WinSock2.
    * It initiates a proprietary XML handshake (`<client-details>`), which requires a one-time manual approval within the Focusrite Control desktop UI.
    * Messages are framed using a hexadecimal length prefix (`Length=0000xx `).
    * It subscribes to the device (`<device-subscribe devid="1">`) and sends a `<set>` payload targeting Item ID `905` (the internal identifier for Sample Rate) to flip the rate to 48kHz, waits 2 seconds for a hard hardware lock, and flips it back to 44.1kHz.
4.  **Tray UI & Utilities:**
    * Standard `Shell_NotifyIcon` for background operation.
    * Provides a right-click context menu for "Manual De-Crush" and an option to enable verbose `-logs` via a spawned console window for troubleshooting.
    * Runs at `ABOVE_NORMAL_PRIORITY_CLASS` to prevent throttling during system idle transitions.

---

## 3. Usage & Setup

Because this application utilizes the internal ControlServer protocol, it contains a security authorization step:
1. Run `DeCrusher.exe`.
2. Open the official **Focusrite Control** desktop application.
3. Navigate to the settings and approve the new client named **"De-Crusher"**.
4. The application will now silently manage audio synchronization in the background.
---

## 4. Building from Source

This project requires **CMake** and **Visual Studio / MSVC**.

1. Clone the repository.
2. Double-click the `build.bat` file.
3. The compiled standalone executable will be generated at `build\Release\DeCrusher.exe`.
