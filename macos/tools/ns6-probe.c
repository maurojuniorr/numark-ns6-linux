/* Read-only USB descriptor probe for the Numark NS6. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <stdio.h>
#include <stdlib.h>

#define NS6_VENDOR  0x15e4
#define NS6_PRODUCT 0x0079

static unsigned le16(const UInt8 *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static void print_descriptors(const UInt8 *data, size_t length)
{
    size_t offset = 0;
    while (offset + 2 <= length) {
        unsigned size = data[offset], type = data[offset + 1];
        if (size < 2 || offset + size > length) {
            fprintf(stderr, "invalid descriptor at offset %zu\\n", offset);
            return;
        }
        if (type == kUSBInterfaceDesc && size >= 9) {
            printf("interface %u alt %u: class %02x/%02x/%02x, endpoints %u\\n",
                   data[offset + 2], data[offset + 3], data[offset + 5],
                   data[offset + 6], data[offset + 7], data[offset + 4]);
        } else if (type == kUSBEndpointDesc && size >= 7) {
            unsigned address = data[offset + 2];
            unsigned attributes = data[offset + 3] & 0x3;
            const char *kind = attributes == 1 ? "iso" : attributes == 2 ? "bulk" :
                               attributes == 3 ? "interrupt" : "control";
            printf("  endpoint 0x%02x: %s %s, max packet %u, interval %u\\n",
                   address, (address & 0x80) ? "IN" : "OUT", kind,
                   le16(data + offset + 4) & 0x7ff, data[offset + 6]);
        }
        offset += size;
    }
}

int main(void)
{
    CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBHostDeviceClassName);
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_service_t service = IO_OBJECT_NULL;
    kern_return_t kr;

    if (!matching)
        return 1;
    CFDictionarySetValue(matching, CFSTR(kUSBVendorID),
                         CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                        &(int){NS6_VENDOR}));
    CFDictionarySetValue(matching, CFSTR(kUSBProductID),
                         CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                        &(int){NS6_PRODUCT}));
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS || !(service = IOIteratorNext(iterator))) {
        fprintf(stderr, "NS6 %04x:%04x not found (0x%x)\\n", NS6_VENDOR, NS6_PRODUCT, kr);
        return 1;
    }

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOUSBDeviceInterface **device = NULL;
    HRESULT hr;
    kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                           kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != kIOReturnSuccess || !plugin) {
        fprintf(stderr, "cannot create USB user client: 0x%x\\n", kr);
        goto done;
    }
    hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                   (LPVOID)&device);
    IODestroyPlugInInterface(plugin);
    if (hr || !device) {
        fprintf(stderr, "cannot query USB device interface: 0x%x\\n", (unsigned)hr);
        goto done;
    }

    UInt8 configurations = 0, current = 0;
    (*device)->GetNumberOfConfigurations(device, &configurations);
    (*device)->GetConfiguration(device, &current);
    printf("NS6 %04x:%04x: configuration %u of %u\\n", NS6_VENDOR, NS6_PRODUCT,
           current, configurations);
    for (UInt8 i = 0; i < configurations; ++i) {
        IOUSBConfigurationDescriptorPtr descriptor = NULL;
        if ((*device)->GetConfigurationDescriptorPtr(device, i, &descriptor) == kIOReturnSuccess && descriptor) {
            printf("configuration %u: %u bytes\\n", descriptor->bConfigurationValue,
                   le16((const UInt8 *)&descriptor->wTotalLength));
            print_descriptors((const UInt8 *)descriptor,
                              le16((const UInt8 *)&descriptor->wTotalLength));
        }
    }
    (*device)->Release(device);

done:
    IOObjectRelease(service);
    IOObjectRelease(iterator);
    return device ? 0 : 1;
}
