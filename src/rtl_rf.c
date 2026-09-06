/*
 * rtl_rf.c — RF-Init + Kanalsteuerung fuer den RTL8812AU (macOS/libusb, userspace).
 *
 * INTEGRATION / AUFRUFREIHENFOLGE:
 *   1. rtl_power_on()      (rtl_usb.c)   — Chip einschalten
 *   2. rtl_fw_download()   (rtl_usb.c)   — Firmware laden
 *   3. MAC-Init            (anderes Modul)
 *   4. BB-Init  (PHY_REG + AGC-Tabellen) (anderes Modul)  <-- Voraussetzung!
 *   5. rtl_rf_init(h, verbose)           — DIESES Modul: radioA/radioB-Tabellen
 *   6. (RF/IQ/LCK-Kalibrierung)          — anderes Modul, optional aber empfohlen
 *   7. rtl_rf_set_channel(h, ch, RTL_BW_20)  — DIESES Modul: Band + Kanal + BW
 *
 *   rtl_rf_read()/rtl_rf_write() koennen nach Schritt 4 jederzeit genutzt werden.
 *
 * VORAUSSETZUNGEN (VOR rtl_rf_init):
 *   - Chip powered on, Firmware geladen, MAC-Init fertig.
 *   - BB-Init fertig: insbesondere muessen die BB-RF-Interface-Register und die
 *     LSSI/3-wire-Pfade aktiv sein, sonst schlagen RF-Reads/Writes fehl.
 *     (PHY_BBConfig8812 schaltet u.a. REG_RF_CTRL=0x07 und die RF-Power ein.)
 *
 * RF-ZUGRIFF: RF-Register (Radio A/B) sind NICHT per USB adressierbar. Sie werden
 * indirekt ueber Baseband-LSSI-Register (3-wire) gelesen/geschrieben — hier
 * self-contained via rtl_read32/rtl_write32 nachgebildet (phy_RFSerialRead/Write).
 *
 * ANMERKUNGEN ZUR TREUE:
 *   - Die radioA/radioB-Tabellen sind VERBATIM aus halhwimg8812a_rf.c uebernommen.
 *   - Der Apply-Loop inkl. IF/ELSEIF/ELSE/ENDIF und check_positive() ist exakt
 *     nachgebildet. Die Hardware-Deskriptoren (board_type, cut_version, ...) sind
 *     auf die Standard-Konfiguration eines generischen 8812AU ohne externes
 *     Frontend (board_type=0) gesetzt -> es greifen die ELSE-Zweige. Keine der
 *     Tabellen-Bedingungen selektiert nach Cut-Version/Package (die betreffenden
 *     Nibbles sind in allen Bedingungen 0), daher ist das Ergebnis deterministisch.
 *   - Tx-Power-Setzung und IQK/LCK-Kalibrierung gehoeren NICHT hierher (Schritt 6).
 */

#include <stdio.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_rf.h"

typedef unsigned int u32;   /* fuer die verbatim uebernommenen Tabellen */
typedef unsigned char u8;
typedef unsigned short u16;

/* ===================================================================== *
 *  Register-/Bitmasken-Konstanten (aus include/Hal8812PhyReg.h,
 *  include/hal_com_reg.h, include/rtl8812a_spec.h)
 * ===================================================================== */

/* --- Baseband-Register fuer RF-3-wire-Zugriff (Jaguar) --- */
#define rA_LSSIWrite_Jaguar     0xc90   /* rf3wireOffset Pfad A (RF-Write) */
#define rB_LSSIWrite_Jaguar     0xe90   /* rf3wireOffset Pfad B (RF-Write) */
#define rHSSIRead_Jaguar        0x8b0   /* rfHSSIPara2 (RF-Read-Adresse), beide Pfade */
#define bHSSIRead_addr_Jaguar   0xff
#define rA_PIRead_Jaguar        0xd04   /* RF-Readback (PI) Pfad A */
#define rB_PIRead_Jaguar        0xd44   /* RF-Readback (PI) Pfad B */
#define rA_SIRead_Jaguar        0xd08   /* RF-Readback (SI) Pfad A */
#define rB_SIRead_Jaguar        0xd48   /* RF-Readback (SI) Pfad B */
#define rRead_data_Jaguar       0xfffff /* Readback-Datenmaske (20 bit) */
#define bLSSIWrite_data_Jaguar  0x000fffff
#define RFREGOFFSETMASK         0xfffff /* == bLSSIWrite_data_Jaguar */

/* --- Baseband-Register fuer Kanal/Bandbreite --- */
#define rCCAonSec_Jaguar        0x838
#define rL1PeakTH_Jaguar        0x848
#define rRFMOD_Jaguar           0x8ac   /* RF mode / Bandbreite */
#define rADC_Buf_Clk_Jaguar     0x8c4
#define rFc_area_Jaguar         0x860   /* fc_area */
#define rPwed_TH_Jaguar         0x830
#define rBWIndication_Jaguar    0x834
#define rAGC_table_Jaguar       0x82c
#define rOFDMCCKEN_Jaguar       0x808
#define bOFDMEN_Jaguar          0x20000000
#define bCCKEN_Jaguar           0x10000000
#define rTxPath_Jaguar          0x80c
#define rCCK_RX_Jaguar          0xa04
#define rA_TxScale_Jaguar       0xc1c
#define rB_TxScale_Jaguar       0xe1c
#define rA_RFE_Pinmux_Jaguar    0xcb0
#define rB_RFE_Pinmux_Jaguar    0xeb0
#define rA_RFE_Inv_Jaguar       0xcb4
#define rB_RFE_Inv_Jaguar       0xeb4
#define bMask_RFEInv_Jaguar     0x3ff00000

/* --- MAC-Register --- */
#define REG_RF_CTRL             0x001f
#define REG_CCK_CHECK_8812      0x0454
#define REG_TXPKT_EMPTY         0x041a
#define REG_DATA_SC_8812        0x0483
#define REG_WMAC_TRXPTCL_CTL    0x0668

/* --- RF-Register (adressiert ueber 3-wire) --- */
#define RF_CHNLBW_Jaguar        0x18    /* Kanal + Bandbreite */

/* --- Masken --- */
#define bMaskDWord              0xffffffff
#define bMaskByte0              0xff

/* Bandbreiten (intern; entspricht enum channel_width) */
#define CH_WIDTH_20   0
#define CH_WIDTH_40   1
#define CH_WIDTH_80   2

/* Bandtypen */
#define BAND_ON_2_4G  0
#define BAND_ON_5G    1

/*
 * Hardware-Konfiguration dieses Adapters (AWUS036ACH / generischer 8812AU).
 * 8812AU ist 2T2R -> 2 RF-Pfade. Bei einem 1T1R-Modul auf 1 setzen.
 */
#define RTL_RF_NUM_PATHS   2
#define RTL_RF_IS_2T2R     1   /* beeinflusst L1PeakTH/PWED bei Bandbreite */

/* ===================================================================== *
 *  Kleine Helfer
 * ===================================================================== */

static void udelay_us(unsigned us) { usleep(us); }
static void mdelay_ms(unsigned ms) { usleep(ms * 1000u); }

