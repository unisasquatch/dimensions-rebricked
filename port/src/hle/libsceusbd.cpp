// libSceUsbd: Sony's libusb-1.0 wrapper. The only device on the bus is the virtual Toy Pad.
// Structures follow libusb's layouts, since the game reads transfer fields directly.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "hle/hle.h"
#include "hle/toypad.h"
#include "hle/toypad_usb.h"

namespace {

using namespace std::chrono;

constexpr uint16_t kToyPadVendor = 0x0E6F, kToyPadProduct = 0x0241;
constexpr uint8_t kEndpointIn = 0x81, kEndpointOut = 0x01;
constexpr uint16_t kPacketSize = 32;

constexpr uint8_t kTransferTypeInterrupt = 3;
constexpr int kTransferCompleted = 0, kTransferTimedOut = 2;

struct Device { int unused; };
struct DeviceHandle { Device* device; };
Device g_toypad;
DeviceHandle g_toypad_handle{&g_toypad};

#pragma pack(push, 1)
struct DeviceDescriptor {
    uint8_t length, type;
    uint16_t bcd_usb;
    uint8_t device_class, device_subclass, device_protocol, max_packet_size0;
    uint16_t vendor, product, bcd_device;
    uint8_t manufacturer, product_string, serial, num_configurations;
};
#pragma pack(pop)
static_assert(sizeof(DeviceDescriptor) == 18);

struct EndpointDescriptor {
    uint8_t length, type, address, attributes;
    uint16_t max_packet_size;
    uint8_t interval, refresh, synch_address;
    const uint8_t* extra;
    int extra_length;
};
static_assert(sizeof(EndpointDescriptor) == 32);
struct InterfaceDescriptor {
    uint8_t length, type, number, alternate, num_endpoints, iface_class, iface_subclass, iface_protocol, iface_string;
    const EndpointDescriptor* endpoints;
    const uint8_t* extra;
    int extra_length;
};
static_assert(sizeof(InterfaceDescriptor) == 40);
struct Interface {
    const InterfaceDescriptor* altsettings;
    int num_altsettings;
};
struct ConfigDescriptor {
    uint8_t length, type;
    uint16_t total_length;
    uint8_t num_interfaces, value, config_string, attributes, max_power;
    const Interface* interfaces;
    const uint8_t* extra;
    int extra_length;
};
static_assert(sizeof(ConfigDescriptor) == 40);

struct Transfer;
using TransferCallback = SYSV void (*)(Transfer*);
struct Transfer {
    DeviceHandle* handle;
    uint8_t flags, endpoint, type;
    uint32_t timeout;  // ms, 0 = wait forever
    int status;
    int length;
    int actual_length;
    TransferCallback callback;
    void* user_data;
    uint8_t* buffer;
    int num_iso_packets;
    // iso packet descriptors follow (unused by the Toy Pad)
};
static_assert(offsetof(Transfer, status) == 16 && offsetof(Transfer, callback) == 32 && offsetof(Transfer, buffer) == 48);

const EndpointDescriptor kEndpoints[2] = {
    {7, 5, kEndpointIn, kTransferTypeInterrupt, kPacketSize, 1, 0, 0, nullptr, 0},
    {7, 5, kEndpointOut, kTransferTypeInterrupt, kPacketSize, 1, 0, 0, nullptr, 0},
};
const InterfaceDescriptor kInterface = {9, 4, 0, 0, 2, 3 /* HID */, 0, 0, 0, kEndpoints, nullptr, 0};
const Interface kInterfaces[1] = {{&kInterface, 1}};
const ConfigDescriptor kConfig = {9, 2, 0x29, 1, 1, 0, 0x80, 0xFA, kInterfaces, nullptr, 0};

struct Pending {
    Transfer* transfer;
    steady_clock::time_point deadline;
};
std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<Pending> g_pending;
bool g_submitted = false;  // a transfer arrived since HandleEvents last looked

SYSV int Init() { return 0; }
SYSV void Exit() {}

SYSV int64_t GetDeviceList(Device*** list) {
    *list = static_cast<Device**>(std::calloc(2, sizeof(Device*)));
    (*list)[0] = &g_toypad;
    return 1;
}
SYSV void FreeDeviceList(Device** list, int) { std::free(list); }
SYSV int GetDeviceDescriptor(Device*, DeviceDescriptor* d) {
    *d = {18, 1, 0x0200, 0, 0, 0, 0x20, kToyPadVendor, kToyPadProduct, 0x0200, 1, 2, 3, 1};
    return 0;
}
SYSV int GetConfigDescriptor(Device*, uint8_t, const ConfigDescriptor** out) { *out = &kConfig; return 0; }
SYSV void FreeConfigDescriptor(const ConfigDescriptor*) {}
SYSV uint8_t GetBusNumber(Device*) { return 1; }
SYSV uint8_t GetDeviceAddress(Device*) { return 2; }
SYSV int GetMaxPacketSize(Device*, uint8_t) { return kPacketSize; }

SYSV DeviceHandle* OpenDeviceWithVidPid(uint16_t vendor, uint16_t product) {
    return (vendor == kToyPadVendor && product == kToyPadProduct) ? &g_toypad_handle : nullptr;
}
SYSV int Ok(DeviceHandle*) { return 0; }
SYSV int OkInterface(DeviceHandle*, int) { return 0; }
// The PS4 HID driver owns the pad until the game detaches it; the game insists on seeing 1.
SYSV int KernelDriverActive(DeviceHandle*, int) { return 1; }

SYSV Transfer* AllocTransfer(int iso_packets) {
    return static_cast<Transfer*>(std::calloc(1, sizeof(Transfer) + size_t(iso_packets) * 12));
}
SYSV void FillInterruptTransfer(Transfer* t, DeviceHandle* h, uint8_t endpoint, uint8_t* buffer, int length,
                                TransferCallback callback, void* user_data, uint32_t timeout) {
    t->handle = h;
    t->endpoint = endpoint;
    t->type = kTransferTypeInterrupt;
    t->timeout = timeout;
    t->buffer = buffer;
    t->length = length;
    t->callback = callback;
    t->user_data = user_data;
}
// LDPORT_USB_LOG=1: logs every completed transfer (direction, length, bytes).
void LogTransfer(const char* dir, const uint8_t* b, int n, int status) {
    static const bool on = std::getenv("LDPORT_USB_LOG") != nullptr;
    if (!on) return;
    static int logged = 0;
    if (++logged > 400) return;
    std::fprintf(stderr, "[usb] %s status %d len %d:", dir, status, n);
    for (int i = 0; i < n && i < 32; ++i) std::fprintf(stderr, " %02x", b[i]);
    std::fprintf(stderr, "\n");
}

SYSV int SubmitTransfer(Transfer* t) {
    {
        std::lock_guard lock(g_mutex);
        const auto deadline = t->timeout ? steady_clock::now() + milliseconds(t->timeout) : steady_clock::time_point::max();
        g_pending.push_back({t, deadline});
        g_submitted = true;
    }
    g_cv.notify_all();
    return 0;
}

// Completes whatever transfers the device can satisfy and runs their callbacks on this thread.
SYSV int HandleEvents() {
    std::vector<Transfer*> done;
    {
        std::unique_lock lock(g_mutex);
        // Sleep (with the lock released) until the game submits something, or 2 ms for the pad to have data. The game
        // calls this in a loop with an IN transfer always pending: waiting only for "anything pending" made it spin on
        // g_mutex, and the unfair lock then starved GameMain's SubmitTransfer for up to ~200 ms (visible stutters).
        g_cv.wait_for(lock, milliseconds(2), [] { return g_submitted; });
        g_submitted = false;
        const auto now = steady_clock::now();
        auto& pad = toypad::Instance();
        // A physical pad plugged in joins the virtual one: the game sees one pad with both pads' figures.
        toypad::RealToyPad* real = toypad::RealToyPad::Get();
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            Transfer* t = it->transfer;
            bool finished = false;
            if (t->endpoint == kEndpointOut) {
                if (real) pad.WriteMerged(t->buffer, *real);
                else pad.Write(t->buffer);
                t->actual_length = t->length;
                t->status = kTransferCompleted;
                finished = true;
            } else if (auto packet = real ? pad.ReadMerged(*real) : pad.Read()) {
                const int n = std::min<int>(t->length, int(packet->size()));
                std::memcpy(t->buffer, packet->data(), n);
                t->actual_length = n;
                t->status = kTransferCompleted;
                finished = true;
            } else if (now >= it->deadline) {
                t->actual_length = 0;
                t->status = kTransferTimedOut;
                finished = true;
            }
            if (finished) {
                done.push_back(t);
                it = g_pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Transfer* t : done) {
        LogTransfer(t->endpoint == kEndpointOut ? "out" : "in ", t->buffer, t->endpoint == kEndpointOut ? t->length : t->actual_length,
                    t->status);
        t->callback(t);
    }
    return 0;
}

}  // namespace

#define USBD(name, fn) HLE_EXPORT("libSceUsbd", name, reinterpret_cast<const void*>(&fn))
USBD("sceUsbdInit", Init);
USBD("sceUsbdExit", Exit);
USBD("sceUsbdGetDeviceList", GetDeviceList);
USBD("sceUsbdFreeDeviceList", FreeDeviceList);
USBD("sceUsbdGetDeviceDescriptor", GetDeviceDescriptor);
USBD("sceUsbdGetConfigDescriptor", GetConfigDescriptor);
USBD("sceUsbdFreeConfigDescriptor", FreeConfigDescriptor);
USBD("sceUsbdGetBusNumber", GetBusNumber);
USBD("sceUsbdGetDeviceAddress", GetDeviceAddress);
USBD("sceUsbdGetMaxPacketSize", GetMaxPacketSize);
USBD("sceUsbdOpenDeviceWithVidPid", OpenDeviceWithVidPid);
USBD("sceUsbdCheckConnected", Ok);
USBD("sceUsbdResetDevice", Ok);
USBD("sceUsbdKernelDriverActive", KernelDriverActive);
USBD("sceUsbdDetachKernelDriver", OkInterface);
USBD("sceUsbdClaimInterface", OkInterface);
USBD("sceUsbdAllocTransfer", AllocTransfer);
USBD("sceUsbdFillInterruptTransfer", FillInterruptTransfer);
USBD("sceUsbdSubmitTransfer", SubmitTransfer);
USBD("sceUsbdHandleEvents", HandleEvents);
