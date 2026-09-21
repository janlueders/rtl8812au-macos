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

# Alle Header als Prerequisite: sonst rebuildet make nach einer reinen
# Header-Aenderung nicht (es gibt keine .o-Zwischenstufe).
HDRS := $(wildcard src/*.h)

# Rezept: nur die .c-Dateien an den Compiler, Header rausfiltern.
BUILD = $(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDFLAGS)

# Gemeinsame Chip-Module (Bring-up bis Monitor, inkl. ops-Tabelle und TX).
CORE := src/rtl_usb.c src/rtl_init.c src/rtl_fw.c src/fw_8812a_nic.c \
        src/rtl_efuse.c src/rtl_led.c src/rtl_mac.c src/rtl_bb.c \
        src/rtl_rf.c src/rtl_cal.c src/rtl_txpwr.c \
        src/rtl_tx.c src/rtl_chip.c

# Bring-up + Empfangspfad (alles, was rtl_hal_full_init nutzt).
HAL := src/rtl_hal.c src/rtl_rx.c $(CORE)

BINS := usbprobe chipinfo initchip efuseinfo fwload ledtest stagetest \
        monitor inject ledscan alfa-extcap scan associate connect \
        ccmptest alfa-netd deauth

all: $(BINS)

# --- Einzeltools ---

usbprobe: src/usbprobe.c $(HDRS)
	$(BUILD)

chipinfo: src/chipinfo.c src/rtl_usb.c $(HDRS)
	$(BUILD)

initchip: src/initchip.c src/rtl_init.c src/rtl_usb.c $(HDRS)
	$(BUILD)

efuseinfo: src/efuseinfo.c src/rtl_efuse.c src/rtl_init.c src/rtl_usb.c $(HDRS)
	$(BUILD)

fwload: src/fwload.c src/rtl_fw.c src/fw_8812a_nic.c src/rtl_init.c src/rtl_usb.c $(HDRS)
	$(BUILD)

ledtest: src/ledtest.c src/rtl_led.c src/rtl_fw.c src/fw_8812a_nic.c src/rtl_init.c src/rtl_usb.c $(HDRS)
	$(BUILD)

ccmptest: src/ccmptest.c src/rtl_ccmp.c $(HDRS)
	$(BUILD)

stagetest: src/stagetest.c $(CORE) $(HDRS)
	$(BUILD)

# --- Tools mit vollem Bring-up + RX-Pfad (rtl_tx.c steckt in CORE!) ---

monitor: src/monitor.c $(HAL) $(HDRS)
	$(BUILD)

inject: src/inject.c $(HAL) $(HDRS)
	$(BUILD)

ledscan: src/ledscan.c $(HAL) $(HDRS)
	$(BUILD)

alfa-extcap: src/alfa_extcap.c $(HAL) $(HDRS)
	$(BUILD)

scan: src/scan.c $(HAL) $(HDRS)
	$(BUILD)

associate: src/associate.c $(HAL) $(HDRS)
	$(BUILD)

connect: src/connect.c src/rtl_ccmp.c $(HAL) $(HDRS)
	$(BUILD)

alfa-netd: src/alfa_netd.c src/rtl_wpa.c src/rtl_ccmp.c $(HAL) $(HDRS)
	$(BUILD)

deauth: src/deauth.c $(HAL) $(HDRS)
	$(BUILD)

clean:
	rm -f $(BINS)

.PHONY: all clean
