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
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
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

// Helper to find TCP Port of ControlServer
USHORT FindControlServerPort(DWORD pid) {
    DWORD size = 0;
    GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return 0;

    std::vector<BYTE> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        PMIB_TCPTABLE_OWNER_PID table = (PMIB_TCPTABLE_OWNER_PID)buffer.data();
        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            if (table->table[i].dwOwningPid == pid && table->table[i].dwState == MIB_TCP_STATE_LISTEN) {
                // Return the first listening port for this PID
                return ntohs((USHORT)table->table[i].dwLocalPort);
            }
        }
    }
    return 0;
}

void SendXML(SOCKET sock, const std::string& xml) {
    char header[32];
    sprintf_s(header, "Length=%06zx ", xml.length());
    std::string fullMsg = header + xml;
    send(sock, fullMsg.c_str(), fullMsg.length(), 0);
    Log("TCP Sent: %s", xml.c_str());
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
        Log("WSAStartup failed.");
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Log("socket() failed.");
        WSACleanup();
        return false;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sock, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        Log("connect() failed.");
        closesocket(sock);
        WSACleanup();
        return false;
    }

    Log("Connected to ControlServer!");

    std::string clientKey = "decrusher-12345";
    std::string clientDetails = "<client-details hostname=\"De-Crusher\" client-key=\"" + clientKey + "\"/>";
    SendXML(sock, clientDetails);

    // Give server time to process handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Subscribe to devid="1" (typically the main/only Focusrite interface)
    std::string subscribe = "<device-subscribe devid=\"1\" subscribe=\"true\"/>";
    SendXML(sock, subscribe);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Send the sample rate flip
    // 905 is the Sample Rate ID. We set it to 48 kHz, then back to 44.1 kHz.
    Log("Flipping sample rate to 48 kHz...");
    std::string set48 = "<set devid=\"1\"><item id=\"905\" value=\"48 kHz\"/></set>";
    SendXML(sock, set48);

    Log("Waiting 2000ms for hardware PLL lock...");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    Log("Flipping sample rate back to 44.1 kHz...");
    std::string set44 = "<set devid=\"1\"><item id=\"905\" value=\"44.1 kHz\"/></set>";
    SendXML(sock, set44);

    Log("Done sending TCP commands.");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    closesocket(sock);
    WSACleanup();

    Log("ReclockFocusriteDevice (TCP Mode): Finished.");
    return true;
}
