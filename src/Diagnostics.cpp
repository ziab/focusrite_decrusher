#include "Diagnostics.h"
#include "AudioReset.h"
#include <dbt.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <filesystem>
#include <string>

bool g_LoggingEnabled = false;
static bool diagnosticMode = false;
static bool consoleMode = false;
static FILE* logFile = nullptr;
static std::mutex logMutex;
static HDEVNOTIFY deviceNotification = nullptr;
static HPOWERNOTIFY powerNotifications[3] = {};
static std::filesystem::path logPath;

bool InitializeLogging(bool console, bool diagnostics) {
    consoleMode = console;
    diagnosticMode = diagnostics;
    if (console) {
        AllocConsole();
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        SetConsoleTitleW(L"De-Crusher Debug Logs");
    }
    if (diagnostics) {
        wchar_t local[MAX_PATH];
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        logPath = std::filesystem::path(local) / L"DeCrusher" / L"Diagnostics";
        std::error_code error;
        std::filesystem::create_directories(logPath, error);
        if (error) return false;
        logPath /= L"events.log";
        if (_wfopen_s(&logFile, logPath.c_str(), L"ab") != 0) return false;
        _fseeki64(logFile, 0, SEEK_END);
    }
    g_LoggingEnabled = console || diagnostics;
    Log("Logging started. PID=%lu diagnostics=%d; timestamps are local time; tick is milliseconds since boot", GetCurrentProcessId(), diagnostics);
    return true;
}

void Log(const char* format, ...) {
    if (!g_LoggingEnabled) return;
    std::lock_guard<std::mutex> lock(logMutex);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char body[8192];
    va_list args;
    va_start(args, format);
    vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    char line[8500];
    snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u tick=%llu tid=%lu] %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetTickCount64(), GetCurrentThreadId(), body);
    if (consoleMode) { fputs(line, stdout); fflush(stdout); }
    if (logFile) {
        // Two files of about 8 MiB each, with no polling or background writer.
        if (_ftelli64(logFile) >= 8 * 1024 * 1024) {
            fclose(logFile);
            logFile = nullptr;
            auto previous = logPath;
            previous += L".1";
            MoveFileExW(logPath.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
            _wfopen_s(&logFile, logPath.c_str(), L"ab");
            if (logFile) _fseeki64(logFile, 0, SEEK_END);
        }
        if (logFile) { fputs(line, logFile); fflush(logFile); }
    }
}

void StartDiagnostics(HWND window) {
    if (!diagnosticMode) return;
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    deviceNotification = RegisterDeviceNotificationW(window, &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
    if (!deviceNotification) Log("Device notification registration failed: %lu", GetLastError());
    else Log("Diagnostic device notifications registered");
    const GUID* settings[] = { &GUID_ACDC_POWER_SOURCE, &GUID_CONSOLE_DISPLAY_STATE, &GUID_POWER_SAVING_STATUS };
    for (int i = 0; i < 3; ++i) {
        powerNotifications[i] = RegisterPowerSettingNotification(window, settings[i], DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!powerNotifications[i]) Log("Diagnostic power registration %d failed: %lu", i, GetLastError());
    }
}

void LogDiagnosticMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (!diagnosticMode) return;
    if (message == WM_POWERBROADCAST) {
        Log("Power event: code=0x%llx", (unsigned long long)wParam);
        if (wParam == PBT_POWERSETTINGCHANGE && lParam) {
            auto setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
            wchar_t guid[40];
            StringFromGUID2(setting->PowerSetting, guid, 40);
            DWORD value = 0;
            if (setting->DataLength >= sizeof(value)) memcpy(&value, setting->Data, sizeof(value));
            Log("Power setting: guid=%ls bytes=%lu value=%lu", guid, setting->DataLength, value);
        }
    } else if (message == WM_DEVICECHANGE) {
        Log("Device event: code=0x%llx", (unsigned long long)wParam);
        if (lParam && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)) {
            auto header = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
            if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                auto device = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(lParam);
                Log("Device interface %s: %ls", wParam == DBT_DEVICEARRIVAL ? "arrived" : "removed", device->dbcc_name);
            }
        }
    }
}

void StopDiagnostics() {
    if (deviceNotification) UnregisterDeviceNotification(deviceNotification);
    deviceNotification = nullptr;
    for (auto& notification : powerNotifications) {
        if (notification) UnregisterPowerSettingNotification(notification);
        notification = nullptr;
    }
    // Detached reset workers can still log during process shutdown; keep the
    // synchronized file sink alive until the process closes its handles.
    Log("Diagnostic notifications stopped");
}
