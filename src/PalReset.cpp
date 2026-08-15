#include "PalReset.h"
#include "AudioReset.h"
#include <windows.h>
#include <stdint.h>
#include <atomic>
#include <thread>
#include <chrono>

namespace Pal {
    class DeviceDelegate {
    public:
        virtual ~DeviceDelegate() {}
        virtual void sampleRateChanged() { Log("Pal::DeviceDelegate: sampleRateChanged notification fired"); }
        virtual void clockLockChanged() { Log("Pal::DeviceDelegate: clockLockChanged notification fired"); }
        virtual void bufferSizeChanged() { Log("Pal::DeviceDelegate: bufferSizeChanged notification fired"); }
        virtual void clockSourceChanged() { Log("Pal::DeviceDelegate: clockSourceChanged notification fired"); }
        virtual void mount() {}
        virtual void unmount() {}
        virtual void userDcpReceived(unsigned int) {}
        virtual void firmwareProgress(unsigned int, int) {}
    };

    class SystemDelegate {
    public:
        virtual ~SystemDelegate() {}
        virtual void deviceAdded(void* devRaw) {
            void** sptr = (void**)devRaw;
            Log("Pal::SystemDelegate: deviceAdded (Device ptr=%p)", sptr[0]);
            detectedDevicePtr = sptr[0];
            ready = true;
        }
        virtual void deviceRemoved(void* devRaw) {
            Log("Pal::SystemDelegate: deviceRemoved");
            detectedDevicePtr = nullptr;
            ready = false;
        }
        virtual bool supportsUsb() { return true; }
        virtual bool supportsUsbDevice(unsigned int) { return true; }

        std::atomic<void*> detectedDevicePtr{nullptr};
        std::atomic<bool> ready{false};
    };
}

typedef void (*CreateSystemFn)(void* outSharedPtr, Pal::SystemDelegate*);
typedef void (*SetDelegateFn)(void*, Pal::DeviceDelegate*);
typedef uint32_t (*GetRateFn)(void*);
typedef void (*SetRateFn)(void*, uint32_t);

// Static/persistent instances to maintain driver connection throughout application lifecycle
static Pal::SystemDelegate g_PalSysDelegate;
static Pal::DeviceDelegate g_PalDevDelegate;
static alignas(16) char g_PalSysStorage[64] = {0};
static std::atomic<bool> g_PalInitialized{false};
static std::atomic<bool> g_DelegateAttached{false};

bool InitializePal() {
    if (g_PalInitialized) return true;

    Log("InitializePal: Loading FocusritePal64.dll from system...");
    HMODULE hPal = LoadLibraryW(L"C:\\Windows\\System32\\FocusritePal64.dll");
    if (!hPal) {
        hPal = LoadLibraryW(L"FocusritePal64.dll");
    }
    if (!hPal) {
        Log("InitializePal: Failed to load FocusritePal64.dll (Error: %lu)", GetLastError());
        return false;
    }

    CreateSystemFn createSys = (CreateSystemFn)GetProcAddress(hPal, "?createSystem@System@Pal@@SA?AV?$shared_ptr@VSystem@Pal@@@std@@PEAVSystemDelegate@2@@Z");
    if (!createSys) {
        Log("InitializePal: Failed to find Pal::System::createSystem export");
        return false;
    }

    Log("InitializePal: Initializing persistent Pal::System via driver stack...");
    createSys(g_PalSysStorage, &g_PalSysDelegate);
    g_PalInitialized = true;
    return true;
}

bool ReclockViaFocusritePal() {
    if (!InitializePal()) {
        return false;
    }

    // Wait up to 3000ms for device enumeration if not already cached
    auto deadline = GetTickCount64() + 3000;
    while (!g_PalSysDelegate.ready && GetTickCount64() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!g_PalSysDelegate.ready || !g_PalSysDelegate.detectedDevicePtr) {
        Log("ReclockViaFocusritePal: No Scarlett device detected within timeout");
        return false;
    }

    void* dev = g_PalSysDelegate.detectedDevicePtr;
    void** vtable = *(void***)dev;

    // Attach device delegate once per device pointer
    SetDelegateFn setDelegate = (SetDelegateFn)vtable[1];
    setDelegate(dev, &g_PalDevDelegate);

    GetRateFn getRate = (GetRateFn)vtable[9];
    SetRateFn setRate = (SetRateFn)vtable[8];

    uint32_t currentRate = getRate(dev);
    Log("ReclockViaFocusritePal: Current Hardware Sample Rate: %u Hz", currentRate);

    uint32_t targetRate = (currentRate == 44100) ? 48000 : 44100;
    if (currentRate == 0) targetRate = 48000;

    Log("ReclockViaFocusritePal: Flipping sample rate %u -> %u...", currentRate, targetRate);
    setRate(dev, targetRate);

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    uint32_t midRate = getRate(dev);
    Log("ReclockViaFocusritePal: Interim Sample Rate: %u Hz", midRate);

    Log("ReclockViaFocusritePal: Flipping sample rate back %u -> %u...", targetRate, (currentRate == 0 ? 44100 : currentRate));
    setRate(dev, (currentRate == 0 ? 44100 : currentRate));

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    uint32_t finalRate = getRate(dev);
    Log("ReclockViaFocusritePal: Final Sample Rate: %u Hz (PLL re-locked successfully)", finalRate);

    return true;
}
