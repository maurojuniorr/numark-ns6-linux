/* Verify that the legacy IOUSBLib user client can schedule NS6 ISO packets. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>

#define NS6_VENDOR 0x15e4
#define NS6_PRODUCT 0x0079

static int property_u16(io_registry_entry_t entry, CFStringRef key, UInt16 *out)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!value)
        return 0;
    int ok = CFGetTypeID(value) == CFNumberGetTypeID() &&
             CFNumberGetValue(value, kCFNumberSInt16Type, out);
    CFRelease(value);
    return ok;
}

int main(void)
{
    io_iterator_t it = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("IOUSBHostInterface"), &it);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "interface search: 0x%x\\n", kr);
        return 1;
    }
    io_service_t service;
    while ((service = IOIteratorNext(it))) {
        io_registry_entry_t parent = IO_OBJECT_NULL;
        UInt16 vendor = 0, product = 0;
        kr = IORegistryEntryGetParentEntry(service, kIOServicePlane, &parent);
        if (kr != KERN_SUCCESS || !property_u16(parent, CFSTR(kUSBVendorID), &vendor) ||
            !property_u16(parent, CFSTR(kUSBProductID), &product) ||
            vendor != NS6_VENDOR || product != NS6_PRODUCT) {
            if (parent) IOObjectRelease(parent);
            IOObjectRelease(service);
            continue;
        }
        IOObjectRelease(parent);
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        IOUSBInterfaceInterface **interface = NULL;
        HRESULT hr;
        kr = IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID,
                                                kIOCFPlugInInterfaceID, &plugin, &score);
        if (kr != kIOReturnSuccess || !plugin) {
            fprintf(stderr, "create interface user client: 0x%x\\n", kr);
            IOObjectRelease(service);
            continue;
        }
        hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
                                       (LPVOID)&interface);
        IODestroyPlugInInterface(plugin);
        if (hr || !interface) {
            fprintf(stderr, "query interface: 0x%x\\n", (unsigned)hr);
            IOObjectRelease(service);
            continue;
        }
        UInt8 number = 0;
        (*interface)->GetInterfaceNumber(interface, &number);
        printf("NS6 interface %u found\\n", number);
        if (number == 0) {
            kr = (*interface)->USBInterfaceOpen(interface);
            printf("open: 0x%x\\n", kr);
            if (kr == kIOReturnSuccess) {
                CFRunLoopSourceRef source = NULL;
                kr = (*interface)->CreateInterfaceAsyncEventSource(interface, &source);
                printf("async source: 0x%x\\n", kr);
                if (source) CFRelease(source);
                kr = (*interface)->SetAlternateInterface(interface, 1);
                printf("alt 1: 0x%x\\n", kr);
                UInt64 microframe = 0;
                AbsoluteTime when;
                kr = (*interface)->GetBusMicroFrameNumber(interface, &microframe, &when);
                printf("microframe: 0x%x %llu\\n", kr, microframe);
                (*interface)->USBInterfaceClose(interface);
            }
            (*interface)->Release(interface);
            IOObjectRelease(service);
            IOObjectRelease(it);
            return 0;
        }
        (*interface)->Release(interface);
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    fprintf(stderr, "NS6 interface 0 not usable\\n");
    return 1;
}
