#include "FC2HttpClient.h"
#include "AudioReset.h"
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static bool HttpPostJson(const std::string& host, int port, const std::wstring& path, const std::string& body, std::string& outResponse, DWORD& outStatus) {
    outResponse.clear();
    outStatus = 0;
    HINTERNET hSession = WinHttpOpen(L"DeCrusher/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        Log("WinHttpOpen failed: %lu", GetLastError());
        return false;
    }
    std::wstring wHost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        Log("WinHttpConnect %s:%d failed: %lu", host.c_str(), port, GetLastError());
        WinHttpCloseHandle(hSession);
        return false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        Log("WinHttpOpenRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Timeouts: 2s connect, 3s send/receive
    WinHttpSetTimeouts(hRequest, 2000, 2000, 3000, 3000);

    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.length(), (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok) {
        Log("WinHttpSendRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        Log("WinHttpReceiveResponse failed: %lu", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    outStatus = status;

    DWORD available = 0;
    std::vector<char> buf;
    do {
        DWORD queried = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &queried)) break;
        if (queried == 0) break;
        size_t prev = buf.size();
        buf.resize(prev + queried);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data() + prev, queried, &read)) break;
        if (read == 0) break;
        buf.resize(prev + read);
    } while (true);

    outResponse.assign(buf.begin(), buf.end());
    Log("HTTP POST %ls -> %lu %s", path.c_str(), status, outResponse.c_str());

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return status >= 200 && status < 300;
}

static bool HttpGet(const std::string& host, int port, const std::wstring& path, std::string& outResponse, DWORD& outStatus) {
    outResponse.clear();
    outStatus = 0;
    HINTERNET hSession = WinHttpOpen(L"DeCrusher/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    std::wstring wHost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    WinHttpSetTimeouts(hRequest, 1500, 1500, 2000, 2000);
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }
    DWORD status = 0; DWORD sz = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    outStatus = status;
    DWORD available = 0;
    std::vector<char> buf;
    do {
        DWORD q = 0; if (!WinHttpQueryDataAvailable(hRequest, &q) || q==0) break;
        size_t prev = buf.size(); buf.resize(prev+q);
        DWORD r=0; WinHttpReadData(hRequest, buf.data()+prev, q, &r); buf.resize(prev+r); if(r==0) break;
    } while(true);
    outResponse.assign(buf.begin(), buf.end());
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return status >=200 && status<300;
}

bool IsFC2SidecarAvailable(const std::string& host, int port) {
    std::string resp; DWORD status=0;
    if (!HttpGet(host, port, L"/api/v1/health", resp, status)) {
        Log("FC2 sidecar health check failed (no service on %s:%d)", host.c_str(), port);
        return false;
    }
    // health returns JSON with ok:true, backend, paired etc.
    bool ok = resp.find("\"ok\":true") != std::string::npos || resp.find("\"ok\": true") != std::string::npos;
    Log("FC2 health: ok=%d %s", ok, resp.c_str());
    return ok;
}

bool ReclockViaFC2Http(const std::string& host, int port) {
    Log("ReclockViaFC2Http: checking sidecar %s:%d", host.c_str(), port);
    if (!IsFC2SidecarAvailable(host, port)) {
        Log("FC2 sidecar not available - instruct user to run 'focusrite api' (see README)");
        return false;
    }

    // For 4th Gen Scarlett, toggling dim+mute via AES70 forces control path and can re-sync.
    // The unofficial API's FC2 backend exposes dim/monitor-mute/monitor-gain etc.
    // We issue a batch that toggles dim on then off with a short PLL settle.
    // If that doesn't re-clock, user can switch FC2 backend to usb (requires FC2 closed) where
    // direct FCP routing/mixer writes are available - documented in README.
    std::string resp; DWORD status=0;

    // First, get current state to know toggle direction (read is cheap)
    // We simply set dim=true then dim=false - idempotent and forces two AES70 writes.
    std::string batchOn = R"({"operations":[{"control":"dim","value":true}]})";
    if (!HttpPostJson(host, port, L"/api/v1/batch", batchOn, resp, status)) {
        Log("FC2 batch dim=on failed %lu %s", status, resp.c_str());
        return false;
    }
    if (resp.find("\"ok\":false") != std::string::npos) {
        Log("FC2 batch dim=on returned ok:false %s", resp.c_str());
        return false;
    }

    Sleep(150);

    std::string batchOff = R"({"operations":[{"control":"dim","value":false}]})";
    if (!HttpPostJson(host, port, L"/api/v1/batch", batchOff, resp, status)) {
        Log("FC2 batch dim=off failed %lu %s", status, resp.c_str());
        return false;
    }
    if (resp.find("\"ok\":false") != std::string::npos) {
        Log("FC2 batch dim=off returned ok:false %s", resp.c_str());
        return false;
    }

    // Small settle for hardware
    Sleep(200);
    Log("ReclockViaFC2Http: FC2 batch succeeded");
    return true;
}
