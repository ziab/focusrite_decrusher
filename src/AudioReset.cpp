#include "AudioReset.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// Helper to find PID of ControlServer.exe
DWORD FindControlServerPID() {
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log("CreateToolhelp32Snapshot failed: %lu", GetLastError());
        return 0;
    }
    if (Process32First(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ControlServer.exe") == 0) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

// Helper to find TCP Port of ControlServer - filters for loopback interface
USHORT FindControlServerPort(DWORD pid) {
    DWORD size = 0;
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return 0;

    std::vector<BYTE> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        PMIB_TCPTABLE_OWNER_PID table = (PMIB_TCPTABLE_OWNER_PID)buffer.data();
        USHORT fallbackPort = 0;
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            if (table->table[i].dwOwningPid == pid && table->table[i].dwState == MIB_TCP_STATE_LISTEN) {
                DWORD addr = table->table[i].dwLocalAddr;
                // Prefer 127.0.0.1 (loopback). Fall back to 0.0.0.0 (any) if no loopback entry.
                bool isLoopback = (ntohl(addr) == INADDR_LOOPBACK); // 127.0.0.1
                bool isAny = (addr == 0); // 0.0.0.0
                USHORT port = ntohs((USHORT)table->table[i].dwLocalPort);
                if (isLoopback) {
                    return port;
                }
                if (isAny && fallbackPort == 0) {
                    fallbackPort = port;
                } else if (fallbackPort == 0 && !isLoopback && !isAny) {
                    // Keep as last resort if ControlServer binds to specific IP
                    fallbackPort = port;
                }
            }
        }
        if (fallbackPort != 0) {
            Log("FindControlServerPort: no 127.0.0.1 listener, using fallback port %u", fallbackPort);
        }
        return fallbackPort;
    }
    return 0;
}

bool SendAll(SOCKET sock, const char* data, int len) {
    int totalSent = 0;
    while (totalSent < len) {
        int sent = send(sock, data + totalSent, len - totalSent, 0);
        if (sent == SOCKET_ERROR) {
            Log("send() failed: %d", WSAGetLastError());
            return false;
        }
        totalSent += sent;
    }
    return true;
}

// Wait for and drain server response with timeout. Returns true if any data received or timeout is acceptable.
// If server sends error/close, returns false.
bool WaitForServerResponse(SOCKET sock, int timeoutMs, std::string* outResponse) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int sel = select(0, &readSet, NULL, NULL, &tv);
    if (sel == SOCKET_ERROR) {
        Log("select() failed: %d", WSAGetLastError());
        return false;
    }
    if (sel == 0) {
        // Timeout - no data. For ControlServer handshake, this can mean approval pending
        // but we treat as warning, not hard failure; caller decides.
        Log("WaitForServerResponse: timeout after %dms (no data)", timeoutMs);
        return true; // not fatal, let caller continue but log
    }

    char buf[4096];
    int recvd = recv(sock, buf, sizeof(buf) - 1, 0);
    if (recvd == 0) {
        Log("recv(): server closed connection");
        return false;
    }
    if (recvd == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return true;
        Log("recv() failed: %d", err);
        return false;
    }
    buf[recvd] = '\0';
    if (outResponse) *outResponse = std::string(buf, recvd);
    Log("TCP Recv (%d bytes): %s", recvd, buf);

    // Drain any additional buffered data without blocking
    u_long nonBlock = 1;
    ioctlsocket(sock, FIONBIO, &nonBlock);
    while (true) {
        recvd = recv(sock, buf, sizeof(buf) - 1, 0);
        if (recvd <= 0) break;
        buf[recvd] = '\0';
        Log("TCP Recv (drained %d bytes): %s", recvd, buf);
        if (outResponse) outResponse->append(buf, recvd);
    }
    nonBlock = 0;
    ioctlsocket(sock, FIONBIO, &nonBlock);

    return true;
}

bool SendXML(SOCKET sock, const std::string& xml) {
    char header[32];
    sprintf_s(header, "Length=%06zx ", xml.length());
    std::string fullMsg = header + xml;
    if (!SendAll(sock, fullMsg.c_str(), (int)fullMsg.length())) {
        Log("SendXML failed for: %s", xml.c_str());
        return false;
    }
    Log("TCP Sent: %s", xml.c_str());
    return true;
}