/* Anzahl der niederwertigen Null-Bits einer Maske (PHY_CalculateBitShift). */
static u32 calc_bit_shift(u32 mask)
{
	u32 i;
	for (i = 0; i <= 31; i++)
		if (((mask >> i) & 0x1) == 1)
			break;
	return i;
}

/* ---- Baseband (BB) maskiertes Lesen/Schreiben (PHY_Query/SetBBReg8812) ---- */

static u32 bb_read(libusb_device_handle *h, u32 addr, u32 mask)
{
	u32 orig = rtl_read32(h, (uint16_t)addr, NULL);
	u32 shift = calc_bit_shift(mask);
	return (orig & mask) >> shift;
}

static int bb_write(libusb_device_handle *h, u32 addr, u32 mask, u32 data)
{
	if (mask != bMaskDWord) {
		u32 orig  = rtl_read32(h, (uint16_t)addr, NULL);
		u32 shift = calc_bit_shift(mask);
		data = (orig & ~mask) | ((data << shift) & mask);
	}
	return rtl_write32(h, (uint16_t)addr, data);
}

/* ===================================================================== *
 *  RF-Serial-Read/Write ueber die BB-LSSI-Register (phy_RFSerialRead/Write)
 * ===================================================================== */

/* Rohes 20-bit RF-Register lesen (phy_RFSerialRead, non-C-cut-Pfad). */
static u32 phy_rf_serial_read(libusb_device_handle *h, int path, u32 offset)
{
	u32 ret;
	int is_pi;

	/* CCA OFF vor dem Lesen (nur wenn Offset != 0, non-C-cut). */
	if (offset != 0x0)
		bb_write(h, rCCAonSec_Jaguar, 0x8, 1);

	offset &= 0xff;

	/* PI- oder SI-Modus? (0xC00[2] fuer A, 0xE00[2] fuer B) */
	if (path == RTL_RF_PATH_A)
		is_pi = (int)bb_read(h, 0xC00, 0x4);
	else
		is_pi = (int)bb_read(h, 0xE00, 0x4);

	/* RF-Read-Adresse setzen (rfHSSIPara2 = 0x8b0 fuer beide Pfade). */
	bb_write(h, rHSSIRead_Jaguar, bHSSIRead_addr_Jaguar, offset);

	/* kurze Wartezeit bis der 3-wire-Transfer stabil ist */
	udelay_us(10);

	if (is_pi)
		ret = bb_read(h, (path == RTL_RF_PATH_A) ? rA_PIRead_Jaguar : rB_PIRead_Jaguar,
			      rRead_data_Jaguar);
	else
		ret = bb_read(h, (path == RTL_RF_PATH_A) ? rA_SIRead_Jaguar : rB_SIRead_Jaguar,
			      rRead_data_Jaguar);

	/* CCA ON nach dem Lesen. */
	if (offset != 0x0)
		bb_write(h, rCCAonSec_Jaguar, 0x8, 0);

	return ret;
}

/* Rohes 20-bit RF-Register schreiben (phy_RFSerialWrite). */
static int phy_rf_serial_write(libusb_device_handle *h, int path, u32 offset, u32 data)
{
	u32 data_and_addr;
	u32 reg = (path == RTL_RF_PATH_A) ? rA_LSSIWrite_Jaguar : rB_LSSIWrite_Jaguar;

	offset &= 0xff;
	/* Write-Adresse in [27:20], Write-Daten in [19:00]. */
	data_and_addr = ((offset << 20) | (data & 0x000fffff)) & 0x0fffffff;
	return rtl_write32(h, (uint16_t)reg, data_and_addr);
}

/*
 * Maskiertes RF-Schreiben (phy_set_rf_reg / PHY_SetRFReg8812).
 * WICHTIG: Wie im Referenztreiber wird (data << shift) OHNE erneutes Maskieren
 * mit ~mask verodert (bewusst so, siehe RF_MOD_AG-Werte wie 0x101).
 * Bei voller 20-bit-Maske wird direkt geschrieben (kein read-modify-write).
 */
static int rf_write_mask(libusb_device_handle *h, int path, u32 reg, u32 mask, u32 data)
{
	if (mask == 0)
		return 0;
	if (mask != bLSSIWrite_data_Jaguar) {
		u32 orig  = phy_rf_serial_read(h, path, reg);
		u32 shift = calc_bit_shift(mask);
		data = (orig & ~mask) | (data << shift);
	}
	return phy_rf_serial_write(h, path, reg, data);
}

/* ===================================================================== *
 *  Oeffentliche RF-Register-API
 * ===================================================================== */

uint32_t rtl_rf_read(libusb_device_handle *h, int path, uint16_t reg_addr, int *rc)
{
	uint32_t v;
	if (path != RTL_RF_PATH_A && path != RTL_RF_PATH_B) {
		if (rc) *rc = -1;
		return 0;
	}
	v = phy_rf_serial_read(h, path, reg_addr) & rRead_data_Jaguar;
	if (rc) *rc = 0;
	return v;
}

int rtl_rf_write(libusb_device_handle *h, int path, uint16_t reg_addr, uint32_t data)
{
	if (path != RTL_RF_PATH_A && path != RTL_RF_PATH_B)
		return -1;
	return phy_rf_serial_write(h, path, reg_addr, data);
}

/* ===================================================================== *
 *  RF-Register-Tabellen (VERBATIM aus hal/phydm/rtl8812a/halhwimg8812a_rf.c)
 * ===================================================================== */

