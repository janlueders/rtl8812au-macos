# Herkunft und Lizenz

Dieses Projekt implementiert einen **nativen macOS-Userspace-Zugriff (Apple
Silicon, arm64)** auf den Realtek RTL8812AU (Alfa AWUS036ACH) für Monitor Mode
und Packet Injection.

Register-Definitionen, Power-On-Sequenzen, efuse-Dekodierung und HAL-Logik sind
**portiert aus** dem Linux-Kernel-Treiber:

- **aircrack-ng/rtl8812au** — <https://github.com/aircrack-ng/rtl8812au>
- ursprünglich © Realtek Corporation, veröffentlicht unter **GPL v2**.

Da dieses Projekt ein abgeleitetes Werk dieses GPLv2-Codes ist, steht es
ebenfalls unter **GPL v2** (siehe `LICENSE`).

Der Linux-Treiber selbst wird hier **nicht** mitverteilt; er wird nur lokal als
Portierungsreferenz geklont (`reference/`, per `.gitignore` ausgeschlossen).

Kein Kext, kein DriverKit, keine Anthropic-/Apple-Zugehörigkeit. Reine
Userspace-USB-Implementierung über libusb.
