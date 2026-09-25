/* Read-only descriptor probe using libusb's macOS IOUSBHost backend. */
#include <libusb.h>
#include <stdio.h>

#define NS6_VENDOR  0x15e4
#define NS6_PRODUCT 0x0079

static const char *transfer_type(uint8_t attributes)
{
    switch (attributes & LIBUSB_TRANSFER_TYPE_MASK) {
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "iso";
    case LIBUSB_TRANSFER_TYPE_BULK:        return "bulk";
    case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "interrupt";
    default:                               return "control";
    }
}

int main(void)
{
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    ssize_t count;
    int rc = libusb_init(&context);
    if (rc < 0) {
        fprintf(stderr, "libusb_init: %s\\n", libusb_error_name(rc));
        return 1;
    }

    count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        fprintf(stderr, "libusb_get_device_list: %s\\n", libusb_error_name((int)count));
        libusb_exit(context);
        return 1;
    }
    printf("libusb enumerated %zd device(s)\\n", count);
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor device;
        libusb_get_device_descriptor(devices[i], &device);
        printf("  %04x:%04x bus %u address %u\\n", device.idVendor, device.idProduct,
               libusb_get_bus_number(devices[i]), libusb_get_device_address(devices[i]));
        if (device.idVendor != NS6_VENDOR || device.idProduct != NS6_PRODUCT)
            continue;

        libusb_device_handle *handle = NULL;
        printf("NS6 %04x:%04x: bus %u address %u, USB %x.%02x\\n",
               device.idVendor, device.idProduct, libusb_get_bus_number(devices[i]),
               libusb_get_device_address(devices[i]), device.bcdUSB >> 8,
               device.bcdUSB & 0xff);
        rc = libusb_open(devices[i], &handle);
        if (rc < 0)
            fprintf(stderr, "cannot open NS6: %s\\n", libusb_error_name(rc));
        else {
            unsigned char product[128] = {0};
            int length = libusb_get_string_descriptor_ascii(handle, device.iProduct,
                                                             product, sizeof(product));
            if (length >= 0)
                printf("product string: %.*s\\n", length, product);
            libusb_close(handle);
        }
        for (uint8_t cfg_index = 0; cfg_index < device.bNumConfigurations; ++cfg_index) {
            struct libusb_config_descriptor *cfg = NULL;
            rc = libusb_get_config_descriptor(devices[i], cfg_index, &cfg);
            if (rc < 0) {
                fprintf(stderr, "config %u: %s\\n", cfg_index, libusb_error_name(rc));
                continue;
            }
            printf("configuration %u: %u interfaces\\n", cfg->bConfigurationValue,
                   cfg->bNumInterfaces);
            for (uint8_t iface = 0; iface < cfg->bNumInterfaces; ++iface) {
                const struct libusb_interface *interface = &cfg->interface[iface];
                for (int alt = 0; alt < interface->num_altsetting; ++alt) {
                    const struct libusb_interface_descriptor *setting =
                        &interface->altsetting[alt];
                    printf("interface %u alt %u: class %02x/%02x/%02x, endpoints %u\\n",
                           setting->bInterfaceNumber, setting->bAlternateSetting,
                           setting->bInterfaceClass, setting->bInterfaceSubClass,
                           setting->bInterfaceProtocol, setting->bNumEndpoints);
                    for (uint8_t ep = 0; ep < setting->bNumEndpoints; ++ep) {
                        const struct libusb_endpoint_descriptor *endpoint =
                            &setting->endpoint[ep];
                        printf("  endpoint 0x%02x: %s %s, max packet %u, interval %u\\n",
                               endpoint->bEndpointAddress,
                               endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN ? "IN" : "OUT",
                               transfer_type(endpoint->bmAttributes),
                               endpoint->wMaxPacketSize & 0x7ff, endpoint->bInterval);
                    }
                }
            }
            libusb_free_config_descriptor(cfg);
        }
        libusb_free_device_list(devices, 1);
        libusb_exit(context);
        return 0;
    }
    fprintf(stderr, "NS6 %04x:%04x not found\\n", NS6_VENDOR, NS6_PRODUCT);
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return 1;
}