static u32 array_mp_8812a_radioa[] = {
		0x000, 0x00010000,
		0x018, 0x0001712A,
		0x056, 0x00051CF2,
		0x066, 0x00040000,
		0x01E, 0x00080000,
		0x089, 0x00000080,
	0x80000001,	0x00000000,	0x40000000,	0x00000000,
		0x086, 0x00014B3A,
	0x90000001,	0x00000005,	0x40000000,	0x00000000,
		0x086, 0x00014B3A,
	0xA0000000,	0x00000000,
		0x086, 0x00014B38,
	0xB0000000,	0x00000000,
	0x80000004,	0x00000000,	0x40000000,	0x00000000,
		0x08B, 0x00080180,
	0xA0000000,	0x00000000,
		0x08B, 0x00087180,
	0xB0000000,	0x00000000,
		0x0B1, 0x0001FC1A,
		0x0B3, 0x000F0810,
		0x0B4, 0x0001A78D,
		0x0BA, 0x00086180,
		0x018, 0x00000006,
		0x0EF, 0x00002000,
	0x80000001,	0x00000000,	0x40000000,	0x00000000,
		0x03B, 0x0003F218,
		0x03B, 0x00030A58,
		0x03B, 0x0002FA58,
		0x03B, 0x00022590,
		0x03B, 0x0001FA50,
		0x03B, 0x00010248,
		0x03B, 0x00008240,
	0x90000001,	0x00000005,	0x40000000,	0x00000000,
		0x03B, 0x0003F218,
		0x03B, 0x00030A58,
		0x03B, 0x0002FA58,
		0x03B, 0x00022590,
		0x03B, 0x0001FA50,
		0x03B, 0x00010248,
		0x03B, 0x00008240,
	0xA0000000,	0x00000000,
		0x03B, 0x00038A58,
		0x03B, 0x00037A58,
		0x03B, 0x0002A590,
		0x03B, 0x00027A50,
		0x03B, 0x00018248,
		0x03B, 0x00010240,
		0x03B, 0x00008240,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000100,
	0x80000002,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0000A4EE,
		0x034, 0x00009076,
		0x034, 0x00008073,
		0x034, 0x00007070,
		0x034, 0x0000606D,
		0x034, 0x0000506A,
		0x034, 0x00004049,
		0x034, 0x00003046,
		0x034, 0x00002028,
		0x034, 0x00001025,
		0x034, 0x00000022,
	0xA0000000,	0x00000000,
		0x034, 0x0000ADF4,
		0x034, 0x00009DF1,
		0x034, 0x00008DEE,
		0x034, 0x00007DEB,
		0x034, 0x00006DE8,
		0x034, 0x00005DE5,
		0x034, 0x00004DE2,
		0x034, 0x00003CE6,
		0x034, 0x000024E7,
		0x034, 0x000014E4,
		0x034, 0x000004E1,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
		0x0EF, 0x000020A2,
		0x0DF, 0x00000080,
		0x035, 0x00000192,
		0x035, 0x00008192,
		0x035, 0x00010192,
		0x036, 0x00000024,
		0x036, 0x00008024,
		0x036, 0x00010024,
		0x036, 0x00018024,
		0x0EF, 0x00000000,
		0x051, 0x00000C21,
		0x052, 0x000006D9,
		0x053, 0x000FC649,
		0x054, 0x0000017E,
		0x0EF, 0x00000002,
		0x008, 0x00008400,
		0x018, 0x0001712A,
		0x0EF, 0x00001000,
		0x03A, 0x00000080,
		0x03B, 0x0003A02C,
		0x03C, 0x00004000,
		0x03A, 0x00000400,
		0x03B, 0x0003202C,
		0x03C, 0x00010000,
		0x03A, 0x000000A0,
		0x03B, 0x0002B064,
		0x03C, 0x00004000,
		0x03A, 0x000000D8,
		0x03B, 0x00023070,
		0x03C, 0x00004000,
		0x03A, 0x00000468,
		0x03B, 0x0001B870,
		0x03C, 0x00010000,
		0x03A, 0x00000098,
		0x03B, 0x00012085,
		0x03C, 0x000E4000,
		0x03A, 0x00000418,
		0x03B, 0x0000A080,
		0x03C, 0x000F0000,
		0x03A, 0x00000418,
		0x03B, 0x00002080,
		0x03C, 0x00010000,
		0x03A, 0x00000080,
		0x03B, 0x0007A02C,
		0x03C, 0x00004000,
		0x03A, 0x00000400,
		0x03B, 0x0007202C,
		0x03C, 0x00010000,
		0x03A, 0x000000A0,
		0x03B, 0x0006B064,
		0x03C, 0x00004000,
		0x03A, 0x000000D8,
		0x03B, 0x00063070,
		0x03C, 0x00004000,
		0x03A, 0x00000468,
		0x03B, 0x0005B870,
		0x03C, 0x00010000,
		0x03A, 0x00000098,
		0x03B, 0x00052085,
		0x03C, 0x000E4000,
		0x03A, 0x00000418,
		0x03B, 0x0004A080,
		0x03C, 0x000F0000,
		0x03A, 0x00000418,
		0x03B, 0x00042080,
		0x03C, 0x00010000,
		0x03A, 0x00000080,
		0x03B, 0x000BA02C,
		0x03C, 0x00004000,
		0x03A, 0x00000400,
		0x03B, 0x000B202C,
		0x03C, 0x00010000,
		0x03A, 0x000000A0,
		0x03B, 0x000AB064,
		0x03C, 0x00004000,
		0x03A, 0x000000D8,
		0x03B, 0x000A3070,
		0x03C, 0x00004000,
		0x03A, 0x00000468,
		0x03B, 0x0009B870,
		0x03C, 0x00010000,
		0x03A, 0x00000098,
		0x03B, 0x00092085,
		0x03C, 0x000E4000,
		0x03A, 0x00000418,
		0x03B, 0x0008A080,
		0x03C, 0x000F0000,
		0x03A, 0x00000418,
		0x03B, 0x00082080,
		0x03C, 0x00010000,
		0x0EF, 0x00001100,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0004A0B2,
		0x034, 0x000490AF,
		0x034, 0x00048070,
		0x034, 0x0004706D,
		0x034, 0x00046050,
		0x034, 0x0004504D,
		0x034, 0x0004404A,
		0x034, 0x00043047,
		0x034, 0x0004200A,
		0x034, 0x00041007,
		0x034, 0x00040004,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x034, 0x0004A0B2,
		0x034, 0x000490AF,
		0x034, 0x00048070,
		0x034, 0x0004706D,
		0x034, 0x0004604D,
		0x034, 0x0004504A,
		0x034, 0x00044047,
		0x034, 0x00043044,
		0x034, 0x00042007,
		0x034, 0x00041004,
		0x034, 0x00040001,
	0xA0000000,	0x00000000,
		0x034, 0x0004ADF5,
		0x034, 0x00049DF2,
		0x034, 0x00048DEF,
		0x034, 0x00047DEC,
		0x034, 0x00046DE9,
		0x034, 0x00045DE6,
		0x034, 0x00044DE3,
		0x034, 0x000438C8,
		0x034, 0x000428C5,
		0x034, 0x000418C2,
		0x034, 0x000408C0,
	0xB0000000,	0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0002A0B2,
		0x034, 0x000290AF,
		0x034, 0x00028070,
		0x034, 0x0002706D,
		0x034, 0x00026050,
		0x034, 0x0002504D,
		0x034, 0x0002404A,
		0x034, 0x00023047,
		0x034, 0x0002200A,
		0x034, 0x00021007,
		0x034, 0x00020004,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x034, 0x0002A0B4,
		0x034, 0x000290B1,
		0x034, 0x00028072,
		0x034, 0x0002706F,
		0x034, 0x0002604F,
		0x034, 0x0002504C,
		0x034, 0x00024049,
		0x034, 0x00023046,
		0x034, 0x00022009,
		0x034, 0x00021006,
		0x034, 0x00020003,
	0xA0000000,	0x00000000,
		0x034, 0x0002ADF5,
		0x034, 0x00029DF2,
		0x034, 0x00028DEF,
		0x034, 0x00027DEC,
		0x034, 0x00026DE9,
		0x034, 0x00025DE6,
		0x034, 0x00024DE3,
		0x034, 0x000238C8,
		0x034, 0x000228C5,
		0x034, 0x000218C2,
		0x034, 0x000208C0,
	0xB0000000,	0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0000A0B2,
		0x034, 0x000090AF,
		0x034, 0x00008070,
		0x034, 0x0000706D,
		0x034, 0x00006050,
		0x034, 0x0000504D,
		0x034, 0x0000404A,
		0x034, 0x00003047,
		0x034, 0x0000200A,
		0x034, 0x00001007,
		0x034, 0x00000004,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x034, 0x0000A0B2,
		0x034, 0x000090AF,
		0x034, 0x00008070,
		0x034, 0x0000706D,
		0x034, 0x0000604D,
		0x034, 0x0000504A,
		0x034, 0x00004047,
		0x034, 0x00003044,
		0x034, 0x00002007,
		0x034, 0x00001004,
		0x034, 0x00000001,
	0xA0000000,	0x00000000,
		0x034, 0x0000AFF7,
		0x034, 0x00009DF7,
		0x034, 0x00008DF4,
		0x034, 0x00007DF1,
		0x034, 0x00006DEE,
		0x034, 0x00005DEB,
		0x034, 0x00004DE8,
		0x034, 0x000038CC,
		0x034, 0x000028C9,
		0x034, 0x000018C6,
		0x034, 0x000008C3,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000040,
		0x035, 0x000001D4,
		0x035, 0x000081D4,
		0x035, 0x000101D4,
		0x035, 0x000201B4,
		0x035, 0x000281B4,
		0x035, 0x000301B4,
		0x035, 0x000401B4,
		0x035, 0x000481B4,
		0x035, 0x000501B4,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000040,
		0x035, 0x000001D4,
		0x035, 0x000081D4,
		0x035, 0x000101D4,
		0x035, 0x000201B4,
		0x035, 0x000281B4,
		0x035, 0x000301B4,
		0x035, 0x000401B4,
		0x035, 0x000481B4,
		0x035, 0x000501B4,
	0xA0000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000040,
		0x035, 0x00000188,
		0x035, 0x00008147,
		0x035, 0x00010147,
		0x035, 0x000201D7,
		0x035, 0x000281D7,
		0x035, 0x000301D7,
		0x035, 0x000401D8,
		0x035, 0x000481D8,
		0x035, 0x000501D8,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000010,
		0x036, 0x00004BFB,
		0x036, 0x0000CBFB,
		0x036, 0x00014BFB,
		0x036, 0x0001CBFB,
		0x036, 0x00024F4B,
		0x036, 0x0002CF4B,
		0x036, 0x00034F4B,
		0x036, 0x0003CF4B,
		0x036, 0x00044F4B,
		0x036, 0x0004CF4B,
		0x036, 0x00054F4B,
		0x036, 0x0005CF4B,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000010,
		0x036, 0x00004BFB,
		0x036, 0x0000CBFB,
		0x036, 0x00014BFB,
		0x036, 0x0001CBFB,
		0x036, 0x00024F4B,
		0x036, 0x0002CF4B,
		0x036, 0x00034F4B,
		0x036, 0x0003CF4B,
		0x036, 0x00044F4B,
		0x036, 0x0004CF4B,
		0x036, 0x00054F4B,
		0x036, 0x0005CF4B,
	0xA0000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000010,
		0x036, 0x00084EB4,
		0x036, 0x0008CC35,
		0x036, 0x00094C35,
		0x036, 0x0009CC35,
		0x036, 0x000A4C35,
		0x036, 0x000ACC35,
		0x036, 0x000B4C35,
		0x036, 0x000BCC35,
		0x036, 0x000C4C34,
		0x036, 0x000CCC35,
		0x036, 0x000D4C35,
		0x036, 0x000DCC35,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
		0x0EF, 0x00000008,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x03C, 0x000002CC,
		0x03C, 0x00000522,
		0x03C, 0x00000902,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x03C, 0x000002CC,
		0x03C, 0x00000522,
		0x03C, 0x00000902,
	0xA0000000,	0x00000000,
		0x03C, 0x000002A8,
		0x03C, 0x000005A2,
		0x03C, 0x00000880,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000002,
		0x0DF, 0x00000080,
		0x01F, 0x00000064,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x061, 0x000FDD43,
		0x062, 0x00038F4B,
		0x063, 0x00032117,
		0x064, 0x000194AC,
		0x065, 0x000931D1,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x061, 0x000FDD43,
		0x062, 0x00038F4B,
		0x063, 0x00032117,
		0x064, 0x000194AC,
		0x065, 0x000931D2,
	0xA0000000,	0x00000000,
		0x061, 0x000E5D53,
		0x062, 0x00038FCD,
		0x063, 0x000114EB,
		0x064, 0x000196AC,
		0x065, 0x000911D7,
	0xB0000000,	0x00000000,
		0x008, 0x00008400,
		0x01C, 0x000739D2,
		0x0B4, 0x0001E78D,
		0x018, 0x0001F12A,
		0xFFE, 0x00000000,
		0xFFE, 0x00000000,
		0xFFE, 0x00000000,
		0xFFE, 0x00000000,
		0x0B4, 0x0001A78D,
		0x018, 0x0001712A,

};

