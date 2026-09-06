# Internet-Client (regulärer Datenempfang) — Fahrplan

Ziel: den AWUS036ACH als **nutzbare Internet-WLAN-Karte** unter macOS, als Ersatz
für einen defekten internen Adapter. Frei verteilbar über Homebrew/GitHub.

## Warum NICHT DriverKit

Eine DriverKit-Networking-Extension (`NetworkingDriverKit`/`IOUserNetworkEthernet`)
könnte ein natives `en`-Interface anmelden, ist aber **nicht frei verteilbar**:
das Entitlement `com.apple.developer.driverkit.family.networking` ist restringiert
und muss von Apple pro Developer-Account freigegeben werden; ohne diese Freigabe
(im Provisioning-Profil) lädt die Extension auf fremden Macs nicht. Damit fällt
brew/GitHub-Verteilung an Nutzer aus.

## Der verteilbare Weg: Userspace-Daemon + utun

Reines Userspace-Programm (wie unser jetziges Tool), kein Entitlement, keine
Notarisierung eines dext, kein SIP-Eingriff — voll über brew/GitHub verteilbar:

```
   macOS-Netzwerkstack
        │  IP-Pakete
        ▼
   utun-Interface (aus Userspace als root angelegt, wie VPNs)
        │
   alfa-netd (Daemon)
        │  802.11-Datenframes (CCMP) über libusb
        ▼
   AWUS036ACH  ── AP
```

Der Daemon: libusb-Chipsteuerung (haben wir), Managed-Mode, Scan (haben wir),
Assoziierung, WPA2-Supplicant (EAPOL-4-Way), Schlüssel setzen, 802.11↔IP über utun.

## Meilensteine

- [x] **IE1 — Scan:** Netze finden (SSID/BSSID/Kanal/Verschlüsselung). → `scan`
- [x] **IE2 — Assoziierung:** Open-Auth + Assoc mit RSN-IE, Auto-ACK via
      REG_MACID. Verifiziert: mit FRITZ!Box assoziiert, Status 0, AID=7. → `associate`
- [~] **IE3 — WPA2-Supplicant:** EAPOL-4-Way-Handshake gebaut (`connect`).
      Krypto verifiziert (PBKDF2 = IEEE-802.11i-Testvektor, PASS). Passwort via
      getpass (bleibt lokal). End-zu-End-Test mit echtem PSK durch den Nutzer offen.
- [ ] **IE4 — Schlüssel/Krypto:** CCMP-Keys in den Chip (HW) oder Software-CCMP.
- [ ] **IE5 — utun-Bridge:** utun anlegen, IP ↔ 802.11-Data, ARP/DHCP-Client.
- [ ] **IE6 — Daemon + brew:** launchd-Daemon, `brew`-Formel, Auto-Reconnect.
- [ ] **IE7 — WPA3 (SAE):** Dragonfly-Handshake (EC-Krypto) fuer WPA3-Personal
      und WPA2/WPA3-Mixed-only-Netze. Eigener, groesserer Auth-Mechanismus.

## Ehrliche Einordnung

IE3 (Supplicant) und IE5 (utun/IP-Glue) sind die großen Brocken. Das ist ein
Mehr-Sitzungen-Projekt. Jeder Meilenstein ist einzeln testbar (IE1 ist es schon).
Nur WPA2-PSK zuerst; WPA3/Enterprise später.
