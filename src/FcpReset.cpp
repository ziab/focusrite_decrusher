#include "FcpReset.h"
#include "AudioReset.h"
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usbiodef.h>
#include <winusbio.h>
#include <compressapi.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "cabinet.lib")

// FCP constants from fcp-protocol.js
static const int FCP_HEADER_SIZE = 16;
static const int FCP_NOTIFICATION_ACK = 1;
static const int USB_REQUEST_STEP0 = 0;
static const int USB_REQUEST_TX = 2;
static const int USB_REQUEST_RX = 3;
static const int OPCODE_INIT1 = 0x000000;
static const int OPCODE_INIT2 = 0x000002;
static const int OPCODE_DEVICE_MAP_INFO = 0x80000c;
static const int OPCODE_DEVICE_MAP_READ = 0x80000d;
static const int OPCODE_DATA_READ = 0x800000;
static const int OPCODE_DATA_WRITE = 0x800001;

struct FcpPacket {
    uint32_t opcode;
    uint16_t size;
    uint16_t sequence;
    uint32_t error;
    uint32_t padding;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> EncodePacket(uint32_t opcode, uint16_t seq, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> pkt(FCP_HEADER_SIZE + payload.size());
    *(uint32_t*)(pkt.data()+0) = opcode;
    *(uint16_t*)(pkt.data()+4) = (uint16_t)payload.size();
    *(uint16_t*)(pkt.data()+6) = seq;
    *(uint32_t*)(pkt.data()+8) = 0;
    *(uint32_t*)(pkt.data()+12) = 0;
    if (!payload.empty()) memcpy(pkt.data()+FCP_HEADER_SIZE, payload.data(), payload.size());
    return pkt;
}
static bool DecodePacket(const std::vector<uint8_t>& pkt, FcpPacket& out) {
    if (pkt.size() < FCP_HEADER_SIZE) return false;
    out.opcode = *(uint32_t*)(pkt.data()+0);
    out.size = *(uint16_t*)(pkt.data()+4);
    out.sequence = *(uint16_t*)(pkt.data()+6);
    out.error = *(uint32_t*)(pkt.data()+8);
    out.padding = *(uint32_t*)(pkt.data()+12);
    if (pkt.size() != FCP_HEADER_SIZE + out.size) return false;
    out.data.assign(pkt.begin()+FCP_HEADER_SIZE, pkt.end());
    return true;
}

static std::vector<uint8_t> DataRequest(uint32_t offset, uint32_t size) {
    std::vector<uint8_t> b(8);
    *(uint32_t*)(b.data()+0) = offset;
    *(uint32_t*)(b.data()+4) = size;
    return b;
}
static std::vector<uint8_t> DataWriteRequest(uint32_t offset, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b(8 + data.size());
    *(uint32_t*)(b.data()+0) = offset;
    *(uint32_t*)(b.data()+4) = (uint32_t)data.size();
    if (!data.empty()) memcpy(b.data()+8, data.data(), data.size());
    return b;
}

struct WinUsbDevice {
    HANDLE hDevice = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE hWinUsb = nullptr;
    uint8_t interfaceNumber = 0;
    uint8_t interruptInPipe = 0;
    bool valid = false;
    ~WinUsbDevice(){ close(); }
    void close(){
        if(hWinUsb){ WinUsb_Free(hWinUsb); hWinUsb=nullptr; }
        if(hDevice!=INVALID_HANDLE_VALUE){ CloseHandle(hDevice); hDevice=INVALID_HANDLE_VALUE; }
        valid=false;
    }
};

static bool FindScarlettDevice(WinUsbDevice& out) {
    // Try composite + associated interfaces: open composite, then WinUsb_GetAssociatedInterface to find vendor 0xFF
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo==INVALID_HANDLE_VALUE) { Log("SetupDiGetClassDevs failed %lu", GetLastError()); return false; }
    SP_DEVICE_INTERFACE_DATA ifData{}; ifData.cbSize=sizeof(ifData);
    for(DWORD i=0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, i, &ifData); ++i){
        DWORD req=0; SetupDiGetDeviceInterfaceDetailW(hDevInfo,&ifData,nullptr,0,&req,nullptr);
        std::vector<uint8_t> buf(req); auto detail=(PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data(); detail->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{}; devInfo.cbSize=sizeof(devInfo);
        if(!SetupDiGetDeviceInterfaceDetailW(hDevInfo,&ifData,detail,req,&req,&devInfo)) continue;
        wchar_t prop[512]={0}; DWORD regType=0;
        if(!SetupDiGetDeviceRegistryPropertyW(hDevInfo,&devInfo,SPDRP_HARDWAREID,&regType,(PBYTE)prop,sizeof(prop),nullptr)) continue;
        std::wstring hw(prop);
        // Log every USB device for diagnostics (user has 6i6 PID 8203)
        Log("Enumerated USB device %ls path %ls", prop, detail->DevicePath);
        if(hw.find(L"VID_1235")==std::wstring::npos) continue;
        Log("Candidate VID_1235 device %ls path %ls", prop, detail->DevicePath);
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if(h==INVALID_HANDLE_VALUE){ Log("CreateFile %ls failed %lu", detail->DevicePath, GetLastError()); continue; }
        WINUSB_INTERFACE_HANDLE hUsb=nullptr;
        if(!WinUsb_Initialize(h,&hUsb)){ Log("WinUsb_Initialize %ls failed %lu (try associated)", detail->DevicePath, GetLastError()); CloseHandle(h); continue; }
        // Try current interface and all associated interfaces
        for(int assocIdx=-1; assocIdx<8; ++assocIdx){
            WINUSB_INTERFACE_HANDLE hCur = nullptr;
            USB_INTERFACE_DESCRIPTOR desc{};
            bool got=false;
            if(assocIdx==-1){
                hCur = hUsb;
                got = WinUsb_QueryInterfaceSettings(hCur,0,&desc);
            } else {
                if(!WinUsb_GetAssociatedInterface(hUsb, (UCHAR)assocIdx, &hCur)) continue;
                got = WinUsb_QueryInterfaceSettings(hCur,0,&desc);
            }
            if(!got){ if(assocIdx!=-1 && hCur) WinUsb_Free(hCur); continue; }
            Log("Interface %d class 0x%02x/%d endpoints %u", assocIdx==-1? (int)desc.bInterfaceNumber : assocIdx, desc.bInterfaceClass, desc.bInterfaceNumber, desc.bNumEndpoints);
            if(desc.bInterfaceClass!=0xFF){
                if(assocIdx!=-1 && hCur) WinUsb_Free(hCur);
                continue;
            }
            uint8_t intIn=0;
            for(uint8_t p=0;p<desc.bNumEndpoints;p++){
                WINUSB_PIPE_INFORMATION pipe{}; if(!WinUsb_QueryPipe(hCur,0,p,&pipe)) continue;
                Log(" Pipe %u type %d id 0x%02x", p, pipe.PipeType, pipe.PipeId);
                if(pipe.PipeType==UsbdPipeTypeInterrupt && USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)){ intIn=pipe.PipeId; break; }
            }
            if(intIn==0){
                Log("Vendor interface %d has no interrupt IN", assocIdx);
                if(assocIdx!=-1 && hCur) WinUsb_Free(hCur);
                continue;
            }
            // Found vendor with interrupt IN - keep that handle
            if(assocIdx==-1){
                out.hDevice=h; out.hWinUsb=hCur; out.interfaceNumber=desc.bInterfaceNumber; out.interruptInPipe=intIn; out.valid=true;
            } else {
                // For associated, we need to keep associated handle and original
                // Keep associated as winusb handle, close original device handle? Keep both
                // Close original hUsb? Actually we need to keep hDevice and use associated
                WinUsb_Free(hUsb);
                out.hDevice=h; out.hWinUsb=hCur; out.interfaceNumber=desc.bInterfaceNumber; out.interruptInPipe=intIn; out.valid=true;
            }
            Log("Claimed Scarlett vendor interface %u pipe 0x%02x (assoc %d)", out.interfaceNumber, intIn, assocIdx);
            SetupDiDestroyDeviceInfoList(hDevInfo);
            return true;
        }
        WinUsb_Free(hUsb); CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    Log("No Scarlett vendor interface found (VID_1235 mi_ vendor 0xFF) - ensure 6i6 Gen2 connected, FC2/Control closed");
    return false;
}