static u32 array_mp_8812a_radiob[] = {
		0x056, 0x00051CF2,
		0x066, 0x00040000,
		0x089, 0x00000080,
	0x80000001,	0x00000000,	0x40000000,	0x00000000,
		0x086, 0x00014B3A,
	0x90000001,	0x00000005,	0x40000000,	0x00000000,
		0x086, 0x00014B3A,
	0xA0000000,	0x00000000,
		0x086, 0x00014B38,
	0xB0000000,	0x00000000,
	0x80000004,	0x00000000,	0x40000000,	0x00000000,
		0x08B, 0x00080180,
	0xA0000000,	0x00000000,
		0x08B, 0x00087180,
	0xB0000000,	0x00000000,
		0x018, 0x00000006,
		0x0EF, 0x00002000,
	0x80000001,	0x00000000,	0x40000000,	0x00000000,
		0x03B, 0x0003F218,
		0x03B, 0x00030A58,
		0x03B, 0x0002FA58,
		0x03B, 0x00022590,
		0x03B, 0x0001FA50,
		0x03B, 0x00010248,
		0x03B, 0x00008240,
	0x90000001,	0x00000005,	0x40000000,	0x00000000,
		0x03B, 0x0003F218,
		0x03B, 0x00030A58,
		0x03B, 0x0002FA58,
		0x03B, 0x00022590,
		0x03B, 0x0001FA50,
		0x03B, 0x00010248,
		0x03B, 0x00008240,
	0xA0000000,	0x00000000,
		0x03B, 0x00038A58,
		0x03B, 0x00037A58,
		0x03B, 0x0002A590,
		0x03B, 0x00027A50,
		0x03B, 0x00018248,
		0x03B, 0x00010240,
		0x03B, 0x00008240,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000100,
	0x80000002,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0000A4EE,
		0x034, 0x00009076,
		0x034, 0x00008073,
		0x034, 0x00007070,
		0x034, 0x0000606D,
		0x034, 0x0000506A,
		0x034, 0x00004049,
		0x034, 0x00003046,
		0x034, 0x00002028,
		0x034, 0x00001025,
		0x034, 0x00000022,
	0xA0000000,	0x00000000,
		0x034, 0x0000ADF4,
		0x034, 0x00009DF1,
		0x034, 0x00008DEE,
		0x034, 0x00007DEB,
		0x034, 0x00006DE8,
		0x034, 0x00005DE5,
		0x034, 0x00004DE2,
		0x034, 0x00003CE6,
		0x034, 0x000024E7,
		0x034, 0x000014E4,
		0x034, 0x000004E1,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
		0x0EF, 0x000020A2,
		0x0DF, 0x00000080,
		0x035, 0x00000192,
		0x035, 0x00008192,
		0x035, 0x00010192,
		0x036, 0x00000024,
		0x036, 0x00008024,
		0x036, 0x00010024,
		0x036, 0x00018024,
		0x0EF, 0x00000000,
		0x051, 0x00000C21,
		0x052, 0x000006D9,
		0x053, 0x000FC649,
		0x054, 0x0000017E,
		0x0EF, 0x00000002,
		0x008, 0x00008400,
		0x018, 0x0001712A,
		0x0EF, 0x00001000,
		0x03A, 0x00000080,
		0x03B, 0x0003A02C,
		0x03C, 0x00004000,
		0x03A, 0x00000400,
		0x03B, 0x0003202C,
		0x03C, 0x00010000,
		0x03A, 0x000000A0,
		0x03B, 0x0002B064,
		0x03C, 0x00004000,
		0x03A, 0x000000D8,
		0x03B, 0x00023070,
		0x03C, 0x00004000,
		0x03A, 0x00000468,
		0x03B, 0x0001B870,
		0x03C, 0x00010000,
		0x03A, 0x00000098,
		0x03B, 0x00012085,
		0x03C, 0x000E4000,
		0x03A, 0x00000418,
		0x03B, 0x0000A080,
		0x03C, 0x000F0000,
		0x03A, 0x00000418,
		0x03B, 0x00002080,
		0x03C, 0x00010000,
		0x03A, 0x00000080,
		0x03B, 0x0007A02C,
		0x03C, 0x00004000,
		0x03A, 0x00000400,
		0x03B, 0x0007202C,
		0x03C, 0x00010000,
		0x03A, 0x000000A0,
		0x03B, 0x0006B064,
		0x03C, 0x00004000,
		0x03A, 0x000000D8,
		0x03B, 0x00063070,
		0x03C, 0x00004000,
		0x03A, 0x00000468,
		0x03B, 0x0005B870,
		0x03C, 0x00010000,
		0x03A, 0x00000098,
		0x03B, 0x00052085,
		0x03C, 0x000E4000,
		0x03A, 0x00000418,
		0x03B, 0x0004A080,
		0x03C, 0x000F0000,
		0x03A, 0x00000418,
		0x03B, 0x00042080,
		0x03C, 0x00010000,
		0x03A, 0x00000080,
		0x03B, 0x000BA02C,
		0x03C, 0x00004000,
		0x03A, 0x00000400,
		0x03B, 0x000B202C,
		0x03C, 0x00010000,
		0x03A, 0x000000A0,
		0x03B, 0x000AB064,
		0x03C, 0x00004000,
		0x03A, 0x000000D8,
		0x03B, 0x000A3070,
		0x03C, 0x00004000,
		0x03A, 0x00000468,
		0x03B, 0x0009B870,
		0x03C, 0x00010000,
		0x03A, 0x00000098,
		0x03B, 0x00092085,
		0x03C, 0x000E4000,
		0x03A, 0x00000418,
		0x03B, 0x0008A080,
		0x03C, 0x000F0000,
		0x03A, 0x00000418,
		0x03B, 0x00082080,
		0x03C, 0x00010000,
		0x0EF, 0x00001100,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0004A0B2,
		0x034, 0x000490AF,
		0x034, 0x00048070,
		0x034, 0x0004706D,
		0x034, 0x00046050,
		0x034, 0x0004504D,
		0x034, 0x0004404A,
		0x034, 0x00043047,
		0x034, 0x0004200A,
		0x034, 0x00041007,
		0x034, 0x00040004,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x034, 0x0004A0B1,
		0x034, 0x000490AE,
		0x034, 0x0004806F,
		0x034, 0x0004706C,
		0x034, 0x0004604C,
		0x034, 0x00045049,
		0x034, 0x00044046,
		0x034, 0x00043043,
		0x034, 0x00042006,
		0x034, 0x00041003,
		0x034, 0x00040000,
	0xA0000000,	0x00000000,
		0x034, 0x0004ADF5,
		0x034, 0x00049DF2,
		0x034, 0x00048DEF,
		0x034, 0x00047DEC,
		0x034, 0x00046DE9,
		0x034, 0x00045DE6,
		0x034, 0x00044DE3,
		0x034, 0x000438C8,
		0x034, 0x000428C5,
		0x034, 0x000418C2,
		0x034, 0x000408C0,
	0xB0000000,	0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0002A0B2,
		0x034, 0x000290AF,
		0x034, 0x00028070,
		0x034, 0x0002706D,
		0x034, 0x00026050,
		0x034, 0x0002504D,
		0x034, 0x0002404A,
		0x034, 0x00023047,
		0x034, 0x0002200A,
		0x034, 0x00021007,
		0x034, 0x00020004,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x034, 0x0002A0B3,
		0x034, 0x000290B0,
		0x034, 0x00028071,
		0x034, 0x0002706E,
		0x034, 0x0002604E,
		0x034, 0x0002504B,
		0x034, 0x00024048,
		0x034, 0x00023045,
		0x034, 0x00022008,
		0x034, 0x00021005,
		0x034, 0x00020002,
	0xA0000000,	0x00000000,
		0x034, 0x0002ADF5,
		0x034, 0x00029DF2,
		0x034, 0x00028DEF,
		0x034, 0x00027DEC,
		0x034, 0x00026DE9,
		0x034, 0x00025DE6,
		0x034, 0x00024DE3,
		0x034, 0x000238C8,
		0x034, 0x000228C5,
		0x034, 0x000218C2,
		0x034, 0x000208C0,
	0xB0000000,	0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x034, 0x0000A0B2,
		0x034, 0x000090AF,
		0x034, 0x00008070,
		0x034, 0x0000706D,
		0x034, 0x00006050,
		0x034, 0x0000504D,
		0x034, 0x0000404A,
		0x034, 0x00003047,
		0x034, 0x0000200A,
		0x034, 0x00001007,
		0x034, 0x00000004,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x034, 0x0000A0B3,
		0x034, 0x000090B0,
		0x034, 0x00008070,
		0x034, 0x0000706D,
		0x034, 0x0000604D,
		0x034, 0x0000504A,
		0x034, 0x00004047,
		0x034, 0x00003044,
		0x034, 0x00002007,
		0x034, 0x00001004,
		0x034, 0x00000001,
	0xA0000000,	0x00000000,
		0x034, 0x0000AFF7,
		0x034, 0x00009DF7,
		0x034, 0x00008DF4,
		0x034, 0x00007DF1,
		0x034, 0x00006DEE,
		0x034, 0x00005DEB,
		0x034, 0x00004DE8,
		0x034, 0x000038CC,
		0x034, 0x000028C9,
		0x034, 0x000018C6,
		0x034, 0x000008C3,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000040,
		0x035, 0x000001C5,
		0x035, 0x000081C5,
		0x035, 0x000101C5,
		0x035, 0x00020174,
		0x035, 0x00028174,
		0x035, 0x00030174,
		0x035, 0x00040185,
		0x035, 0x00048185,
		0x035, 0x00050185,
		0x0EF, 0x00000000,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000040,
		0x035, 0x000001C5,
		0x035, 0x000081C5,
		0x035, 0x000101C5,
		0x035, 0x00020174,
		0x035, 0x00028174,
		0x035, 0x00030174,
		0x035, 0x00040185,
		0x035, 0x00048185,
		0x035, 0x00050185,
		0x0EF, 0x00000000,
	0xA0000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000040,
		0x035, 0x00000188,
		0x035, 0x00008147,
		0x035, 0x00010147,
		0x035, 0x000201D7,
		0x035, 0x000281D7,
		0x035, 0x000301D7,
		0x035, 0x000401D8,
		0x035, 0x000481D8,
		0x035, 0x000501D8,
		0x0EF, 0x00000000,
	0xB0000000,	0x00000000,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000010,
		0x036, 0x00005B8B,
		0x036, 0x0000DB8B,
		0x036, 0x00015B8B,
		0x036, 0x0001DB8B,
		0x036, 0x000262DB,
		0x036, 0x0002E2DB,
		0x036, 0x000362DB,
		0x036, 0x0003E2DB,
		0x036, 0x0004553B,
		0x036, 0x0004D53B,
		0x036, 0x0005553B,
		0x036, 0x0005D53B,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000010,
		0x036, 0x00005B8B,
		0x036, 0x0000DB8B,
		0x036, 0x00015B8B,
		0x036, 0x0001DB8B,
		0x036, 0x000262DB,
		0x036, 0x0002E2DB,
		0x036, 0x000362DB,
		0x036, 0x0003E2DB,
		0x036, 0x0004553B,
		0x036, 0x0004D53B,
		0x036, 0x0005553B,
		0x036, 0x0005D53B,
	0xA0000000,	0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000010,
		0x036, 0x00084EB4,
		0x036, 0x0008CC35,
		0x036, 0x00094C35,
		0x036, 0x0009CC35,
		0x036, 0x000A4C35,
		0x036, 0x000ACC35,
		0x036, 0x000B4C35,
		0x036, 0x000BCC35,
		0x036, 0x000C4C34,
		0x036, 0x000CCC35,
		0x036, 0x000D4C35,
		0x036, 0x000DCC35,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
		0x0EF, 0x00000008,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x03C, 0x000002DC,
		0x03C, 0x00000524,
		0x03C, 0x00000902,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x03C, 0x000002DC,
		0x03C, 0x00000524,
		0x03C, 0x00000902,
	0xA0000000,	0x00000000,
		0x03C, 0x000002A8,
		0x03C, 0x000005A2,
		0x03C, 0x00000880,
	0xB0000000,	0x00000000,
		0x0EF, 0x00000000,
		0x018, 0x0001712A,
		0x0EF, 0x00000002,
		0x0DF, 0x00000080,
	0x80000008,	0x00000000,	0x40000000,	0x00000000,
		0x061, 0x000EAC43,
		0x062, 0x00038F47,
		0x063, 0x00031157,
		0x064, 0x0001C4AC,
		0x065, 0x000931D1,
	0x90000008,	0x05000000,	0x40000000,	0x00000000,
		0x061, 0x000EAC43,
		0x062, 0x00038F47,
		0x063, 0x00031157,
		0x064, 0x0001C4AC,
		0x065, 0x000931D2,
	0x90000002,	0x00000000,	0x40000000,	0x00000000,
		0x061, 0x000EAC43,
		0x062, 0x00038F47,
		0x063, 0x00031157,
		0x064, 0x0001C4AC,
		0x065, 0x000931D1,
	0xA0000000,	0x00000000,
		0x061, 0x000E5D53,
		0x062, 0x00038FCD,
		0x063, 0x000114EB,
		0x064, 0x000196AC,
		0x065, 0x000911D7,
	0xB0000000,	0x00000000,
		0x008, 0x00008400,

};

