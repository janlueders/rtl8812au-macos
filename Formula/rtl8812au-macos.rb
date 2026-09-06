class Rtl8812auMacos < Formula
  desc "Userspace RTL8812AU/AWUS036ACH monitor mode & injection for macOS"
  homepage "https://github.com/janlueders/rtl8812au-macos"
  license "GPL-2.0-only"
  head "https://github.com/janlueders/rtl8812au-macos.git", branch: "main"

  depends_on "libusb"

  def install
    system "make"
    # Nutzer-Tools mit alfa- Praefix, um generische Namenskollisionen zu vermeiden.
    bin.install "monitor"  => "alfa-monitor"
    bin.install "inject"   => "alfa-inject"
    bin.install "usbprobe" => "alfa-usbprobe"
    bin.install "chipinfo" => "alfa-chipinfo"
    bin.install "alfa-extcap"
  end

  def caveats
    <<~EOS
      Monitor Mode:   alfa-monitor <kanal> <sekunden> <ausgabe.pcap>
      Injection:      alfa-inject  <kanal> <anzahl>
      Geraet-Info:    alfa-usbprobe / alfa-chipinfo

      Wireshark-Anbindung (extcap): alfa-extcap in Wiresharks extcap-Verzeichnis
      verlinken, dann erscheint "Alfa AWUS036ACH" in der Interface-Liste:

        mkdir -p ~/.config/wireshark/extcap
        ln -sf #{opt_bin}/alfa-extcap ~/.config/wireshark/extcap/alfa-extcap

      Hinweis: Nur fuer autorisiertes WLAN-Auditing. Kein System-WLAN-Treiber
      (Apple Silicon unterstuetzt das nicht); reine Userspace-Capture/Injection.
    EOS
  end

  test do
    assert_match "interface {value=alfa0}", shell_output("#{bin}/alfa-extcap --extcap-interfaces")
  end
end
