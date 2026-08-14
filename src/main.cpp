#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <atomic>
#include <stdio.h>
#include <stdarg.h>
#include "resource.h"
#include "AudioReset.h"

bool g_LoggingEnabled = false;

void Log(const char* format, ...) {
    if (!g_LoggingEnabled) return;
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

#define WM_TRAYICON (WM_USER + 1)
#define WM_DECRUSH_RESULT (WM_APP + 1)
#define ID_TRAY_APP_ICON                1001
#define ID_TRAY_EXIT                    1002
#define ID_TRAY_MANUAL_DECRUSH          1003

NOTIFYICONDATA nid = {};
HWND hWnd;
HPOWERNOTIFY hPowerNotify = NULL;

void ShowTrayNotification(const wchar_t* title, const wchar_t* msg, DWORD dwInfoFlags) {
    nid.uFlags |= NIF_INFO;
    nid.dwInfoFlags = dwInfoFlags;
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, msg);
    // Use large icon version to ensure balloon shows on Win10/11
    Shell_NotifyIcon(NIM_MODIFY, &nid);
    Log("Notification shown: %ls - %ls", title, msg);
}

void AddTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(NOTIFYICONDATA));
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY_APP_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"De-Crusher (Focusrite Auto-Reset)");
    Shell_NotifyIcon(NIM_ADD, &nid);
    // Request balloon-capable version. Use a clean struct for NIM_SETVERSION
    // and keep legacy WM_RBUTTONUP behavior (NOTIFYICON_VERSION == 3). VERSION_4
    // changes lParam to WM_CONTEXTMENU and breaks the menu if not handled.
    NOTIFYICONDATA ver = {};
    ver.cbSize = sizeof(NOTIFYICONDATA);
    ver.hWnd = hwnd;
    ver.uID = ID_TRAY_APP_ICON;
    ver.uVersion = NOTIFYICON_VERSION; // 3, keeps WM_RBUTTONUP / WM_LBUTTONUP
    Shell_NotifyIcon(NIM_SETVERSION, &ver);
    Log("Tray icon added.");
}

void RemoveTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &nid);
    Log("Tray icon removed.");
}

void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_MANUAL_DECRUSH, L"Manual De-Crush");
    InsertMenu(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenu(hMenu, 2, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit");
    
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

void PerformDecrush() {
    std::thread([]() {
        Log("PerformDecrush: Monitor off detected. Waiting 5 seconds...");
        Sleep(5000);
        Log("PerformDecrush: 5 seconds elapsed. Firing audio reset.");
        bool ok = ReclockFocusriteDevice();
        // Marshal result to UI thread for balloon (thread-safe). wParam=success, lParam=0=auto
        PostMessage(hWnd, WM_DECRUSH_RESULT, (WPARAM)ok, (LPARAM)0);
        if (!ok) {
            Log("PerformDecrush: Audio reset failed - notification posted.");
        } else {
            Log("PerformDecrush: Audio reset succeeded.");
        }
    }).detach();
}

void PerformManualDecrush() {
    std::thread([]() {
        Log("PerformManualDecrush: Manual trigger invoked.");
        bool ok = ReclockFocusriteDevice();
        // wParam=success, lParam=1=manual
        PostMessage(hWnd, WM_DECRUSH_RESULT, (WPARAM)ok, (LPARAM)1);
    }).detach();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            Log("WindowProc: WM_CREATE received.");
            AddTrayIcon(hwnd);
            hPowerNotify = RegisterPowerSettingNotification(hwnd, &GUID_MONITOR_POWER_ON, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (hPowerNotify) {
                Log("WindowProc: Registered for monitor power notifications.");
            } else {
                Log("WindowProc: Failed to register for power notifications! Error: %d", GetLastError());
            }
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_POWERSETTINGCHANGE) {
                POWERBROADCAST_SETTING* pbs = (POWERBROADCAST_SETTING*)lParam;
                if (pbs->PowerSetting == GUID_MONITOR_POWER_ON && pbs->DataLength == sizeof(DWORD)) {
                    DWORD monitorState = *(DWORD*)pbs->Data;
                    Log("WindowProc: WM_POWERBROADCAST GUID_MONITOR_POWER_ON state=%u", monitorState);
                    if (monitorState == 0 || monitorState == 1) {
                        PerformDecrush();
                    }
                }
            }
            break;

        case WM_TRAYICON:
            // With NOTIFYICON_VERSION, lParam is WM_RBUTTONUP/WM_LBUTTONUP.
            // With NOTIFYICON_VERSION_4, system sends WM_CONTEXTMENU/NIN_SELECT.
            // Handle all variants and also LOWORD(lParam) for version-4 packing.
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP || lParam == WM_CONTEXTMENU ||
                lParam == NIN_SELECT || lParam == NIN_KEYSELECT ||
                LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                ShowContextMenu(hwnd);
            }
            break;

        case WM_DECRUSH_RESULT: {
            bool ok = (bool)wParam;
            bool isManual = (bool)lParam;
            if (!ok) {
                if (isManual) {
                    ShowTrayNotification(L"De-Crusher Failed", L"Manual de-crush failed. Is Focusrite Control running and approved?", NIIF_ERROR);
                } else {
                    ShowTrayNotification(L"De-Crusher Failed", L"Audio reset failed. Check Focusrite Control approval or restart ControlServer. Run with -logs for details.", NIIF_ERROR);
                }
            } else {
                if (isManual) {
                    ShowTrayNotification(L"De-Crusher", L"Manual de-crush succeeded.", NIIF_INFO);
                }
                // Auto success is silent to avoid spam on every monitor off/on; log only.
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_EXIT) {
                Log("WindowProc: Exit selected from tray.");
                PostQuitMessage(0);
            } else if (LOWORD(wParam) == ID_TRAY_MANUAL_DECRUSH) {
                Log("WindowProc: Manual De-Crush selected from tray.");
                PerformManualDecrush();
            }
            break;

        case WM_DESTROY:
            Log("WindowProc: WM_DESTROY received. Cleaning up...");
            if (hPowerNotify) {
                UnregisterPowerSettingNotification(hPowerNotify);
            }
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (strstr(lpCmdLine, "-logs") != NULL) {
        g_LoggingEnabled = true;
        AllocConsole();
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        SetConsoleTitle(L"De-Crusher Debug Logs");
        Log("Logging enabled. Command line: %s", lpCmdLine);
    }

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    Log("WinMain: Process priority set to ABOVE_NORMAL_PRIORITY_CLASS.");

    const wchar_t CLASS_NAME[] = L"DeCrusherTrayApp";

    WNDCLASS wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClass(&wc);
    Log("WinMain: Window class registered.");

    hWnd = CreateWindowEx(
        0,
        CLASS_NAME,
        L"De-Crusher",
        0, 
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (hWnd == NULL) {
        Log("WinMain: CreateWindowEx failed! Error: %d", GetLastError());
        return 0;
    }

    Log("WinMain: Entering message loop.");
    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Log("WinMain: Message loop exited. Closing app.");
    if (g_LoggingEnabled) {
        FreeConsole();
    }
    return 0;
}