/* ===================================================================== *
 *  Tabellen-Apply (odm_read_and_config_mp_8812a_radioa/b + check_positive)
 * ===================================================================== */

#define COND_ELSE   2
#define COND_ENDIF  3

/*
 * Hardware-Deskriptoren fuer check_positive(). Standardwerte fuer einen
 * generischen 8812AU ohne externes Frontend-Modul (AWUS036ACH): board_type=0,
 * keine LNA/PA-Typen. -> es greifen ausschliesslich die ELSE-Zweige.
 * (Keine Tabellenbedingung selektiert nach Cut-Version/Package, daher spielen
 *  cut_version/package_type keine Rolle fuer die 8812AU-RF-Tabellen.)
 */
static const u32 g_board_type   = 0;
static const u32 g_cut_version  = 0;
static const u32 g_package_type = 0;
static const u32 g_support_interface = 0x01; /* USB (fuer die Tabellen irrelevant) */
static const u32 g_support_platform  = 0x08; /* ODM_CE (irrelevant) */
static const u32 g_type_glna = 0, g_type_gpa = 0, g_type_alna = 0, g_type_apa = 0;

/* 1:1-Portierung von check_positive() aus halhwimg8812a_rf.c */
static int check_positive(u32 condition1, u32 condition2, u32 condition3, u32 condition4)
{
	u8 _board_type = ((g_board_type & (1u << 4)) >> 4) << 0 | /* _GLNA */
			 ((g_board_type & (1u << 3)) >> 3) << 1 | /* _GPA  */
			 ((g_board_type & (1u << 7)) >> 7) << 2 | /* _ALNA */
			 ((g_board_type & (1u << 6)) >> 6) << 3 | /* _APA  */
			 ((g_board_type & (1u << 2)) >> 2) << 4 | /* _BT   */
			 ((g_board_type & (1u << 1)) >> 1) << 5 | /* _NGFF */
			 ((g_board_type & (1u << 5)) >> 5) << 6;  /* _TRSWT*/

	u32 cond1 = condition1, cond2 = condition2, cond4 = condition4;
	(void)condition3;

	u8 cut_version_for_para = (g_cut_version == 0 /*ODM_CUT_A*/) ? 15 : (u8)g_cut_version;
	u8 pkg_type_for_para = (g_package_type == 0) ? 15 : (u8)g_package_type;

	u32 driver1 = (u32)cut_version_for_para << 24 |
		      (g_support_interface & 0xF0) << 16 |
		      g_support_platform << 16 |
		      (u32)pkg_type_for_para << 12 |
		      (g_support_interface & 0x0F) << 8 |
		      _board_type;

	u32 driver2 = (g_type_glna & 0xFF) << 0 |
		      (g_type_gpa & 0xFF) << 8 |
		      (g_type_alna & 0xFF) << 16 |
		      (g_type_apa & 0xFF) << 24;

	u32 driver4 = (g_type_glna & 0xFF00) >> 8 |
		      (g_type_gpa & 0xFF00) |
		      (g_type_alna & 0xFF00) << 8 |
		      (g_type_apa & 0xFF00) << 16;

	/*============== value Defined Check ===============*/
	if (((cond1 & 0x0000F000) != 0) && ((cond1 & 0x0000F000) != (driver1 & 0x0000F000)))
		return 0;
	if (((cond1 & 0x0F000000) != 0) && ((cond1 & 0x0F000000) != (driver1 & 0x0F000000)))
		return 0;

	/*=============== Bit Defined Check ================*/
	cond1 &= 0x00FF0FFF;
	driver1 &= 0x00FF0FFF;

	if ((cond1 & driver1) == cond1) {
		u32 bit_mask = 0;

		if ((cond1 & 0x0F) == 0) /* board_type is DONTCARE */
			return 1;

		if ((cond1 & (1u << 0)) != 0) bit_mask |= 0x000000FF; /* GLNA */
		if ((cond1 & (1u << 1)) != 0) bit_mask |= 0x0000FF00; /* GPA  */
		if ((cond1 & (1u << 2)) != 0) bit_mask |= 0x00FF0000; /* ALNA */
		if ((cond1 & (1u << 3)) != 0) bit_mask |= 0xFF000000; /* APA  */

		if (((cond2 & bit_mask) == (driver2 & bit_mask)) &&
		    ((cond4 & bit_mask) == (driver4 & bit_mask)))
			return 1;
		return 0;
	}
	return 0;
}

