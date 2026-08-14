#pragma once
#include <string>

bool ReclockViaFC2Http(const std::string& host = "127.0.0.1", int port = 41780);
bool IsFC2SidecarAvailable(const std::string& host = "127.0.0.1", int port = 41780);
