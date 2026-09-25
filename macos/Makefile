LIBUSB_CFLAGS := $(shell /opt/homebrew/bin/pkgconf --cflags libusb-1.0)
LIBUSB_LIBS := $(shell /opt/homebrew/bin/pkgconf --libs libusb-1.0)
CFLAGS := -Wall -Wextra -Werror -arch arm64 $(LIBUSB_CFLAGS)

ns6-clock-probe: ns6-clock-probe.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBUSB_LIBS) -lm

clean:
	rm -f ns6-clock-probe