/* Eine RF-Tabellenzeile anwenden (odm_config_rf_reg_8812a). */
static int config_rf_reg(libusb_device_handle *h, int path, u32 addr, u32 data)
{
	if (addr == 0xfe || addr == 0xffe) {
		mdelay_ms(50);
		return 0;
	}
	/* addr | maskfor_phy_set; maskfor_phy_set = content & 0xE000 = 0 fuer 8812AU */
	int rc = rf_write_mask(h, path, addr & 0xff, RFREGOFFSETMASK, data);
	udelay_us(1);
	return rc;
}

/*
 * Tabellen-Interpreter, exakt wie odm_read_and_config_mp_8812a_radioa/b:
 * behandelt IF/ELSEIF (0x8.../0x9...), ELSE (0xA...), ENDIF (0xB...) und die
 * Negativbedingung (0x4...) via check_positive().
 */
static int apply_rf_table(libusb_device_handle *h, int path,
			  const u32 *array, u32 array_len, int verbose)
{
	u32 i = 0;
	u8 c_cond;
	int is_matched = 1, is_skipped = 0;
	u32 v1 = 0, v2 = 0, pre_v1 = 0, pre_v2 = 0;
	int rc = 0, applied = 0;

	while ((i + 1) < array_len) {
		v1 = array[i];
		v2 = array[i + 1];

		if (v1 & ((1u << 31) | (1u << 30))) { /* positive & negative condition */
			if (v1 & (1u << 31)) {         /* positive condition */
				c_cond = (u8)((v1 & ((1u << 29) | (1u << 28))) >> 28);
				if (c_cond == COND_ENDIF) {
					is_matched = 1;
					is_skipped = 0;
				} else if (c_cond == COND_ELSE) {
					is_matched = is_skipped ? 0 : 1;
				} else { /* IF / ELSE IF */
					pre_v1 = v1;
					pre_v2 = v2;
				}
			} else if (v1 & (1u << 30)) {  /* negative condition */
				if (!is_skipped) {
					if (check_positive(pre_v1, pre_v2, v1, v2)) {
						is_matched = 1;
						is_skipped = 1;
					} else {
						is_matched = 0;
						is_skipped = 0;
					}
				} else {
					is_matched = 0;
				}
			}
		} else {
			if (is_matched) {
				int r = config_rf_reg(h, path, v1, v2);
				if (r < 0 && rc == 0)
					rc = r;
				applied++;
			}
		}
		i += 2;
	}

	if (verbose)
		printf("[rtl_rf] radio%c: %d Registerschreibvorgaenge (rc=%d)\n",
		       (path == RTL_RF_PATH_A) ? 'A' : 'B', applied, rc);
	return rc;
}

