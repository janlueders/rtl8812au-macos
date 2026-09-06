# alfa-driver — RTL8812AU nativ auf macOS Apple Silicon (Userspace)

Ziel: Monitor Mode + Packet Injection mit dem **Alfa AWUS036ACH (RTL8812AU)**
**nativ auf macOS (Apple Silicon, M-Prozessor)** — ohne VM, ohne Linux, ohne Kext,
ohne DriverKit.

## Warum Userspace statt Treiber

Auf macOS Apple Silicon gibt es **keinen** gangbaren Kernel-Treiber-Pfad für ein
USB-WLAN-Gerät:

- Kexts sind faktisch abgeschafft; die `IO80211`-KPIs sind nicht öffentlich.
- DriverKit hat `USBDriverKit` und (nur Ethernet-artiges) `NetworkingDriverKit`,
  aber **keine 802.11/WLAN-Familie**. Apples WLAN-Stack ist privat.

Monitor Mode und Injection brauchen diese OS-Integration aber nicht. Beim
RTL8812AU sind das **Chip-Funktionen**: Register konfigurieren, dann rohe
802.11-Frames über die USB-Bulk-Endpoints empfangen/senden. Das macht ein
normales macOS-Userspace-Programm über **libusb** vollständig selbst.

Vorteil: läuft auf einem komplett gesperrten Apple-Silicon-Mac. Keine
SIP-Abschaltung, keine Reduced Security, keine Signatur/Notarisierung nötig.
macOS bringt keinen RTL8812AU-Treiber mit → das Gerät ist frei und greifbar.

## Architektur

```
  Alfa AWUS036ACH (RTL8812AU)
        │  USB (bulk-in = RX, bulk-out = TX, control = Register/Firmware)
        ▼
  ┌──────────────────────────────────────────┐
  │  alfa-driver (arm64 macOS, libusb)         │
  │                                            │
  │  1. usbprobe   — Gerät finden/greifen      │  ← Meilenstein 1 (DA)
  │  2. hal        — Firmware-Upload, Reg-Init │  ← aus Linux-HAL portiert
  │  3. phy/rf     — Kanal, Band (2.4/5GHz),   │
  │                  Bandbreite setzen         │
  │  4. rx         — Bulk-IN, RX-Deskriptor →  │
  │                  802.11-Frame + radiotap   │
  │  5. tx/inject  — 802.11-Frame + TX-Desk. → │
  │                  Bulk-OUT                   │
  │  6. output     — Wireshark extcap / pcap   │
  └──────────────────────────────────────────┘
        │
        ▼
  Wireshark / Kismet / aircrack (via pcap)
```

## Roadmap (Meilensteine)

- [x] **M0 — Fundament:** arm64, Homebrew, libusb 1.0.29 vorhanden. Projekt-Setup.
- [x] **M1 — Sehen & Greifen:** Gerät `0bda:8812` enumeriert, Interface 0 geclaimt
      (ohne Kext/SIP-Eingriff), Register-Zugriff steht. → `usbprobe`, `chipinfo`
- [x] **M2 — Register & Power-On:** Register-R/W verifiziert; Power-On-Sequenz
      (CARDEMU_TO_ACT) läuft, State-Machine aktiv. → `initchip`
- [x] **M2b — efuse-Read:** physische efuse dekodiert, echte Hersteller-MAC
      gelesen (`00:c0:ca:...`, OUI = ALFA Network). → `efuseinfo`
- [x] **M2c — Firmware-Download:** 27 KB NIC-Firmware (v52.14) geladen,
      Checksum OK, WINTINI_RDY gesetzt — Firmware laeuft. → `fwload`
- [ ] **M3 — PHY/BB/RF-Init + Kanal:** MAC-/BB-/RF-Register-Tabellen +
      RF-Kalibrierung (IQK) portieren, Kanal/Band/Bandbreite setzen.
      **Groesster Brocken** (viele Parameter-Tabellen, HW-Kalibrierung).
- [ ] **M4 — RX/Monitor:** Bulk-IN, RX-Deskriptor → 802.11 + radiotap.
- [ ] **M5 — TX/Injection:** rohe Frames + TX-Deskriptor über Bulk-OUT.
- [ ] **M6 — Wireshark extcap:** als Capture-Quelle einbinden.

### Verifizierter Hardware-Stand (auf M2 Pro, macOS 26.6.2)

- Chip antwortet nativ auf USB-Vendor-Requests aus Userspace, ohne Kext/SIP.
- `SYS_CFG (0x00F0) = 0x04411137` → MP-Chip, TSMC, Cut-Version 1.
- Interface-Claim erfolgreich, kein Kernel-Treiber im Weg.
- **Power-On erfolgreich:** Power-Ready (0x04[17]=1) und MAC-on (0x04[8]=0)
  beide durchgepollt → Chip-State-Machine reagiert auf unsere Writes.
- MAC (0x0610) nach Power-On = `00:00:...` (Registerfile), echte MAC via efuse.
- **efuse-Read erfolgreich:** MAC `00:c0:ca:bc:4e:fa` — OUI `00:c0:ca` = ALFA
  Network Inc., bestätigt die korrekte Map-Dekodierung.
- **Firmware-Download erfolgreich:** v52.14, 26998 Byte, Checksum OK,
  WINTINI_RDY gesetzt → Firmware bootet nativ von macOS aus.
- [ ] **M3 — Kanal/RF:** Kanal + Band + Bandbreite setzen (Register aus HAL).
- [ ] **M4 — RX/Monitor:** Bulk-IN empfangen, RX-Deskriptor parsen, rohe
      802.11-Frames + radiotap-Header ausgeben.
- [ ] **M5 — TX/Injection:** rohe Frames mit TX-Deskriptor über Bulk-OUT senden.
- [ ] **M6 — Wireshark extcap:** als Capture-Quelle in Wireshark einbinden.

## Referenz-Quelle

Die RTL8812AU-Register-/Firmware-Sequenzen gibt es nur im Linux-Treiber (kein
öffentliches Datenblatt). Wir portieren die **Logik** aus:
<https://github.com/aircrack-ng/rtl8812au> — insbesondere `hal/`, `core/`, die
`usb_ops`-Abstraktion und die Firmware unter `hal/rtl8812a/`.

## Bauen

```bash
make            # baut ./usbprobe
./usbprobe      # Adapter einstecken, dann ausführen
```

## Ehrliche Einordnung

Das ist ein echtes Reverse-Engineering-Projekt (Logik aus dem Linux-Quelltext
nachbauen), kein Nachmittagsprojekt. Aber jeder Meilenstein ist einzeln testbar,
und M1 läuft sofort. Nichts daran ist auf Apple Silicon blockiert — es ist reine
Userspace-USB-Arbeit.
