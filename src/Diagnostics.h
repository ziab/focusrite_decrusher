#pragma once
#include <windows.h>

bool InitializeLogging(bool console, bool diagnostics);
void StartDiagnostics(HWND window);
void LogDiagnosticMessage(UINT message, WPARAM wParam, LPARAM lParam);
void StopDiagnostics();