/* ===================================================================== *
 *  RF-Grundkonfiguration (PHY_RFConfig8812 / PHY_RF6052_Config_8812)
 * ===================================================================== */

int rtl_rf_init(libusb_device_handle *h, int verbose)
{
	int rc = 0, r;
	u32 lena = (u32)(sizeof(array_mp_8812a_radioa) / sizeof(u32));
	u32 lenb = (u32)(sizeof(array_mp_8812a_radiob) / sizeof(u32));

	if (verbose)
		printf("[rtl_rf] RF-Init: radioA (%u u32) auf Pfad A, radioB (%u u32) auf Pfad B\n",
		       lena, lenb);

	/* Pfad A: radioA-Tabelle */
	r = apply_rf_table(h, RTL_RF_PATH_A, array_mp_8812a_radioa, lena, verbose);
	if (r < 0 && rc == 0) rc = r;

#if (RTL_RF_NUM_PATHS >= 2)
	/* Pfad B: radioB-Tabelle */
	r = apply_rf_table(h, RTL_RF_PATH_B, array_mp_8812a_radiob, lenb, verbose);
	if (r < 0 && rc == 0) rc = r;
#endif

	if (verbose)
		printf("[rtl_rf] RF-Init fertig (rc=%d)\n", rc);
	return rc;
}

/* ===================================================================== *
 *  Band-Umschaltung (Teilportierung von PHY_SwitchWirelessBand8812,
 *  8812AU-Pfad, rfe_type 0)
 * ===================================================================== */

