# alfa-driver — Build fuer macOS Apple Silicon (arm64), Userspace via libusb.

CC      ?= clang

PKGCONF := $(shell command -v pkg-config 2>/dev/null)
ifeq ($(PKGCONF),)
  LIBUSB_CFLAGS := -I/opt/homebrew/include/libusb-1.0
  LIBUSB_LIBS   := -L/opt/homebrew/lib -lusb-1.0
else
  LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
  LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)
endif

CFLAGS  += -Wall -Wextra -O2 -arch arm64 $(LIBUSB_CFLAGS) -Isrc
LDFLAGS += $(LIBUSB_LIBS)

# Gemeinsame Chip-Module (Bring-up bis Monitor).
CORE := src/rtl_usb.c src/rtl_init.c src/rtl_fw.c src/fw_8812a_nic.c \
        src/rtl_efuse.c src/rtl_led.c src/rtl_mac.c src/rtl_bb.c \
        src/rtl_rf.c src/rtl_cal.c

BINS := usbprobe chipinfo initchip efuseinfo fwload ledtest stagetest monitor inject ledscan alfa-extcap scan associate

all: $(BINS)

usbprobe: src/usbprobe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

chipinfo: src/chipinfo.c src/rtl_usb.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

initchip: src/initchip.c src/rtl_init.c src/rtl_usb.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

efuseinfo: src/efuseinfo.c src/rtl_efuse.c src/rtl_init.c src/rtl_usb.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

fwload: src/fwload.c src/rtl_fw.c src/fw_8812a_nic.c src/rtl_init.c src/rtl_usb.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

ledtest: src/ledtest.c src/rtl_led.c src/rtl_fw.c src/fw_8812a_nic.c src/rtl_init.c src/rtl_usb.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stagetest: src/stagetest.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

monitor: src/monitor.c src/rtl_hal.c src/rtl_rx.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

inject: src/inject.c src/rtl_hal.c src/rtl_rx.c src/rtl_tx.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BINS)

.PHONY: all clean

ledscan: src/ledscan.c src/rtl_hal.c src/rtl_rx.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

alfa-extcap: src/alfa_extcap.c src/rtl_hal.c src/rtl_rx.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

scan: src/scan.c src/rtl_hal.c src/rtl_rx.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

associate: src/associate.c src/rtl_hal.c src/rtl_rx.c src/rtl_tx.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
