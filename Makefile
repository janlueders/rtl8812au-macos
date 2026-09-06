# alfa-driver — Build fuer macOS Apple Silicon (arm64), Userspace via libusb.

CC      ?= clang

# libusb via Homebrew. pkg-config bevorzugt, sonst feste Homebrew-Pfade.
PKGCONF := $(shell command -v pkg-config 2>/dev/null)
ifeq ($(PKGCONF),)
  LIBUSB_CFLAGS := -I/opt/homebrew/include/libusb-1.0
  LIBUSB_LIBS   := -L/opt/homebrew/lib -lusb-1.0
else
  LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
  LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)
endif

CFLAGS  += -Wall -Wextra -O2 -arch arm64 $(LIBUSB_CFLAGS)
LDFLAGS += $(LIBUSB_LIBS)

BINS := usbprobe chipinfo initchip

all: $(BINS)

usbprobe: src/usbprobe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

chipinfo: src/chipinfo.c src/rtl_usb.c src/rtl_usb.h
	$(CC) $(CFLAGS) -o $@ src/chipinfo.c src/rtl_usb.c $(LDFLAGS)

initchip: src/initchip.c src/rtl_init.c src/rtl_usb.c src/rtl_usb.h
	$(CC) $(CFLAGS) -o $@ src/initchip.c src/rtl_init.c src/rtl_usb.c $(LDFLAGS)

clean:
	rm -f $(BINS)

.PHONY: all clean
