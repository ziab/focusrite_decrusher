#include "LegacyControlServer.h"
#include "AudioReset.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <vector>
#include <thread>
#include <chrono>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

static DWORD FindControlServerPID() {
    PROCESSENTRY32 entry; entry.dwSize = sizeof(PROCESSENTRY32);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { Log("CreateToolhelp32Snapshot failed %lu", GetLastError()); return 0; }
    if (Process32First(snap, &entry)) do { if (_wcsicmp(entry.szExeFile, L"ControlServer.exe")==0){CloseHandle(snap); return entry.th32ProcessID;}} while(Process32Next(snap,&entry));
    CloseHandle(snap); return 0;
}
static USHORT FindControlServerPort(DWORD pid) {
    DWORD size=0; GetExtendedTcpTable(NULL,&size,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0); if(size==0) return 0;
    std::vector<BYTE> buf(size);
    if(GetExtendedTcpTable(buf.data(),&size,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)==NO_ERROR){
        PMIB_TCPTABLE_OWNER_PID t=(PMIB_TCPTABLE_OWNER_PID)buf.data(); USHORT fallback=0;
        for(DWORD i=0;i<t->dwNumEntries;i++) if(t->table[i].dwOwningPid==pid && t->table[i].dwState==MIB_TCP_STATE_LISTEN){
            DWORD addr=t->table[i].dwLocalAddr; bool loop=ntohl(addr)==INADDR_LOOPBACK; bool any=addr==0; USHORT p=ntohs((USHORT)t->table[i].dwLocalPort);
            if(loop) return p; if(any && fallback==0) fallback=p; else if(fallback==0 && !loop && !any) fallback=p;
        }
        if(fallback) Log("FindControlServerPort: fallback %u",fallback);
        return fallback;
    } return 0;
}
static bool SendAll(SOCKET s,const char* d,int l){int t=0;while(t<l){int n=send(s,d+t,l-t,0); if(n==SOCKET_ERROR){Log("send failed %d",WSAGetLastError()); return false;} t+=n;} return true;}
static bool WaitForServerResponse(SOCKET sock,int timeoutMs,std::string* out){
    fd_set rs; FD_ZERO(&rs); FD_SET(sock,&rs); timeval tv{timeoutMs/1000,(timeoutMs%1000)*1000};
    int sel=select(0,&rs,nullptr,nullptr,&tv); if(sel==SOCKET_ERROR){Log("select failed %d",WSAGetLastError()); return false;}
    if(sel==0){Log("WaitForServerResponse timeout %dms",timeoutMs); return true;}
    char buf[4096]; int r=recv(sock,buf,sizeof(buf)-1,0);
    if(r==0){Log("recv closed"); return false;}
    if(r==SOCKET_ERROR){int e=WSAGetLastError(); if(e==WSAEWOULDBLOCK) return true; Log("recv failed %d",e); return false;}
    buf[r]=0; if(out) *out=std::string(buf,r); Log("TCP Recv %d: %s",r,buf);
    u_long nb=1; ioctlsocket(sock,FIONBIO,&nb); while(true){r=recv(sock,buf,sizeof(buf)-1,0); if(r<=0) break; buf[r]=0; Log("drained %d: %s",r,buf); if(out) out->append(buf,r);} nb=0; ioctlsocket(sock,FIONBIO,&nb); return true;
}
static bool SendXML(SOCKET sock,const std::string& xml){
    char hdr[32]; sprintf_s(hdr,"Length=%06zx ",xml.length()); std::string msg=std::string(hdr)+xml;
    if(!SendAll(sock,msg.c_str(),(int)msg.length())){Log("SendXML failed %s",xml.c_str()); return false;}
    Log("TCP Sent: %s",xml.c_str()); return true;
}

bool ReclockViaLegacyControlServer(){
    Log("ReclockViaLegacyControlServer (Control 1, 6i6 Gen2): Starting...");
    DWORD pid=FindControlServerPID(); if(pid==0){Log("Failed to find ControlServer.exe"); return false;}
    Log("Found ControlServer PID %lu",pid);
    USHORT port=FindControlServerPort(pid); if(port==0){Log("Failed to find port"); return false;}
    Log("ControlServer port %u",port);
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)!=0){Log("WSAStartup failed %d",WSAGetLastError()); return false;}
    SOCKET sock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); if(sock==INVALID_SOCKET){Log("socket failed %d",WSAGetLastError()); WSACleanup(); return false;}
    DWORD rto=3000; setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(char*)&rto,sizeof(rto));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(port); inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    if(connect(sock,(SOCKADDR*)&addr,sizeof(addr))==SOCKET_ERROR){Log("connect failed %d",WSAGetLastError()); closesocket(sock); WSACleanup(); return false;}
    Log("Connected to ControlServer!");
    std::string key="decrusher-12345"; std::string cd="<client-details hostname=\"De-Crusher\" client-key=\""+key+"\"/>";
    if(!SendXML(sock,cd)){closesocket(sock); WSACleanup(); return false;}
    {std::string resp; if(!WaitForServerResponse(sock,2000,&resp)){Log("handshake failed"); closesocket(sock); WSACleanup(); return false;} if(resp.find("error")!=std::string::npos||resp.find("denied")!=std::string::npos){Log("denied %s",resp.c_str()); closesocket(sock); WSACleanup(); return false;}}
    std::string sub="<device-subscribe devid=\"1\" subscribe=\"true\"/>"; if(!SendXML(sock,sub)){closesocket(sock); WSACleanup(); return false;}
    {std::string resp; if(!WaitForServerResponse(sock,2000,&resp)){Log("subscribe failed"); closesocket(sock); WSACleanup(); return false;} if(resp.find("error")!=std::string::npos){Log("subscribe error %s",resp.c_str()); closesocket(sock); WSACleanup(); return false;}}
    Log("Flipping 48kHz..."); std::string s48="<set devid=\"1\"><item id=\"905\" value=\"48 kHz\"/></set>"; if(!SendXML(sock,s48)){closesocket(sock); WSACleanup(); return false;}
    {std::string resp; if(!WaitForServerResponse(sock,2000,&resp)){Log("set48 ack fail"); closesocket(sock); WSACleanup(); return false;} if(resp.find("error")!=std::string::npos){Log("set48 error %s",resp.c_str()); closesocket(sock); WSACleanup(); return false;}}
    Log("Waiting 2000ms PLL..."); std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    Log("Flipping 44.1kHz..."); std::string s44="<set devid=\"1\"><item id=\"905\" value=\"44.1 kHz\"/></set>"; if(!SendXML(sock,s44)){closesocket(sock); WSACleanup(); return false;}
    {std::string resp; if(!WaitForServerResponse(sock,2000,&resp)){Log("set44 ack fail"); closesocket(sock); WSACleanup(); return false;} if(resp.find("error")!=std::string::npos){Log("set44 error %s",resp.c_str()); closesocket(sock); WSACleanup(); return false;}}
    Log("Done"); std::string fin; WaitForServerResponse(sock,500,&fin); closesocket(sock); WSACleanup(); Log("Legacy Reclock Finished successfully"); return true;
}