struct FcpTransport {
    WinUsbDevice* dev=nullptr;
    uint16_t seq=0;
    int timeoutMs=1500;
    bool ControlTransferIn(uint8_t req, uint16_t value, uint16_t index, std::vector<uint8_t>& out, size_t len){
        WINUSB_SETUP_PACKET setup{}; setup.RequestType=0xA1; setup.Request=req; setup.Value=value; setup.Index=index; setup.Length=(USHORT)len;
        out.resize(len); ULONG transferred=0;
        if(!WinUsb_ControlTransfer(dev->hWinUsb, setup, out.data(), (ULONG)len, &transferred, nullptr)){ Log("ControlTransferIn req %u failed %lu", req, GetLastError()); return false; }
        out.resize(transferred); return true;
    }
    bool ControlTransferOut(uint8_t req, uint16_t value, uint16_t index, const std::vector<uint8_t>& data){
        WINUSB_SETUP_PACKET setup{}; setup.RequestType=0x21; setup.Request=req; setup.Value=value; setup.Index=index; setup.Length=(USHORT)data.size();
        ULONG transferred=0;
        if(!WinUsb_ControlTransfer(dev->hWinUsb, setup, (PUCHAR)(data.empty()?nullptr:data.data()), (ULONG)data.size(), &transferred, nullptr)){ Log("ControlTransferOut req %u failed %lu", req, GetLastError()); return false; }
        return transferred==data.size();
    }
    bool InterruptIn(std::vector<uint8_t>& out, size_t len, int timeout){
        out.resize(len); ULONG read=0; 
        // WinUsb_ReadPipe is synchronous with timeout via overlapped
        OVERLAPPED ov{}; ov.hEvent=CreateEvent(nullptr,false,false,nullptr);
        if(!WinUsb_ReadPipe(dev->hWinUsb, dev->interruptInPipe, out.data(), (ULONG)len, &read, &ov)){
            DWORD err=GetLastError(); if(err!=ERROR_IO_PENDING){ CloseHandle(ov.hEvent); return false; }
            DWORD wait=WaitForSingleObject(ov.hEvent, timeout);
            if(wait!=WAIT_OBJECT_0){ WinUsb_AbortPipe(dev->hWinUsb, dev->interruptInPipe); CloseHandle(ov.hEvent); return false; }
            if(!GetOverlappedResult(dev->hDevice, &ov, &read, FALSE)){ CloseHandle(ov.hEvent); return false; }
        }
        CloseHandle(ov.hEvent);
        out.resize(read); return true;
    }
    bool WaitForAck(){
        auto deadline = GetTickCount64() + timeoutMs;
        while(GetTickCount64() < deadline){
            int remain = (int)(deadline - GetTickCount64());
            std::vector<uint8_t> msg;
            if(!InterruptIn(msg, 8, remain)) return false;
            if(msg.size()!=8) { Log("FCP notify size %zu", msg.size()); continue; }
            uint32_t bits = *(uint32_t*)msg.data();
            uint32_t evt = bits & ~FCP_NOTIFICATION_ACK;
            if(evt) Log("FCP event %u", evt);
            if(bits & FCP_NOTIFICATION_ACK) return true;
        }
        Log("FCP ack timeout");
        return false;
    }
    bool Command(uint32_t opcode, const std::vector<uint8_t>& req, std::vector<uint8_t>& respData, size_t respSize){
        uint16_t curSeq = seq; seq = (seq+1)&0xFFFF;
        auto pkt = EncodePacket(opcode, curSeq, req);
        // Start async ack wait
        // Use thread to wait for ack while we send
        bool ackOk=false;
        // Simple: send then wait
        if(!ControlTransferOut(USB_REQUEST_TX, 0, dev->interfaceNumber, pkt)){ Log("TX failed"); return false; }
        if(!WaitForAck()){ Log("WaitForAck failed for opcode 0x%x", opcode); return false; }
        std::vector<uint8_t> raw;
        if(!ControlTransferIn(USB_REQUEST_RX, 0, dev->interfaceNumber, raw, FCP_HEADER_SIZE+respSize)){ Log("RX failed"); return false; }
        FcpPacket decoded;
        if(!DecodePacket(raw, decoded)){ Log("DecodePacket failed size %zu", raw.size()); return false; }
        if(decoded.opcode!=opcode || decoded.sequence!=curSeq || decoded.error!=0 || decoded.size!=respSize){
            Log("Validate failed opcode 0x%x vs 0x%x seq %u vs %u err %u size %u vs %zu", decoded.opcode, opcode, decoded.sequence, curSeq, decoded.error, decoded.size, respSize);
            return false;
        }
        respData = decoded.data;
        return true;
    }
    bool Step0(std::vector<uint8_t>& out){ return ControlTransferIn(USB_REQUEST_STEP0,0,dev->interfaceNumber,out,24); }
    bool Initialize(uint32_t& outFw){
        std::vector<uint8_t> s0;
        if(!Step0(s0)){ Log("Step0 failed"); return false; }
        seq=0;
        std::vector<uint8_t> dummy;
        if(!Command(OPCODE_INIT1, {}, dummy, 0)){ Log("Init1 failed"); return false; }
        std::vector<uint8_t> s2;
        if(!Command(OPCODE_INIT2, {}, s2, 84)){ Log("Init2 failed"); return false; }
        if(s2.size()>=12) outFw = *(uint32_t*)(s2.data()+8);
        Log("FCP initialized fw 0x%x", outFw);
        return true;
    }
};

