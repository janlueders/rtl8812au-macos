# AWUS036ACH (RTL8812AU) nativ auf dem MacBook Pro M2 Pro — via Asahi Linux

Ziel: Monitor Mode + Packet Injection mit dem Alfa AWUS036ACH, **ohne VM**, nativ
auf dem Blech. Weg: Fedora Asahi Remix (bare-metal ARM64-Linux auf Apple Silicon)
+ der `aircrack-ng/rtl8812au`-Treiber per DKMS.

> Warum nicht macOS-nativ: Der Treiber ist reiner Linux-Kernel-Code, Apple Silicon
> hat keinen Third-Party-USB-WLAN-Pfad, und Monitor/Injection sind unter macOS für
> Fremdadapter gar nicht exponiert. Asahi umgeht alle drei Mauern, weil es echtes
> Linux ist.

---

## 0. Vorbereitung (in macOS)

- [ ] **Time-Machine-Backup.** Der Installer verkleinert die macOS-Partition.
- [ ] macOS auf aktuellem Stand (Systemeinstellungen → Softwareupdate).
- [ ] Mindestens ~50–60 GB frei für die Linux-Partition.
- [ ] Admin-Rechte / dein Passwort bereithalten.

## 1. Asahi Linux installieren

In der macOS-**Terminal.app**:

```bash
curl https://alx.sh | sh
```

Der Installer ist interaktiv:
- Distribution wählen: **Fedora Asahi Remix** (KDE Plasma für Desktop, oder die
  minimale/Server-Variante, wenn du nur die Pentest-Tools willst).
- Partitionsgröße für Linux festlegen.
- Am Ende wird ein Reboot in die Recovery/1TR verlangt (Power-Taste gedrückt
  halten), um den Boot-Eintrag zu setzen. Anweisungen des Installers Schritt für
  Schritt folgen.

Nach dem Reboot startest du ins frische Fedora Asahi und schließt die
Erst-Einrichtung ab (User, Netzwerk über das interne WLAN — das läuft auf Asahi).

## 2. System aktualisieren

```bash
sudo dnf update -y
sudo reboot
```

## 3. Build-Abhängigkeiten + passende Kernel-Header

**Wichtig:** Asahi fährt einen **16K-Page-Kernel**. Die `kernel-devel`-Header
müssen exakt zum laufenden Kernel passen, sonst schlägt der DKMS-Build fehl.

```bash
# laufenden Kernel prüfen
uname -r

# Header/Devel passend zum laufenden Kernel + Build-Tools
sudo dnf install -y dkms git bc make gcc \
  "kernel-devel-$(uname -r)" "kernel-headers-$(uname -r)"

# Gegenprobe: Header-Verzeichnis existiert?
ls /usr/src/kernels/$(uname -r)
```

Falls `kernel-devel-$(uname -r)` nicht gefunden wird, erst `sudo dnf update`,
reboot, dann erneut — Header und laufender Kernel müssen dieselbe Version haben.

## 4. rtl8812au-Treiber bauen (DKMS)

```bash
git clone https://github.com/aircrack-ng/rtl8812au.git
cd rtl8812au

# ARM64-Build: Repo unterstützt aarch64. Falls der Makefile-Default nicht greift,
# explizit setzen:
#   make ARCH=arm64
# Für den dauerhaften, kernel-update-festen Weg DKMS nehmen:
sudo make dkms_install
```

`dkms_install` registriert das Modul, sodass es nach Kernel-Updates automatisch
neu gebaut wird. Status prüfen:

```bash
dkms status
```

## 5. Modul laden + Adapter erkennen

Adapter einstecken (direkt am USB-C-Port per Adapter oder an einem USB-Hub).

```bash
sudo modprobe 8812au
# Adapter am USB-Bus?
lsusb | grep -i realtek
# WLAN-Interface da?
ip link
```

Du solltest ein Interface wie `wlan0` / `wlp*` sehen.

## 6. Monitor Mode + Injection testen

```bash
sudo dnf install -y aircrack-ng
sudo airmon-ng start wlan0        # Interface-Namen aus `ip link` einsetzen
# danach heißt es meist wlan0mon o.ä.
sudo airodump-ng wlan0mon         # sniffen — zeigt APs = Monitor Mode läuft

# Injection-Test:
sudo aireplay-ng --test wlan0mon  # "Injection is working!" = Ziel erreicht
```

`Injection is working!` = fertig. Alfa AWUS036ACH nativ, ohne VM.

---

## Troubleshooting / bekannte Stolpersteine

- **DKMS-Build bricht ab (Header-Mismatch):** `uname -r` und die installierte
  `kernel-devel`-Version müssen identisch sein. Nach jedem `dnf update` reboot,
  dann DKMS neu: `cd rtl8812au && sudo make dkms_install`.
- **16K-Page-Kernel:** rtl8812au sollte page-size-agnostisch bauen. Falls doch ein
  Compile-Fehler in Zusammenhang mit `PAGE_SIZE` auftaucht → Issue im Repo suchen
  oder melden; dann brauchen wir ggf. einen kleinen Patch.
- **Interface erscheint nicht (`ip link` leer):** `dmesg | tail -40` direkt nach
  dem Einstecken lesen — zeigt, ob das Modul den Chip greift oder ein Firmware-/
  USB-Problem vorliegt.
- **Monitor Mode nicht aktivierbar:** manche Firmware-Stände brauchen
  `sudo iw dev wlan0 set type monitor` statt airmon-ng. Erst `sudo airmon-ng
  check kill` gegen NetworkManager-Störer.
- **Diese Kombi (rtl8812au auf Asahi) ist öffentlich nicht dokumentiert.** Wenn
  Schritt 4/5 hakt, ist das der wahrscheinlichste Ort. `dmesg`-Ausgabe sichern,
  dann können wir gezielt debuggen.

## Quellen
- Asahi M2 Feature Support: https://asahilinux.org/docs/platform/feature-support/m2/
- Fedora Asahi Remix: https://asahilinux.org/fedora/
- rtl8812au-Treiber: https://github.com/aircrack-ng/rtl8812au