/* phy_SetRFEReg8812 fuer rfe_type 0 (AWUS036ACH-Standard). */
static void set_rfe_reg_8812(libusb_device_handle *h, int band)
{
	if (band == BAND_ON_2_4G) {
		bb_write(h, rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
		bb_write(h, rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77777777);
		bb_write(h, rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
		bb_write(h, rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x000);
	} else {
		bb_write(h, rA_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337717);
		bb_write(h, rB_RFE_Pinmux_Jaguar, bMaskDWord, 0x77337717);
		bb_write(h, rA_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
		bb_write(h, rB_RFE_Inv_Jaguar, bMask_RFEInv_Jaguar, 0x010);
	}
}

static void switch_band(libusb_device_handle *h, int band)
{
	if (band == BAND_ON_2_4G) {
		bb_write(h, rOFDMCCKEN_Jaguar, bOFDMEN_Jaguar | bCCKEN_Jaguar, 0x03);

		/* 8812: BWIndication + PD_TH_20M (Yn user guide) */
		bb_write(h, rBWIndication_Jaguar, 0x3, 0x1);
		bb_write(h, rPwed_TH_Jaguar, (1u<<13)|(1u<<14)|(1u<<15)|(1u<<16)|(1u<<17), 0x17);
		/* PWED_TH [3:1]: 2T2R -> 0x04 */
		bb_write(h, rPwed_TH_Jaguar, (1u<<1)|(1u<<2)|(1u<<3), 0x04);

		/* AGC-Tabellen-Auswahl (2.4G) */
		bb_write(h, rAGC_table_Jaguar, 0x3, 0);

		set_rfe_reg_8812(h, band);

		/* CCK-FA-Workaround (mp_mode == 0) */
		bb_write(h, rTxPath_Jaguar, 0xf0, 0x1);
		bb_write(h, rCCK_RX_Jaguar, 0x0f000000, 0x1);

		/* CCK_CHECK: BIT7 = 0 (2.4G) */
		rtl_write8(h, REG_CCK_CHECK_8812,
			   (uint8_t)(rtl_read8(h, REG_CCK_CHECK_8812, NULL) & ~(1u << 7)));
	} else { /* 5G */
		u16 count = 0, reg41A;

		/* CCK_CHECK: BIT7 = 1 (5G) */
		rtl_write8(h, REG_CCK_CHECK_8812,
			   (uint8_t)(rtl_read8(h, REG_CCK_CHECK_8812, NULL) | (1u << 7)));

		/* Warten bis TX-Queue leer (Reg41A[5:4] == 0x30), max. 50x50us */
		reg41A = rtl_read16(h, REG_TXPKT_EMPTY, NULL) & 0x30;
		while ((reg41A != 0x30) && (count < 50)) {
			udelay_us(50);
			reg41A = rtl_read16(h, REG_TXPKT_EMPTY, NULL) & 0x30;
			count++;
		}

		bb_write(h, rOFDMCCKEN_Jaguar, bOFDMEN_Jaguar | bCCKEN_Jaguar, 0x03);

		bb_write(h, rBWIndication_Jaguar, 0x3, 0x2);
		bb_write(h, rPwed_TH_Jaguar, (1u<<13)|(1u<<14)|(1u<<15)|(1u<<16)|(1u<<17), 0x15);
		bb_write(h, rPwed_TH_Jaguar, (1u<<1)|(1u<<2)|(1u<<3), 0x04);

		bb_write(h, rAGC_table_Jaguar, 0x3, 1);

		set_rfe_reg_8812(h, band);

		bb_write(h, rTxPath_Jaguar, 0xf0, 0x0);
		bb_write(h, rCCK_RX_Jaguar, 0x0f000000, 0xF);
	}

	/*
	 * phy_SetBBSwingByBand_8812A: Tx-BB-Swing. Ohne efuse-Auswertung setzen wir
	 * den sicheren Default 0 dB (0x200) fuer beide Pfade (0xC1C/0xE1C [31:21]).
	 * Feinabstimmung uebernimmt das Tx-Power-/Kalibriermodul.
	 */
	bb_write(h, rA_TxScale_Jaguar, 0xFFE00000, 0x200);
	bb_write(h, rB_TxScale_Jaguar, 0xFFE00000, 0x200);
}

/* ===================================================================== *
 *  Kanal-Umschaltung (phy_SwChnl8812) + Bandbreite 20 MHz
 *  (phy_PostSetBwMode8812 + PHY_RF6052SetBandwidth8812)
 * ===================================================================== */

static void sw_chnl(libusb_device_handle *h, int ch)
{
	int path;

	/* fc_area (0x860 [28:17]) */
	if (36 <= ch && ch <= 48)
		bb_write(h, rFc_area_Jaguar, 0x1ffe0000, 0x494);
	else if (15 <= ch && ch <= 35)
		bb_write(h, rFc_area_Jaguar, 0x1ffe0000, 0x494);
	else if (50 <= ch && ch <= 80)
		bb_write(h, rFc_area_Jaguar, 0x1ffe0000, 0x453);
	else if (82 <= ch && ch <= 116)
		bb_write(h, rFc_area_Jaguar, 0x1ffe0000, 0x452);
	else if (118 <= ch)
		bb_write(h, rFc_area_Jaguar, 0x1ffe0000, 0x412);
	else
		bb_write(h, rFc_area_Jaguar, 0x1ffe0000, 0x96a);

	for (path = 0; path < RTL_RF_NUM_PATHS; path++) {
		u32 rfmod_mask = (1u<<18)|(1u<<17)|(1u<<16)|(1u<<9)|(1u<<8);

		/* RF_MOD_AG (RF 0x18 [18:16],[9:8]) */
		if (36 <= ch && ch <= 80)
			rf_write_mask(h, path, RF_CHNLBW_Jaguar, rfmod_mask, 0x101);
		else if (15 <= ch && ch <= 35)
			rf_write_mask(h, path, RF_CHNLBW_Jaguar, rfmod_mask, 0x101);
		else if (82 <= ch && ch <= 140)
			rf_write_mask(h, path, RF_CHNLBW_Jaguar, rfmod_mask, 0x301);
		else if (140 < ch)
			rf_write_mask(h, path, RF_CHNLBW_Jaguar, rfmod_mask, 0x501);
		else
			rf_write_mask(h, path, RF_CHNLBW_Jaguar, rfmod_mask, 0x000);

		/* Spur-Fix (8812): 2.4G ADC-Clock. Bei 20 MHz. */
		if (ch == 13 || ch == 14)
			bb_write(h, rRFMOD_Jaguar, 0x300, 0x3);
		else if (ch <= 14)
			bb_write(h, rRFMOD_Jaguar, 0x300, 0x2);

		/* Kanalnummer (RF 0x18 [7:0]) */
		rf_write_mask(h, path, RF_CHNLBW_Jaguar, bMaskByte0, (u32)ch);
	}
}

/* phy_PostSetBwMode8812 fuer 20 MHz + PHY_RF6052SetBandwidth8812. */
static void set_bw_20(libusb_device_handle *h)
{
	int path;
	u16 trx;

	/* phy_SetRegBW_8812: REG_WMAC_TRXPTCL_CTL (0x668) BIT7=0, BIT8=0 */
	trx = rtl_read16(h, REG_WMAC_TRXPTCL_CTL, NULL);
	rtl_write16(h, REG_WMAC_TRXPTCL_CTL, (uint16_t)(trx & 0xFE7F));

	/* REG_DATA_SC (0x483) = 0 fuer 20 MHz */
	rtl_write8(h, REG_DATA_SC_8812, 0x00);

	/* 20-MHz-BB-Register */
	bb_write(h, rRFMOD_Jaguar, 0x003003C3, 0x00300200);
	bb_write(h, rADC_Buf_Clk_Jaguar, (1u << 30), 0);
#if RTL_RF_IS_2T2R
	bb_write(h, rL1PeakTH_Jaguar, 0x03C00000, 7);
#else
	bb_write(h, rL1PeakTH_Jaguar, 0x03C00000, 8);
#endif

	/* PHY_RF6052SetBandwidth8812: RF 0x18 [11:10] = 3 (20 MHz), beide Pfade */
	for (path = 0; path < RTL_RF_NUM_PATHS; path++)
		rf_write_mask(h, path, RF_CHNLBW_Jaguar, (1u << 11) | (1u << 10), 3);
}

int rtl_rf_set_channel(libusb_device_handle *h, int ch, int bw)
{
	int band;

	/* Nur 20 MHz vollstaendig implementiert. */
	if (bw != RTL_BW_20)
		return 1;

	/* Kanalgueltigkeit (2.4G 1..14, 5G 36..165). */
	if (!((ch >= 1 && ch <= 14) || (ch >= 36 && ch <= 165)))
		return 1;

	band = (ch > 14) ? BAND_ON_5G : BAND_ON_2_4G;

	/* 1) Band einstellen (idempotent bei jedem Aufruf angewandt). */
	switch_band(h, band);

	/* 2) Kanal setzen (fc_area + RF_MOD_AG + Kanalnummer je Pfad). */
	sw_chnl(h, ch);

	/* 3) Bandbreite 20 MHz (BB + RF). */
	set_bw_20(h);

	/*
	 * HINWEIS: PHY_SetTxPowerLevel8812 und IQK/LCK werden hier bewusst NICHT
	 * aufgerufen — Tx-Power und Kalibrierung gehoeren in separate Module und
	 * sollten nach dem Kanalwechsel ausgefuehrt werden.
	 */
	return 0;
}