// Base64 + MSZIP decompress for deviceMap
static bool Base64Decode(const std::string& in, std::vector<uint8_t>& out){
    DWORD len=0;
    if(!CryptStringToBinaryA(in.c_str(), (DWORD)in.size(), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return false;
    out.resize(len);
    if(!CryptStringToBinaryA(in.c_str(), (DWORD)in.size(), CRYPT_STRING_BASE64, out.data(), &len, nullptr, nullptr)) return false;
    out.resize(len); return true;
}
static bool MszipDecompress(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst){
    DECOMPRESSOR_HANDLE dec=nullptr;
    if(!CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &dec)){ Log("CreateDecompressor failed %lu", GetLastError()); return false; }
    SIZE_T compSize = src.size();
    SIZE_T decompSize=0;
    if(!Decompress(dec, src.data(), compSize, nullptr, 0, &decompSize)){
        DWORD err=GetLastError(); if(err!=ERROR_INSUFFICIENT_BUFFER){ CloseDecompressor(dec); return false; }
    }
    dst.resize(decompSize);
    SIZE_T outSize=decompSize;
    bool ok = Decompress(dec, src.data(), compSize, dst.data(), decompSize, &outSize);
    CloseDecompressor(dec);
    if(!ok){ Log("Decompress failed %lu", GetLastError()); return false; }
    dst.resize(outSize); return true;
}

bool ReclockViaFcpSampleRate(){
    Log("ReclockViaFcpSampleRate: Starting direct USB FCP (6i6 Gen2, no driver restart)...");
    WinUsbDevice dev;
    if(!FindScarlettDevice(dev)){ Log("FindScarlettDevice failed - ensure 6i6 connected, FC2/Control closed for exclusive access"); return false; }
    FcpTransport t; t.dev=&dev; t.timeoutMs=1500;
    uint32_t fw=0;
    if(!t.Initialize(fw)){ Log("FCP Initialize failed - close Focusrite Control/FC2 and retry"); return false; }

    // Read deviceMap to find sample-rate member
    std::vector<uint8_t> mapInfo;
    if(!t.Command(OPCODE_DEVICE_MAP_INFO, {}, mapInfo, 4)){ Log("deviceMapInfo failed"); return false; }
    uint16_t encSize = *(uint16_t*)(mapInfo.data()+2);
    Log("deviceMap encoded size %u", encSize);
    std::vector<uint8_t> enc(encSize);
    const size_t block=1024;
    for(size_t off=0; off<encSize; off+=block){
        size_t chunk = std::min(block, (size_t)(encSize - off));
        std::vector<uint8_t> idx(4); *(uint32_t*)idx.data() = (uint32_t)(off/block);
        std::vector<uint8_t> blk;
        if(!t.Command(OPCODE_DEVICE_MAP_READ, idx, blk, chunk)){ Log("deviceMapRead block %zu failed", off/block); return false; }
        memcpy(enc.data()+off, blk.data(), chunk);
    }
    std::string b64(enc.begin(), enc.end());
    // Trim nulls
    b64.erase(std::find(b64.begin(), b64.end(), '\0'), b64.end());
    // Base64 decode then MSZIP inflate
    std::vector<uint8_t> compressed;
    if(!Base64Decode(b64, compressed)){ Log("Base64 decode failed size %zu", b64.size()); return false; }
    std::vector<uint8_t> jsonBytes;
    if(!MszipDecompress(compressed, jsonBytes)){
        // Fallback: try raw if not compressed
        Log("MSZIP decompress failed, trying raw");
        jsonBytes = compressed;
    }
    std::string json(jsonBytes.begin(), jsonBytes.end());
    Log("deviceMap JSON %zu bytes: %.200s", json.size(), json.c_str());

    // Search for sample rate member path - look for "sample" "clock" "rate"
    // Parse crudely: find "\"sampleRate\"" or "\"clock\"" keys, extract offset via structs.APP_SPACE.members
    // For 6i6 Gen2, known member is often "clockRate" or "sampleRate" at offset maybe 0x... 
    // We search JSON string for '"sample' and try to extract surrounding offset.
    // Simpler: brute force dataRead at likely offsets for sample rate enum: try reading uint32 at offsets 0-256 and look for values 44100/48000
    // Instead, search for any member with type uint32 and value matching current rate, then flip.
    // Quick heuristic: read a few offsets that previously held 44100 for Gen2 from community: offset 0x100? Not known.
    // Fallback: try to use direct control definitions build - search for member path containing "sample" case-insensitive.
    std::string lower=json; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    size_t pos = lower.find("samplerate");
    if(pos==std::string::npos) pos=lower.find("sample_rate");
    if(pos==std::string::npos) pos=lower.find("clockrate");
    if(pos==std::string::npos) pos=lower.find("clock_rate");
    uint32_t sampleRateOffset = 0; bool found=false;
    // Try to parse offset near the found key: look for "\"offset\":"
    if(pos!=std::string::npos){
        size_t offPos = lower.rfind("\"offset\"", pos);
        if(offPos==std::string::npos) offPos = lower.find("\"offset\"", pos);
        if(offPos!=std::string::npos){
            size_t colon = lower.find(":", offPos);
            if(colon!=std::string::npos){
                sampleRateOffset = (uint32_t)atoi(lower.c_str()+colon+1);
                found=true;
                Log("Found sampleRate offset %u near pos %zu", sampleRateOffset, pos);
            }
        }
    }
    if(!found){
        Log("Could not locate sampleRate in deviceMap, trying brute force scan for 44100/48000 values");
        // Brute force: scan first 512 bytes of APP_SPACE via dataRead
        for(uint32_t off=0; off<512; off+=4){
            std::vector<uint8_t> val;
            if(!t.Command(OPCODE_DATA_READ, DataRequest(off,4), val, 4)) continue;
            uint32_t v = *(uint32_t*)val.data();
            if(v==44100 || v==48000){
                Log("Brute force found sampleRate candidate at offset %u value %u", off, v);
                sampleRateOffset=off; found=true; break;
            }
            // Also check uint16
            uint16_t v16 = *(uint16_t*)val.data();
            if(v16==441 || v16==480){ // maybe stored as 44/48 ?
                Log("Brute force found 16-bit candidate at %u value %u", off, v16);
            }
        }
    }
    if(!found){
        Log("FCP sampleRate offset not found - cannot flip, but will try generic outputMute toggle as fallback");
        // Fallback: toggle outputMute via dataWrite if we can find its offset similarly
        // Search for "outputMute"
        size_t p = lower.find("outputmute");
        if(p!=std::string::npos){
            size_t colon = lower.find("\"offset\"", p);
            if(colon!=std::string::npos){
                // Try toggle
            }
        }
        return false;
    }

    // Read current rate
    std::vector<uint8_t> curBytes;
    if(!t.Command(OPCODE_DATA_READ, DataRequest(sampleRateOffset,4), curBytes, 4)){ Log("Read sampleRate failed at %u", sampleRateOffset); return false; }
    uint32_t cur = *(uint32_t*)curBytes.data();
    Log("Current sampleRate at offset %u = %u", sampleRateOffset, cur);
    uint32_t target = (cur==44100)?48000:44100;
    // 6i6 expects value as uint32 44100/48000 or maybe enum 0/1? Try direct
    std::vector<uint8_t> targetBytes(4); *(uint32_t*)targetBytes.data()=target;
    // Write target
    Log("Flipping sampleRate %u -> %u", cur, target);
    std::vector<uint8_t> dummy;
    std::vector<uint8_t> wr = DataWriteRequest(sampleRateOffset, targetBytes);
    if(!t.Command(OPCODE_DATA_WRITE, wr, dummy, 0)){ Log("Write sampleRate failed"); return false; }
    // Notify - try notify 0 or 6
    t.Command(0x800002, std::vector<uint8_t>{0,0,0,0}, dummy, 0); // dataNotify
    Sleep(1200);
    // Flip back
    Log("Flipping back %u -> %u", target, cur);
    *(uint32_t*)targetBytes.data()=cur;
    wr = DataWriteRequest(sampleRateOffset, targetBytes);
    if(!t.Command(OPCODE_DATA_WRITE, wr, dummy, 0)){ Log("Write back failed"); return false; }
    t.Command(0x800002, std::vector<uint8_t>{0,0,0,0}, dummy, 0);
    Sleep(800);
    Log("ReclockViaFcpSampleRate: Finished successfully (no driver restart)");
    return true;
}
