# alfa-driver — RTL8812AU native on macOS Apple Silicon (userspace)

Monitor mode + packet injection with the **Alfa AWUS036ACH (RTL8812AU)**
**natively on macOS (Apple Silicon, M-series)** — no VM, no Linux, no kext,
no DriverKit. Pure userspace over libusb.

> For authorized WLAN auditing, CTFs, and security research on networks you own
> or have permission to test.

## Why userspace instead of a driver

On macOS Apple Silicon there is **no** viable kernel-driver path for a USB Wi-Fi
device:

- Kexts are effectively gone; the `IO80211` KPIs are private/undocumented.
- DriverKit offers `USBDriverKit` and (Ethernet-only) `NetworkingDriverKit`, but
  **no 802.11/Wi-Fi family**. Apple's Wi-Fi stack is private.

Monitor mode and injection do not need that OS integration. On the RTL8812AU they
are **chip functions**: configure registers, then send/receive raw 802.11 frames
over the USB bulk endpoints. A normal macOS userspace program does all of that
itself via **libusb**.

Upside: runs on a fully locked-down Apple Silicon Mac — no SIP disable, no reduced
security, no signing/notarization required. macOS ships no RTL8812AU driver, so
the device is free for userspace to claim.

## Architecture

```
  Alfa AWUS036ACH (RTL8812AU)
        │  USB (bulk-in = RX, bulk-out = TX, control = registers/firmware)
        ▼
  ┌────────────────────────────────────────────┐
  │  alfa-driver (arm64 macOS, libusb)          │
  │  rtl_usb   USB vendor-request register I/O   │
  │  rtl_init  power-on sequence (card enable)   │
  │  rtl_fw    firmware download                 │
  │  rtl_efuse efuse read (real MAC)             │
  │  rtl_mac   MAC init + monitor RCR            │
  │  rtl_bb    baseband/PHY init                 │
  │  rtl_rf    RF init + channel (2.4/5 GHz)     │
  │  rtl_cal   IQK/LCK calibration               │
  │  rtl_rx    bulk-IN → 802.11 + radiotap        │
  │  rtl_tx    802.11 + TX descriptor → bulk-OUT  │
  │  rtl_ccmp  AES-CCM (WPA2 data crypto)         │
  │  rtl_wpa   WPA2-PSK connect (auth/assoc/4-way)│
  └────────────────────────────────────────────┘
        │
        ▼
  Wireshark / Kismet / aircrack (pcap)   |   utun (internet client, WIP)
```

## What works (verified on hardware: M2 Pro, macOS 26.6.2)

- **Device bring-up:** claim device (no kext/SIP), register R/W, power-on,
  efuse read (real ALFA MAC), firmware download (checksum OK, firmware running).
- **MAC/BB/RF init + channel** on 2.4 and 5 GHz (tables byte-identical to the
  Linux reference).
- **Monitor mode:** captures real 802.11 traffic to pcap+radiotap. Verified on
  channel 6 and channel 100 (beacons, data, ACK/Block-ACK), decoded by tcpdump.
- **Packet injection:** verified via probe-request → probe-response to our MAC
  from the AP. Deauth and other management frames use the same path. Moderate TX
  power (index 0x12), no PA stress.
- **Wireshark integration** via extcap (`alfa-extcap`), and a **Homebrew** formula.
- **Status LED** (register `LEDCFG0`): on after init, blinks on traffic.

### Internet client (work in progress)

A userspace daemon (`alfa-netd`) that connects as a WPA2 station and bridges IP
through a `utun` interface. Verified so far: WPA2-PSK 4-way handshake (validated
against the IEEE 802.11i test vector and live), CCMP decrypt of real traffic,
association, and DHCP. Data-path bring-up (routing/ping) is under active
debugging. Note: because it uses `utun`, it appears as a tunnel, not as an
Ethernet adapter in Network settings (that would require the entitlement-gated
DriverKit path).

## Tools

| Tool | Purpose |
|------|---------|
| `usbprobe`  | find/claim the device, dump endpoints |
| `chipinfo`  | read/decode chip version |
| `efuseinfo` | read efuse → real MAC |
| `fwload`    | download firmware |
| `monitor`   | monitor-mode capture to pcap |
| `inject`    | packet injection (probe-request test) |
| `scan`      | scan 2.4/5 GHz for networks |
| `alfa-extcap` | Wireshark extcap capture source |
| `connect`   | WPA2-PSK connect + handshake test |
| `alfa-netd` | internet-client daemon (WIP, needs sudo) |

## Build & use

```bash
make                       # builds all tools (arm64, against libusb)
./monitor 6 15 out.pcap    # monitor mode: channel 6, 15s -> pcap (Wireshark/tshark)
./inject 6 30              # injection: 30 probe requests on channel 6
./scan                     # list nearby networks
```

## Install via Homebrew

```bash
brew install --HEAD ./Formula/rtl8812au-macos.rb
# or from a tap:  brew install --HEAD janlueders/tap/rtl8812au-macos
```

This installs `alfa-monitor`, `alfa-inject`, `alfa-usbprobe`, `alfa-chipinfo`,
and `alfa-extcap`. For Wireshark, link the extcap helper (see the `brew` caveats):

```bash
mkdir -p ~/.config/wireshark/extcap
ln -sf "$(brew --prefix)/bin/alfa-extcap" ~/.config/wireshark/extcap/alfa-extcap
```

## License & origin

Register definitions, power-on/init sequences, tables (MAC/BB/RF), efuse decoding,
and the embedded firmware are **ported from** the Linux kernel driver
<https://github.com/aircrack-ng/rtl8812au> (originally © Realtek Corporation,
**GPL v2**). This project is a derivative work and is therefore also licensed under
**GPL v2** (see `LICENSE`). The Linux driver itself is not redistributed here; it
is used only as a porting reference.

No kext, no DriverKit, no Anthropic/Apple affiliation — pure userspace USB over
libusb.

## Honest note

This is a real reverse-engineering effort (reimplementing chip logic from the
Linux source), not a weekend project. But every milestone is independently
testable, and nothing about it is blocked on Apple Silicon — it is all userspace
USB work.
