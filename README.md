# Project: Focusrite De-Crusher

## 1. Background & Context

Many audio professionals and hobbyists (specifically e-kit drummers) leave their workstations and ASIO hosts running 24/7. However, a common issue arises with **Focusrite Scarlett** interfaces (and potentially others) during Windows power state transitions.

* **The Conflict:** When the Windows display turns off or enters sleep, a Power State change (D3) is broadcast over the PCIe bus. This frequently triggers a DPC latency spike that desynchronizes the Focusrite hardware clock from the active ASIO stream.
* **The Symptoms:** Audio becomes "robotic," "decimated," or "bit-crushed." This affects system-wide audio and, critically, low-latency ASIO outputs, persisting until a manual hardware reset occurs.
* **The Manual Fix:** Toggling the sample rate (e.g., 44.1kHz <-> 48kHz) in the Focusrite Control software forces a hardware PLL re-lock and resets the DMA pointers, immediately restoring clean audio.
* **The Objective:** Automate this "sample rate flip" seamlessly in the background when monitor power-down events are detected, ensuring the audio rig remains perfectly synchronized and ready to play at all times.

---

## 2. Technical Architecture - Driver PAL (Sample-Rate Only, No Driver Restart)

Initial attempts to automate this by manipulating the Windows Core Audio `PKEY_AudioEngine_DeviceFormat` property failed. Because the ASIO host holds an exclusive hardware lock, the Focusrite driver ignores WDM-level sample rate changes. `SetupDi` `DICS_DISABLE/ENABLE` device restart was tested but crashes `Superior Drummer 3` (tears `Focusrite USB Audio` stream `DevInst 9/54`).

The working fix uses Focusrite's official **Platform Abstraction Layer (`FocusritePal64.dll`)** to execute a driver-level sample-rate flip (`48kHz` $\rightarrow$ `2000ms PLL` $\rightarrow$ `44.1kHz`):

### Core Components
1. **Monitor State Detection:** 
   * A lightweight Win32 Tray Application uses `RegisterPowerSettingNotification` with `GUID_MONITOR_POWER_ON`.
   * Processes `WM_POWERBROADCAST` to detect when the monitor turns off (or on).
   * Implements a 5-second settle timer after the "Off" event to allow the GPU and PCIe bus to settle into their idle power states before executing the audio reset.
2. **Driver PAL Re-clocking (`src/PalReset.cpp`):**
   * Dynamically hooks `FocusritePal64.dll` (installed in `C:\Windows\System32` with the official Focusrite USB driver).
   * Communicates directly through the Focusrite kernel driver stack (`\Driver\FocusriteUsb`) via internal root IOCTLs.
   * Discovers connected Scarlett interfaces (`PID 0x8203` 6i6 Gen2 and others), reads current hardware clock, and triggers hardware PLL re-lock without restarting the audio driver or disrupting active ASIO sessions (SD3 safe).
   * Fully compatible with **Focusrite Control 2** and driver v4.150+.
3. **Direct USB FCP Fallback (`src/FcpReset.cpp`):**
   * Fallback implementation communicating via Focusrite Control Protocol (FCP) over USB Control Transfers for low-level direct access.
4. **Tray UI & Utilities (`src/main.cpp`):**
   * Standard `Shell_NotifyIcon` for background operation.
   * Right-click context menu for "Manual De-Crush" and Exit.
   * Spawned console window for verbose troubleshooting via `DeCrusher.exe -logs`.
   * Runs at `ABOVE_NORMAL_PRIORITY_CLASS` to prevent CPU throttling during idle transitions.

---

## 3. Usage & Setup

1. Run `build\Release\DeCrusher.exe` (or launch with `DeCrusher.exe -logs` for debug logging).
2. De-Crusher runs in the background system tray (no administrator privileges or UI approvals required).
3. When your monitor turns off, De-Crusher waits 5 seconds for PCIe power states to settle and flips the sample rate directly through the driver without interrupting active ASIO host streams.
4. You can also trigger an immediate reset anytime via right-click $\rightarrow$ **Manual De-Crush**.

---

## 4. Building from Source

This project requires **CMake** and **Visual Studio / MSVC**.

1. Clone the repository.
2. Run `build.bat` (or `cmake --build build --config Release`).
3. The compiled standalone executable is generated at `build\Release\DeCrusher.exe`.

## 5. Opt-in incident diagnostics

Exit the existing tray instance first, then launch `DeCrusher.exe -diagnostics`
(add `-logs` if you also want a console). Normal runs do not open log files or
register the extra device notifications. `-logs` alone remains console-only.
Diagnostics append to `%LOCALAPPDATA%\DeCrusher\Diagnostics\events.log`, rotating
at approximately 8 MiB into `events.log.1`. Lines include the local date/time,
milliseconds since boot and thread ID. Device interface paths and power messages
are recorded along with reset activity. These events do not trigger extra resets.
Monitor on and off both trigger the existing five-second delayed reset.

For deeper investigation, run these commands from **an elevated PowerShell** in
the project directory. WPR collection is separate from the app and starts only
when explicitly requested:

```powershell
.\trace.ps1 -Action Start
.\trace.ps1 -Action Status
# Immediately after hearing distortion:
.\trace.ps1 -Action Save
# Or discard the recording when no longer investigating:
.\trace.ps1 -Action Cancel
```

The named `DeCrusherDiagnostics` WPR instance records GeneralProfile, Audio and
Power in circular memory buffers. It captures recent performance activity for
analysis in Windows Performance Analyzer, including CPU/DPC/ISR and I/O evidence.
Retention depends on activity; save promptly, and note the time you first heard
the problem and any recovery actions. Saving stops recording, creates a dated
ETL, exports the previous hour of relevant Windows event logs, and copies the
app logs into the same diagnostics directory. Start again to record another
incident. The helper does not enable disabled Windows event-log channels.

Tracing has overhead while enabled and continues if the app exits; explicitly
Save or Cancel when finished. Saved incidents are not automatically deleted.
Trace files can contain process names, paths and device identifiers. A rate
readback or a completed reset is not proof that audio quality or PLL lock is good.

WPR memory-mode and named-instance behavior:
[Microsoft WPR command-line documentation](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options).
