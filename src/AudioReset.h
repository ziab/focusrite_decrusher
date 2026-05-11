#pragma once
#include <string>

// Re-clocks Focusrite by toggling the sample rate
bool ReclockFocusriteDevice();

extern bool g_LoggingEnabled;
void Log(const char* format, ...);
