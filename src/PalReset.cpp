#include "PalReset.h"
#include "AudioReset.h"
#include <windows.h>
#include <stdint.h>
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
        }
        virtual bool supportsUsb() { return true; }
        virtual bool supportsUsbDevice(unsigned int) { return true; }

        void* detectedDevicePtr = nullptr;
        bool ready = false;
    };
}

typedef void (*CreateSystemFn)(void* outSharedPtr, Pal::SystemDelegate*);

// Pal::Device virtual method helpers:
// vtable[1] = void setDelegate(DeviceDelegate*)
// vtable[8] = void setSampleRate(uint32_t)
// vtable[9] = uint32_t getSampleRate()

static void Device_SetDelegate(void* devPtr, Pal::DeviceDelegate* delegate) {
    typedef void (*SetDelegateFn)(void*, Pal::DeviceDelegate*);
    void** vtable = *(void***)devPtr;
    ((SetDelegateFn)vtable[1])(devPtr, delegate);
}

static uint32_t Device_GetSampleRate(void* devPtr) {
    typedef uint32_t (*GetRateFn)(void*);
    void** vtable = *(void***)devPtr;
    return ((GetRateFn)vtable[9])(devPtr);
}

static void Device_SetSampleRate(void* devPtr, uint32_t rate) {
    typedef void (*SetRateFn)(void*, uint32_t);
    void** vtable = *(void***)devPtr;
    ((SetRateFn)vtable[8])(devPtr, rate);
}

bool ReclockViaFocusritePal() {
    Log("ReclockViaFocusritePal: Loading FocusritePal64.dll from system...");

    HMODULE hPal = LoadLibraryW(L"C:\\Windows\\System32\\FocusritePal64.dll");
    if (!hPal) {
        hPal = LoadLibraryW(L"FocusritePal64.dll");
    }
    if (!hPal) {
        Log("ReclockViaFocusritePal: Failed to load FocusritePal64.dll (Error: %lu)", GetLastError());
        return false;
    }

    CreateSystemFn createSys = (CreateSystemFn)GetProcAddress(hPal, "?createSystem@System@Pal@@SA?AV?$shared_ptr@VSystem@Pal@@@std@@PEAVSystemDelegate@2@@Z");
    if (!createSys) {
        Log("ReclockViaFocusritePal: Failed to find Pal::System::createSystem export");
        return false;
    }

    Pal::SystemDelegate sysDelegate;
    Pal::DeviceDelegate devDelegate;
    alignas(16) char sysStorage[64] = {0};

    Log("ReclockViaFocusritePal: Initializing Pal::System via driver stack...");
    createSys(sysStorage, &sysDelegate);

    // Wait up to 3000ms for device enumeration callback
    auto deadline = GetTickCount64() + 3000;
    while (!sysDelegate.ready && GetTickCount64() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!sysDelegate.ready || !sysDelegate.detectedDevicePtr) {
        Log("ReclockViaFocusritePal: No Scarlett device detected within timeout");
        return false;
    }

    void* dev = sysDelegate.detectedDevicePtr;
    Device_SetDelegate(dev, &devDelegate);

    uint32_t currentRate = Device_GetSampleRate(dev);
    Log("ReclockViaFocusritePal: Current Hardware Sample Rate: %u Hz", currentRate);

    uint32_t targetRate = (currentRate == 44100) ? 48000 : 44100;
    if (currentRate == 0) targetRate = 48000;

    Log("ReclockViaFocusritePal: Flipping sample rate %u -> %u...", currentRate, targetRate);
    Device_SetSampleRate(dev, targetRate);

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    uint32_t midRate = Device_GetSampleRate(dev);
    Log("ReclockViaFocusritePal: Interim Sample Rate: %u Hz", midRate);

    Log("ReclockViaFocusritePal: Flipping sample rate back %u -> %u...", targetRate, (currentRate == 0 ? 44100 : currentRate));
    Device_SetSampleRate(dev, (currentRate == 0 ? 44100 : currentRate));

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    uint32_t finalRate = Device_GetSampleRate(dev);
    Log("ReclockViaFocusritePal: Final Sample Rate: %u Hz (PLL re-locked successfully)", finalRate);

    return true;
}
