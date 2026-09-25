/* Scheduled ISO playback for the Numark NS6 using Apple's IOUSBLib. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VID 0x15e4
#define PID 0x0079
#define PACKETS 32
#define SLOTS 8
#define MAX_PACKET 156
#define FRAME_BYTES 12
#define RATE 44100

struct slot { IOUSBLowLatencyIsocFrame *frames; unsigned char *data; uint64_t start; };
static IOUSBInterfaceInterface **g_interface;
static struct slot g_slots[SLOTS];
static uint64_t g_next_microframe, g_audio_frame;
static unsigned g_acc, g_ok, g_err, g_late;
static volatile sig_atomic_t g_running = 1;

static void stop(int unused) { (void)unused; g_running = 0; }
static int prop16(io_registry_entry_t e, CFStringRef key, UInt16 *out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    int ok = v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberSInt16Type, out);
    if (v) CFRelease(v); return ok;
}
static IOUSBInterfaceInterface **open_ns6(void) {
    io_iterator_t it = IO_OBJECT_NULL; io_service_t s; kern_return_t kr;
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &it);
    if (kr != KERN_SUCCESS) return NULL;
    while ((s = IOIteratorNext(it))) {
        io_registry_entry_t p = IO_OBJECT_NULL; UInt16 v = 0, d = 0; UInt8 n = 255;
        if (IORegistryEntryGetParentEntry(s, kIOServicePlane, &p) || !prop16(p, CFSTR(kUSBVendorID), &v) || !prop16(p, CFSTR(kUSBProductID), &d) || v != VID || d != PID) { if (p) IOObjectRelease(p); IOObjectRelease(s); continue; }
        IOObjectRelease(p); IOCFPlugInInterface **plug = NULL; SInt32 score = 0; IOUSBInterfaceInterface **in = NULL;
        HRESULT hr = IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        if (hr == kIOReturnSuccess && plug) { hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID)&in); IODestroyPlugInInterface(plug); }
        IOObjectRelease(s); if (hr || !in) continue;
        (*in)->GetInterfaceNumber(in, &n); if (n == 0) { IOObjectRelease(it); return in; }
        (*in)->Release(in);
    }
    IOObjectRelease(it); return NULL;
}
static void ctl(UInt8 type, UInt8 request, UInt16 value, UInt16 index, void *data, UInt16 len) {
    IOUSBDevRequest r = { type, request, value, index, len, data, 0 };
    IOReturn e = (*g_interface)->ControlRequest(g_interface, 0, &r);
    if (e) fprintf(stderr, "control %02x failed: 0x%x\\n", request, e);
}
static void fill(struct slot *s) {
    unsigned char *p = s->data;
    for (int i = 0; i < PACKETS; ++i) {
        g_acc += RATE; unsigned frames = g_acc / 8000; g_acc %= 8000;
        s->frames[i].frReqCount = frames * FRAME_BYTES;
        for (unsigned f = 0; f < frames; ++f, ++g_audio_frame) for (int ch = 0; ch < 4; ++ch) {
            int32_t sample = (int32_t)(sin(2.0 * M_PI * 440.0 * g_audio_frame / RATE) * 0x180000);
            *p++ = sample & 255; *p++ = (sample >> 8) & 255; *p++ = (sample >> 16) & 255;
        }
    }
}
/* C callbacks cannot use blocks; submit with this completion trampoline. */
static void submit_real(struct slot *s);
static void complete(void *ref, IOReturn status, void *arg) {
    (void)arg; struct slot *s = ref;
    if (status == kIOReturnSuccess) ++g_ok; else { ++g_err; if (status == kIOReturnIsoTooOld) ++g_late; }
    if (g_running) submit_real(s);
}
static void submit_real(struct slot *s) {
    UInt64 now = 0; AbsoluteTime at;
    if ((*g_interface)->GetBusMicroFrameNumber(g_interface, &now, &at) != kIOReturnSuccess) { ++g_err; return; }
    if (g_next_microframe < now + 50) g_next_microframe = now + 50;
    s->start = 0; fill(s);
    IOReturn e = (*g_interface)->LowLatencyWriteIsochPipeAsync(g_interface, 1, s->data, s->start, PACKETS, 1, s->frames, complete, s);
    if (e) { ++g_err; if (e == kIOReturnIsoTooOld) ++g_late; fprintf(stderr, "ISO submit 0x%x\\n", e); }
}
int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 30; unsigned char cap[8] = {0}, rate[3] = {0x44,0xac,0};
    g_interface = open_ns6(); if (!g_interface) { fputs("NS6 interface 0 unavailable\\n", stderr); return 1; }
    IOReturn e = (*g_interface)->USBInterfaceOpenSeize(g_interface); if (e) { fprintf(stderr,"open 0x%x\\n",e); return 1; }
    CFRunLoopSourceRef source = NULL; (*g_interface)->CreateInterfaceAsyncEventSource(g_interface, &source); CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    if ((e = (*g_interface)->SetAlternateInterface(g_interface, 1))) { fprintf(stderr,"alt 0/1 0x%x\\n",e); return 1; }
    ctl(0xc0,86,0,0,cap,sizeof(cap)); if (cap[0]) ctl(0xc0,86,0,0,cap,cap[0]);
    ctl(0x22,1,0x0100,134,rate,3); ctl(0x22,1,0x0100,2,rate,3); ctl(0x40,73,0x0032,0,NULL,0);
    for (int i=0;i<SLOTS;++i) {
        e = (*g_interface)->LowLatencyCreateBuffer(g_interface, (void **)&g_slots[i].data, PACKETS * MAX_PACKET, kUSBLowLatencyWriteBuffer);
        if (e) { fprintf(stderr, "audio buffer 0x%x\\n", e); return 1; }
        e = (*g_interface)->LowLatencyCreateBuffer(g_interface, (void **)&g_slots[i].frames, sizeof(*g_slots[i].frames) * PACKETS, kUSBLowLatencyFrameListBuffer);
        if (e) { fprintf(stderr, "frame buffer 0x%x\\n", e); return 1; }
        submit_real(&g_slots[i]);
    }
    signal(SIGINT, stop); time_t end = time(NULL) + seconds;
    while (g_running && time(NULL) < end) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    g_running = 0; (*g_interface)->AbortPipe(g_interface, 1); CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
    printf("scheduled ISO complete=%u errors=%u late=%u\\n",g_ok,g_err,g_late);
    for (int i=0;i<SLOTS;++i) { (*g_interface)->LowLatencyDestroyBuffer(g_interface, g_slots[i].data); (*g_interface)->LowLatencyDestroyBuffer(g_interface, g_slots[i].frames); }
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(),source,kCFRunLoopDefaultMode); CFRelease(source); (*g_interface)->USBInterfaceClose(g_interface); (*g_interface)->Release(g_interface); return 0;
}