bool ReclockFocusriteDevice() {
    Log("ReclockFocusriteDevice (TCP Mode): Starting...");

    DWORD pid = FindControlServerPID();
    if (pid == 0) {
        Log("Failed to find ControlServer.exe process.");
        return false;
    }
    Log("Found ControlServer PID: %lu", pid);

    USHORT port = FindControlServerPort(pid);
    if (port == 0) {
        Log("Failed to find listening port for ControlServer.");
        return false;
    }
    Log("ControlServer listening on port: %u", port);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("WSAStartup failed: %d", WSAGetLastError());
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Log("socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // Set recv timeout so WaitForServerResponse doesn't block indefinitely
    DWORD recvTimeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sock, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        Log("connect() failed: %d", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return false;
    }

    Log("Connected to ControlServer!");

    std::string clientKey = "decrusher-12345";
    std::string clientDetails = "<client-details hostname=\"De-Crusher\" client-key=\"" + clientKey + "\"/>";
    if (!SendXML(sock, clientDetails)) {
        closesocket(sock);
        WSACleanup();
        return false;
    }
    // Wait for handshake ack instead of blind sleep - ensures approval/handshake processed
    {
        std::string resp;
        if (!WaitForServerResponse(sock, 2000, &resp)) {
            Log("Handshake response failed - server closed connection (approval may be pending)");
            closesocket(sock);
            WSACleanup();
            return false;
        }
        // If server replies with error or denies client, detect it
        if (resp.find("error") != std::string::npos || resp.find("denied") != std::string::npos) {
            Log("ControlServer denied handshake: %s", resp.c_str());
            closesocket(sock);
            WSACleanup();
            return false;
        }
    }

    // Subscribe to devid="1" (typically the main/only Focusrite interface)
    std::string subscribe = "<device-subscribe devid=\"1\" subscribe=\"true\"/>";
    if (!SendXML(sock, subscribe)) {
        closesocket(sock);
        WSACleanup();
        return false;
    }
    {
        std::string resp;
        if (!WaitForServerResponse(sock, 2000, &resp)) {
            Log("Subscribe response failed - server closed connection");
            closesocket(sock);
            WSACleanup();
            return false;
        }
        if (resp.find("error") != std::string::npos) {
            Log("Subscribe failed: %s", resp.c_str());
            closesocket(sock);
            WSACleanup();
            return false;
        }
    }

    // Send the sample rate flip - each <set> must be acknowledged; previously fire-and-forget
    // caused message to be dropped if ControlServer hadn't finished processing subscribe.
    // 905 is the Sample Rate ID. We set it to 48 kHz, then back to 44.1 kHz.
    Log("Flipping sample rate to 48 kHz...");
    std::string set48 = "<set devid=\"1\"><item id=\"905\" value=\"48 kHz\"/></set>";
    if (!SendXML(sock, set48)) {
        closesocket(sock);
        WSACleanup();
        return false;
    }
    {
        std::string resp;
        if (!WaitForServerResponse(sock, 2000, &resp)) {
            Log("<set 48kHz> not acknowledged - server closed");
            closesocket(sock);
            WSACleanup();
            return false;
        }
        if (resp.find("error") != std::string::npos) {
            Log("<set 48kHz> rejected: %s", resp.c_str());
            closesocket(sock);
            WSACleanup();
            return false;
        }
    }

    Log("Waiting 2000ms for hardware PLL lock...");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    Log("Flipping sample rate back to 44.1 kHz...");
    std::string set44 = "<set devid=\"1\"><item id=\"905\" value=\"44.1 kHz\"/></set>";
    if (!SendXML(sock, set44)) {
        closesocket(sock);
        WSACleanup();
        return false;
    }
    {
        std::string resp;
        if (!WaitForServerResponse(sock, 2000, &resp)) {
            Log("<set 44.1kHz> not acknowledged - server closed");
            closesocket(sock);
            WSACleanup();
            return false;
        }
        if (resp.find("error") != std::string::npos) {
            Log("<set 44.1kHz> rejected: %s", resp.c_str());
            closesocket(sock);
            WSACleanup();
            return false;
        }
    }

    Log("Done sending TCP commands.");
    // Drain any final notifications before close
    std::string finalResp;
    WaitForServerResponse(sock, 500, &finalResp);

    closesocket(sock);
    WSACleanup();

    Log("ReclockFocusriteDevice (TCP Mode): Finished successfully.");
    return true;
}
