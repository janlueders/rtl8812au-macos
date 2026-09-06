/*
 * rtl_cal — RF calibration (IQK + LCK) for the RTL8812AU (macOS, libusb).
 *
 * INTEGRATION:
 *   This module performs the RF calibration of the RTL8812AU (Jaguar) in
 *   userspace via libusb. Order during init:
 *       power_on -> fw_download -> MAC-Init -> BB-Init -> RF-Init
 *       -> rtl_iqk(h, verbose)      (IQ calibration)      <== HERE
 *       -> rtl_lck(h, verbose)      (LC calibration)      <== HERE
 *       -> set_channel(...)
 *   That is, AFTER RF-Init and BEFORE the first set_channel. The band
 *   currently held in RF register 0x18 determines some register values; the
 *   RF-Init typically leaves the chip on a 2.4-GHz channel, which suits the
 *   init IQK.
 *
 * Ported (faithful) from the GPLv2 Linux driver aircrack-ng/rtl8812au:
 *   hal/phydm/halrf/rtl8812a/halrf_8812a_ce.c  (the CE variant actually
 *   compiled under Linux; the Makefile selects halrf_8812a_ce.o).
 *
 * Mapping of the original helpers:
 *   odm_read_4byte / odm_write_4byte  -> rtl_read32 / rtl_write32
 *   odm_write_1byte                   -> rtl_write8
 *   odm_get_bb_reg / odm_set_bb_reg   -> bb_get / bb_set (local, masked)
 *   odm_get_rf_reg / odm_set_rf_reg   -> rf_get / rf_set (local, masked,
 *                                        on top of rtl_rf_read / rtl_rf_write)
 *   ODM_delay_ms(n)                   -> usleep(n*1000)
 *
 * Struct fields of the original (dm->...) are replaced here by detected /
 * configurable values:
 *   support_interface  -> always USB (not PCIE)
 *   band_type          -> from RF-Reg 0x18 (channel <=14 => 2.4G, else 5G)
 *   band_width         -> only relevant for VDF; VDF is hard off in the original
 *   rfe_type           -> rtl_cal_rfe_type   (default 0)
 *   ext_pa (2G)        -> rtl_cal_ext_pa_2g  (default 0)
 *   ext_pa_5g          -> rtl_cal_ext_pa_5g  (default 0)
 *   rf->dpk_done       -> 0 (DP calibration is not run here)
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "rtl_usb.h"

/* RF register access from the separate rtl_rf module (path 0=A, 1=B). */
extern uint32_t rtl_rf_read (libusb_device_handle *h, int path, uint16_t reg_addr, int *rc);
extern int      rtl_rf_write(libusb_device_handle *h, int path, uint16_t reg_addr, uint32_t data);

/* ------------------------------------------------------------------------- */
/* Configurable board/RFE parameters (see rtl_cal.h).                         */
/* ------------------------------------------------------------------------- */
int rtl_cal_rfe_type  = 0;
int rtl_cal_ext_pa_2g = 0;
int rtl_cal_ext_pa_5g = 0;

/* ------------------------------------------------------------------------- */
/* Constants from the reference headers.                                     */
/* ------------------------------------------------------------------------- */
#ifndef BIT
#define BIT(n)              (1u << (n))
#endif
#define MASKDWORD           0xffffffffu    /* bMaskDWord                      */
#define RFREGOFFSETMASK     0x000fffffu    /* 20-bit RF register width        */

#define RF_PATH_A           0
#define RF_PATH_B           1

/* RF registers (offsets only, as in the original RF_0x.. resp. RF_CHNLBW/RF_LCK). */
#define RF_0x00             0x00
#define RF_0x08             0x08
#define RF_0x18             0x18           /* RF_CHNLBW_Jaguar (channel/BW)   */
#define RF_0x30             0x30
#define RF_0x31             0x31
#define RF_0x32             0x32
#define RF_0x58             0x58
#define RF_0x65             0x65
#define RF_0x8f             0x8f
#define RF_0xb4             0xB4           /* RF_LCK                          */
#define RF_0xef             0xef

/* MAC registers (byte accesses in the original via odm_write_1byte).        */
#define REG_TXPAUSE         0x0522
#define REG_SINGLE_TONE_CONT_TX_JAGUAR  0x0914

/* Band type (CE variant: ODM_BAND_2_4G=0, ODM_BAND_5G=1).                   */
#define BAND_2_4G           0
#define BAND_5G             1

/* Backup register lists (from _phy_iq_calibrate_8812a).                     */
#define MACBB_REG_NUM       9
#define AFE_REG_NUM         12
#define RF_REG_NUM          3
#define CAL_NUM             10          /* cal_num in the original           */

/* ------------------------------------------------------------------------- */
/* Error accounting + debug output.                                          */
/* ------------------------------------------------------------------------- */
static int g_cal_err;        /* accumulated USB errors during a run          */
static int g_cal_verbose;    /* debug output on/off                          */

#define RFDBG(...) do { if (g_cal_verbose) printf(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------- */
/* Delay helper (ODM_delay_ms / mdelay).                                     */
/* ------------------------------------------------------------------------- */
static void mdelay(unsigned ms) { usleep((useconds_t)ms * 1000u); }

/* ------------------------------------------------------------------------- */
/* Bit shift: position of the least significant set bit (PHY_CalculateBitShift). */
/* ------------------------------------------------------------------------- */
static uint32_t bit_shift(uint32_t mask)
{
	uint32_t i;
	for (i = 0; i < 32; i++)
		if (mask & (1u << i))
			return i;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* BB register access (0x800-0xFFF and MAC dwords via rtl_read32/rtl_write32). */
/* ------------------------------------------------------------------------- */
static uint32_t bb_read(libusb_device_handle *h, uint16_t addr)
{
	int rc = 0;
	uint32_t v = rtl_read32(h, addr, &rc);
	if (rc < 0) g_cal_err++;
	return v;
}

static void bb_write(libusb_device_handle *h, uint16_t addr, uint32_t val)
{
	if (rtl_write32(h, addr, val) != 0) g_cal_err++;
}

static void bb_write1(libusb_device_handle *h, uint16_t addr, uint8_t val)
{
	if (rtl_write8(h, addr, val) != 0) g_cal_err++;
}

/* odm_get_bb_reg */
static uint32_t bb_get(libusb_device_handle *h, uint16_t addr, uint32_t mask)
{
	uint32_t orig = bb_read(h, addr);
	return (orig & mask) >> bit_shift(mask);
}

/* odm_set_bb_reg */
static void bb_set(libusb_device_handle *h, uint16_t addr, uint32_t mask, uint32_t data)
{
	uint32_t val;
	if (mask != MASKDWORD) {
		uint32_t orig = bb_read(h, addr);
		val = (orig & ~mask) | ((data << bit_shift(mask)) & mask);
	} else {
		val = data;
	}
	bb_write(h, addr, val);
}

/* ------------------------------------------------------------------------- */
/* RF register access (masked), built on top of rtl_rf_read / rtl_rf_write.   */
/* ------------------------------------------------------------------------- */
/* odm_get_rf_reg */
static uint32_t rf_get(libusb_device_handle *h, int path, uint16_t reg, uint32_t mask)
{
	int rc = 0;
	uint32_t orig = rtl_rf_read(h, path, reg, &rc);
	if (rc < 0) g_cal_err++;
	return (orig & mask) >> bit_shift(mask);
}

/* odm_set_rf_reg */
static void rf_set(libusb_device_handle *h, int path, uint16_t reg, uint32_t mask, uint32_t data)
{
	uint32_t val;
	if (mask != RFREGOFFSETMASK) {
		int rc = 0;
		uint32_t orig = rtl_rf_read(h, path, reg, &rc);
		if (rc < 0) g_cal_err++;
		val = (orig & ~mask) | ((data << bit_shift(mask)) & mask);
	} else {
		val = data;
	}
	if (rtl_rf_write(h, path, reg, val & RFREGOFFSETMASK) != 0) g_cal_err++;
}

/* ========================================================================= */
/* IQK: Fill-IQC-Helfer                                                       */
/* ========================================================================= */

/* _iqk_rx_fill_iqc_8812a */
static void iqk_rx_fill_iqc(libusb_device_handle *h, int path,
			    unsigned int RX_X, unsigned int RX_Y)
{
	switch (path) {
	case RF_PATH_A:
		bb_set(h, 0x82c, BIT(31), 0x0); /* [31]=0 -> Page C */
		if (RX_X >> 1 >= 0x112 || (RX_Y >> 1 >= 0x12 && RX_Y >> 1 <= 0x3ee)) {
			bb_set(h, 0xc10, 0x000003ff, 0x100);
			bb_set(h, 0xc10, 0x03ff0000, 0);
		} else {
			bb_set(h, 0xc10, 0x000003ff, RX_X >> 1);
			bb_set(h, 0xc10, 0x03ff0000, RX_Y >> 1);
		}
		break;
	case RF_PATH_B:
		bb_set(h, 0x82c, BIT(31), 0x0); /* [31]=0 -> Page C */
		if (RX_X >> 1 >= 0x112 || (RX_Y >> 1 >= 0x12 && RX_Y >> 1 <= 0x3ee)) {
			bb_set(h, 0xe10, 0x000003ff, 0x100);
			bb_set(h, 0xe10, 0x03ff0000, 0);
		} else {
			bb_set(h, 0xe10, 0x000003ff, RX_X >> 1);
			bb_set(h, 0xe10, 0x03ff0000, RX_Y >> 1);
		}
		break;
	default:
		break;
	}
}

/* _iqk_tx_fill_iqc_8812a  (rf->dpk_done == 0 -> BIT(29) is set) */
static void iqk_tx_fill_iqc(libusb_device_handle *h, int path,
			    unsigned int TX_X, unsigned int TX_Y)
{
	const int dpk_done = 0;

	switch (path) {
	case RF_PATH_A:
		bb_set(h, 0x82c, BIT(31), 0x1); /* [31]=1 -> Page C1 */
		bb_set(h, 0xc90, BIT(7), 0x1);
		bb_set(h, 0xcc4, BIT(18), 0x1);
		if (!dpk_done)
			bb_set(h, 0xcc4, BIT(29), 0x1);
		bb_set(h, 0xcc8, BIT(29), 0x1);
		bb_set(h, 0xccc, 0x000007ff, TX_Y);
		bb_set(h, 0xcd4, 0x000007ff, TX_X);
		break;
	case RF_PATH_B:
		bb_set(h, 0x82c, BIT(31), 0x1); /* [31]=1 -> Page C1 */
		bb_set(h, 0xe90, BIT(7), 0x1);
		bb_set(h, 0xec4, BIT(18), 0x1);
		if (!dpk_done)
			bb_set(h, 0xec4, BIT(29), 0x1);
		bb_set(h, 0xec8, BIT(29), 0x1);
		bb_set(h, 0xecc, 0x000007ff, TX_Y);
		bb_set(h, 0xed4, 0x000007ff, TX_X);
		break;
	default:
		break;
	}
}

/* ========================================================================= */
/* IQK: Backup / Restore                                                      */
/* ========================================================================= */

/* _iqk_backup_mac_bb_8812a */
static void iqk_backup_mac_bb(libusb_device_handle *h, uint32_t *macbb_backup,
			      const uint32_t *reg, uint32_t num)
{
	uint32_t i;
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	for (i = 0; i < num; i++)
		macbb_backup[i] = bb_read(h, (uint16_t)reg[i]);
	RFDBG("[cal] BackupMacBB\n");
}

/* _iqk_backup_rf_8812a */
static void iqk_backup_rf(libusb_device_handle *h, uint32_t *rfa_backup,
			  uint32_t *rfb_backup, const uint32_t *reg, uint32_t num)
{
	uint32_t i;
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	for (i = 0; i < num; i++) {
		rfa_backup[i] = rf_get(h, RF_PATH_A, (uint16_t)reg[i], MASKDWORD);
		rfb_backup[i] = rf_get(h, RF_PATH_B, (uint16_t)reg[i], MASKDWORD);
	}
	RFDBG("[cal] BackupRF\n");
}

/* _iqk_backup_afe_8812a */
static void iqk_backup_afe(libusb_device_handle *h, uint32_t *afe_backup,
			   const uint32_t *reg, uint32_t num)
{
	uint32_t i;
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	for (i = 0; i < num; i++)
		afe_backup[i] = bb_read(h, (uint16_t)reg[i]);
	RFDBG("[cal] BackupAFE\n");
}

/* _iqk_restore_mac_bb_8812a */
static void iqk_restore_mac_bb(libusb_device_handle *h, const uint32_t *macbb_backup,
			       const uint32_t *reg, uint32_t num)
{
	uint32_t i;
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	for (i = 0; i < num; i++)
		bb_write(h, (uint16_t)reg[i], macbb_backup[i]);
	RFDBG("[cal] RestoreMacBB\n");
}

/* _iqk_restore_rf_8812a */
static void iqk_restore_rf(libusb_device_handle *h, int path,
			   const uint32_t *reg, const uint32_t *rf_backup, uint32_t num)
{
	uint32_t i;
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	for (i = 0; i < num; i++)
		rf_set(h, path, (uint16_t)reg[i], RFREGOFFSETMASK, rf_backup[i]);
	rf_set(h, path, RF_0xef, RFREGOFFSETMASK, 0x0);
	RFDBG("[cal] RestoreRF path %c\n", path == RF_PATH_A ? 'A' : 'B');
}

/* _iqk_restore_afe_8812a  (rf->dpk_done == 0) */
static void iqk_restore_afe(libusb_device_handle *h, const uint32_t *afe_backup,
			    const uint32_t *reg, uint32_t num)
{
	const int dpk_done = 0;
	uint32_t i;

	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	for (i = 0; i < num; i++)
		bb_write(h, (uint16_t)reg[i], afe_backup[i]);

	bb_set(h, 0x82c, BIT(31), 0x1); /* Page C1 */
	bb_write(h, 0xc80, 0x0);
	bb_write(h, 0xc84, 0x0);
	bb_write(h, 0xc88, 0x0);
	bb_write(h, 0xc8c, 0x3c000000);
	bb_set(h, 0xc90, BIT(7), 0x1);
	bb_set(h, 0xcc4, BIT(18), 0x1);
	if (!dpk_done)
		bb_set(h, 0xcc4, BIT(29), 0x1);
	bb_set(h, 0xcc8, BIT(29), 0x1);
	bb_write(h, 0xe80, 0x0);
	bb_write(h, 0xe84, 0x0);
	bb_write(h, 0xe88, 0x0);
	bb_write(h, 0xe8c, 0x3c000000);
	bb_set(h, 0xe90, BIT(7), 0x1);
	bb_set(h, 0xec4, BIT(18), 0x1);
	if (!dpk_done)
		bb_set(h, 0xec4, BIT(29), 0x1);
	bb_set(h, 0xec8, BIT(29), 0x1);
	RFDBG("[cal] RestoreAFE\n");
}

/* _iqk_configure_mac_8812a */
static void iqk_configure_mac(libusb_device_handle *h)
{
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	bb_write1(h, 0x522, 0x3f);
	bb_set(h, 0x550, BIT(11) | BIT(3), 0x0);
	bb_write1(h, 0x808, 0x00);       /* RX ante off */
	bb_set(h, 0x838, 0xf, 0xc);      /* CCA off */
	bb_write1(h, 0xa07, 0xf);        /* CCK RX path off */
}

/* ========================================================================= */
/* IQK: core routine (_iqk_tx_8812a)                                          */
/*   Contains LOK + TX-IQK + RX-IQK for path A and B, incl. averaging.        */
/* ========================================================================= */
static void iqk_tx(libusb_device_handle *h, int is_5g)
{
	uint8_t delay_count, cal0_retry, cal1_retry;
	uint8_t tx0_average = 0, tx1_average = 0, rx0_average = 0, rx1_average = 0;
	int TX_IQC_temp[10][4], TX_IQC[4] = {0, 0, 0, 0};
	int RX_IQC_temp[10][4], RX_IQC[4] = {0, 0, 0, 0};
	int TX0_fail = 1, RX0_fail = 1, IQK0_ready = 0, TX0_finish = 0, RX0_finish = 0;
	int TX1_fail = 1, RX1_fail = 1, IQK1_ready = 0, TX1_finish = 0, RX1_finish = 0;
	int VDF_enable = 0;   /* in the original: derived from band_width, then hard 0 */
	int i, ii, dx = 0, dy = 0;

	const int rfe_type   = rtl_cal_rfe_type;
	const int ext_pa_2g  = rtl_cal_ext_pa_2g;
	const int ext_pa_5g  = rtl_cal_ext_pa_5g;
	/* support_interface: USB (never PCIE) */

	(void)VDF_enable;

	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */

	/* ======== path-A/B AFE all on ======== */
	bb_write(h, 0xc60, 0x77777777);
	bb_write(h, 0xc64, 0x77777777);
	bb_write(h, 0xe60, 0x77777777);
	bb_write(h, 0xe64, 0x77777777);
	bb_write(h, 0xc68, 0x19791979);
	bb_write(h, 0xe68, 0x19791979);
	bb_set(h, 0xc00, 0xf, 0x4);     /* hardware 3-wire off */
	bb_set(h, 0xe00, 0xf, 0x4);     /* hardware 3-wire off */

	/* DAC/ADC sampling rate (160 MHz) */
	bb_set(h, 0xc5c, BIT(26) | BIT(25) | BIT(24), 0x7);
	bb_set(h, 0xe5c, BIT(26) | BIT(25) | BIT(24), 0x7);

	/* ====== path A/B TX IQK RF setting ====== */
	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	rf_set(h, RF_PATH_A, RF_0xef, RFREGOFFSETMASK, 0x80002);
	rf_set(h, RF_PATH_A, RF_0x30, RFREGOFFSETMASK, 0x20000);
	rf_set(h, RF_PATH_A, RF_0x31, RFREGOFFSETMASK, 0x3fffd);
	rf_set(h, RF_PATH_A, RF_0x32, RFREGOFFSETMASK, 0xfe83f);
	rf_set(h, RF_PATH_A, RF_0x65, RFREGOFFSETMASK, 0x931d5);
	rf_set(h, RF_PATH_A, RF_0x8f, RFREGOFFSETMASK, 0x8a001);
	rf_set(h, RF_PATH_B, RF_0xef, RFREGOFFSETMASK, 0x80002);
	rf_set(h, RF_PATH_B, RF_0x30, RFREGOFFSETMASK, 0x20000);
	rf_set(h, RF_PATH_B, RF_0x31, RFREGOFFSETMASK, 0x3fffd);
	rf_set(h, RF_PATH_B, RF_0x32, RFREGOFFSETMASK, 0xfe83f);
	rf_set(h, RF_PATH_B, RF_0x65, RFREGOFFSETMASK, 0x931d5);
	rf_set(h, RF_PATH_B, RF_0x8f, RFREGOFFSETMASK, 0x8a001);
	bb_write(h, 0x90c, 0x00008000);
	bb_set(h, 0xc94, BIT(0), 0x1);
	bb_set(h, 0xe94, BIT(0), 0x1);
	bb_write(h, 0x978, 0x29002000);  /* TX (X,Y) */
	bb_write(h, 0x97c, 0xa9002000);  /* RX (X,Y) */
	bb_write(h, 0x984, 0x00462910);  /* [0]:AGC_en, [15]:idac_K_Mask */
	bb_set(h, 0x82c, BIT(31), 0x1);  /* Page C1 */

	if (ext_pa_5g) {
		if (rfe_type == 1) {
			bb_write(h, 0xc88, 0x821403e3);
			bb_write(h, 0xe88, 0x821403e3);
		} else {
			bb_write(h, 0xc88, 0x821403f7);
			bb_write(h, 0xe88, 0x821403f7);
		}
	} else {
		bb_write(h, 0xc88, 0x821403f1);
		bb_write(h, 0xe88, 0x821403f1);
	}
	if (is_5g) {
		bb_write(h, 0xc8c, 0x68163e96);
		bb_write(h, 0xe8c, 0x68163e96);
	} else {
		bb_write(h, 0xc8c, 0x28163e96);
		bb_write(h, 0xe8c, 0x28163e96);
		if (rfe_type == 3) {
			if (ext_pa_2g)
				bb_write(h, 0xc88, 0x821403e3);
			else
				bb_write(h, 0xc88, 0x821403f7);
		}
	}

	if (VDF_enable) {
		/* VDF is disabled in the original; no code. */
	} else {
		bb_write(h, 0xc80, 0x18008c10); /* TX_Tone_idx[9:0], TxK_Mask[29] */
		bb_write(h, 0xc84, 0x38008c10); /* RX_Tone_idx[9:0], RxK_Mask[29] */
		bb_write(h, 0xce8, 0x00000000);
		bb_write(h, 0xe80, 0x18008c10);
		bb_write(h, 0xe84, 0x38008c10);
		bb_write(h, 0xee8, 0x00000000);

		cal0_retry = 0;
		cal1_retry = 0;
		while (1) {
			/* one shot */
			bb_write(h, 0xcb8, 0x00100000);
			bb_write(h, 0xeb8, 0x00100000);
			bb_write(h, 0x980, 0xfa000000);
			bb_write(h, 0x980, 0xf8000000);

			mdelay(10);
			bb_write(h, 0xcb8, 0x00000000);
			bb_write(h, 0xeb8, 0x00000000);
			delay_count = 0;
			while (1) {
				if (!TX0_finish)
					IQK0_ready = (int)bb_get(h, 0xd00, BIT(10));
				if (!TX1_finish)
					IQK1_ready = (int)bb_get(h, 0xd40, BIT(10));
				if ((IQK0_ready && IQK1_ready) || delay_count > 20)
					break;
				mdelay(1);
				delay_count++;
			}
			RFDBG("[cal] TX delay_count = %d\n", delay_count);
			if (delay_count < 20) { /* otherwise cal_retry++ */
				TX0_fail = (int)bb_get(h, 0xd00, BIT(12));
				TX1_fail = (int)bb_get(h, 0xd40, BIT(12));
				if (!(TX0_fail || TX0_finish)) {
					bb_write(h, 0xcb8, 0x02000000);
					TX_IQC_temp[tx0_average][0] = (int)(bb_get(h, 0xd00, 0x07ff0000) << 21);
					bb_write(h, 0xcb8, 0x04000000);
					TX_IQC_temp[tx0_average][1] = (int)(bb_get(h, 0xd00, 0x07ff0000) << 21);
					tx0_average++;
				} else {
					cal0_retry++;
					if (cal0_retry == 10)
						break;
				}
				if (!(TX1_fail || TX1_finish)) {
					bb_write(h, 0xeb8, 0x02000000);
					TX_IQC_temp[tx1_average][2] = (int)(bb_get(h, 0xd40, 0x07ff0000) << 21);
					bb_write(h, 0xeb8, 0x04000000);
					TX_IQC_temp[tx1_average][3] = (int)(bb_get(h, 0xd40, 0x07ff0000) << 21);
					tx1_average++;
				} else {
					cal1_retry++;
					if (cal1_retry == 10)
						break;
				}
			} else {
				cal0_retry++;
				cal1_retry++;
				RFDBG("[cal] delay 20ms TX IQK Not Ready!\n");
				if (cal0_retry == 10)
					break;
			}
			if (tx0_average >= 2) {
				for (i = 0; i < tx0_average; i++) {
					for (ii = i + 1; ii < tx0_average; ii++) {
						dx = (TX_IQC_temp[i][0] >> 21) - (TX_IQC_temp[ii][0] >> 21);
						if (dx < 4 && dx > -4) {
							dy = (TX_IQC_temp[i][1] >> 21) - (TX_IQC_temp[ii][1] >> 21);
							if (dy < 4 && dy > -4) {
								TX_IQC[0] = ((TX_IQC_temp[i][0] >> 21) + (TX_IQC_temp[ii][0] >> 21)) / 2;
								TX_IQC[1] = ((TX_IQC_temp[i][1] >> 21) + (TX_IQC_temp[ii][1] >> 21)) / 2;
								TX0_finish = 1;
							}
						}
					}
				}
			}
			if (tx1_average >= 2) {
				for (i = 0; i < tx1_average; i++) {
					for (ii = i + 1; ii < tx1_average; ii++) {
						dx = (TX_IQC_temp[i][2] >> 21) - (TX_IQC_temp[ii][2] >> 21);
						if (dx < 4 && dx > -4) {
							dy = (TX_IQC_temp[i][3] >> 21) - (TX_IQC_temp[ii][3] >> 21);
							if (dy < 4 && dy > -4) {
								TX_IQC[2] = ((TX_IQC_temp[i][2] >> 21) + (TX_IQC_temp[ii][2] >> 21)) / 2;
								TX_IQC[3] = ((TX_IQC_temp[i][3] >> 21) + (TX_IQC_temp[ii][3] >> 21)) / 2;
								TX1_finish = 1;
							}
						}
					}
				}
			}
			RFDBG("[cal] tx0_average=%d tx1_average=%d TX0_finish=%d TX1_finish=%d\n",
			      tx0_average, tx1_average, TX0_finish, TX1_finish);
			if (TX0_finish && TX1_finish)
				break;
			if ((cal0_retry + tx0_average) >= 10 || (cal1_retry + tx1_average) >= 10)
				break;
		}
		RFDBG("[cal] TXA_cal_retry=%d TXB_cal_retry=%d\n", cal0_retry, cal1_retry);
	}

	bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
	rf_set(h, RF_PATH_A, RF_0x58, 0x7fe00, rf_get(h, RF_PATH_A, RF_0x08, 0xffc00)); /* Load LOK */
	rf_set(h, RF_PATH_B, RF_0x58, 0x7fe00, rf_get(h, RF_PATH_B, RF_0x08, 0xffc00)); /* Load LOK */
	bb_set(h, 0x82c, BIT(31), 0x1); /* Page C1 */

	if (VDF_enable == 1) {
		/* VDF disabled */
	} else {
		bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
		if (TX0_finish) {
			/* ====== path A RX IQK RF setting ====== */
			rf_set(h, RF_PATH_A, RF_0xef, RFREGOFFSETMASK, 0x80000);
			rf_set(h, RF_PATH_A, RF_0x30, RFREGOFFSETMASK, 0x30000);
			rf_set(h, RF_PATH_A, RF_0x31, RFREGOFFSETMASK, 0x3f7ff);
			rf_set(h, RF_PATH_A, RF_0x32, RFREGOFFSETMASK, 0xfe7bf);
			rf_set(h, RF_PATH_A, RF_0x8f, RFREGOFFSETMASK, 0x88001);
			rf_set(h, RF_PATH_A, RF_0x65, RFREGOFFSETMASK, 0x931d1);
			rf_set(h, RF_PATH_A, RF_0xef, RFREGOFFSETMASK, 0x00000);
		}
		if (TX1_finish) {
			/* ====== path B RX IQK RF setting ====== */
			rf_set(h, RF_PATH_B, RF_0xef, RFREGOFFSETMASK, 0x80000);
			rf_set(h, RF_PATH_B, RF_0x30, RFREGOFFSETMASK, 0x30000);
			rf_set(h, RF_PATH_B, RF_0x31, RFREGOFFSETMASK, 0x3f7ff);
			rf_set(h, RF_PATH_B, RF_0x32, RFREGOFFSETMASK, 0xfe7bf);
			rf_set(h, RF_PATH_B, RF_0x8f, RFREGOFFSETMASK, 0x88001);
			rf_set(h, RF_PATH_B, RF_0x65, RFREGOFFSETMASK, 0x931d1);
			rf_set(h, RF_PATH_B, RF_0xef, RFREGOFFSETMASK, 0x00000);
		}
		bb_set(h, 0x978, BIT(31), 0x1);
		bb_set(h, 0x97c, BIT(31), 0x0);
		bb_write(h, 0x90c, 0x00008000);
		/* support_interface == USB -> not PCIE */
		bb_write(h, 0x984, 0x0046a890);
		if (rfe_type == 1) {
			bb_write(h, 0xcb0, 0x77777717);
			bb_write(h, 0xcb4, 0x00000077);
			bb_write(h, 0xeb0, 0x77777717);
			bb_write(h, 0xeb4, 0x00000077);
		} else {
			bb_write(h, 0xcb0, 0x77777717);
			bb_write(h, 0xcb4, 0x02000077);
			bb_write(h, 0xeb0, 0x77777717);
			bb_write(h, 0xeb4, 0x02000077);
		}

		bb_set(h, 0x82c, BIT(31), 0x1); /* Page C1 */
		if (TX0_finish) {
			bb_write(h, 0xc80, 0x38008c10);
			bb_write(h, 0xc84, 0x18008c10);
			bb_write(h, 0xc88, 0x82140119);
		}
		if (TX1_finish) {
			bb_write(h, 0xe80, 0x38008c10);
			bb_write(h, 0xe84, 0x18008c10);
			bb_write(h, 0xe88, 0x82140119);
		}
		cal0_retry = 0;
		cal1_retry = 0;
		while (1) {
			/* one shot */
			bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
			if (TX0_finish) {
				bb_set(h, 0x978, 0x03FF8000, (uint32_t)TX_IQC[0] & 0x000007ff);
				bb_set(h, 0x978, 0x000007FF, (uint32_t)TX_IQC[1] & 0x000007ff);
				bb_set(h, 0x82c, BIT(31), 0x1); /* Page C1 */
				if (rfe_type == 1)
					bb_write(h, 0xc8c, 0x28161500);
				else
					bb_write(h, 0xc8c, 0x28160cc0);
				bb_write(h, 0xcb8, 0x00300000);
				bb_write(h, 0xcb8, 0x00100000);
				mdelay(5);
				bb_write(h, 0xc8c, 0x3c000000);
				bb_write(h, 0xcb8, 0x00000000);
			}
			if (TX1_finish) {
				bb_set(h, 0x82c, BIT(31), 0x0); /* Page C */
				bb_set(h, 0x978, 0x03FF8000, (uint32_t)TX_IQC[2] & 0x000007ff);
				bb_set(h, 0x978, 0x000007FF, (uint32_t)TX_IQC[3] & 0x000007ff);
				bb_set(h, 0x82c, BIT(31), 0x1); /* Page C1 */
				if (rfe_type == 1)
					bb_write(h, 0xe8c, 0x28161500);
				else
					bb_write(h, 0xe8c, 0x28160ca0);
				bb_write(h, 0xeb8, 0x00300000);
				bb_write(h, 0xeb8, 0x00100000);
				mdelay(5);
				bb_write(h, 0xe8c, 0x3c000000);
				bb_write(h, 0xeb8, 0x00000000);
			}
			delay_count = 0;
			while (1) {
				if (!RX0_finish && TX0_finish)
					IQK0_ready = (int)bb_get(h, 0xd00, BIT(10));
				if (!RX1_finish && TX1_finish)
					IQK1_ready = (int)bb_get(h, 0xd40, BIT(10));
				if ((IQK0_ready && IQK1_ready) || delay_count > 20)
					break;
				mdelay(1);
				delay_count++;
			}
			RFDBG("[cal] RX delay_count = %d\n", delay_count);
			if (delay_count < 20) { /* otherwise cal_retry++ */
				RX0_fail = (int)bb_get(h, 0xd00, BIT(11));
				RX1_fail = (int)bb_get(h, 0xd40, BIT(11));
				if (!(RX0_fail || RX0_finish) && TX0_finish) {
					bb_write(h, 0xcb8, 0x06000000);
					RX_IQC_temp[rx0_average][0] = (int)(bb_get(h, 0xd00, 0x07ff0000) << 21);
					bb_write(h, 0xcb8, 0x08000000);
					RX_IQC_temp[rx0_average][1] = (int)(bb_get(h, 0xd00, 0x07ff0000) << 21);
					rx0_average++;
				} else {
					cal0_retry++;
					if (cal0_retry == 10)
						break;
				}
				if (!(RX1_fail || RX1_finish) && TX1_finish) {
					bb_write(h, 0xeb8, 0x06000000);
					RX_IQC_temp[rx1_average][2] = (int)(bb_get(h, 0xd40, 0x07ff0000) << 21);
					bb_write(h, 0xeb8, 0x08000000);
					RX_IQC_temp[rx1_average][3] = (int)(bb_get(h, 0xd40, 0x07ff0000) << 21);
					rx1_average++;
				} else {
					cal1_retry++;
					if (cal1_retry == 10)
						break;
				}
			} else {
				cal0_retry++;
				cal1_retry++;
				RFDBG("[cal] delay 20ms RX IQK Not Ready!\n");
				if (cal0_retry == 10)
					break;
			}
			if (rx0_average >= 2) {
				for (i = 0; i < rx0_average; i++) {
					for (ii = i + 1; ii < rx0_average; ii++) {
						dx = (RX_IQC_temp[i][0] >> 21) - (RX_IQC_temp[ii][0] >> 21);
						if (dx < 4 && dx > -4) {
							dy = (RX_IQC_temp[i][1] >> 21) - (RX_IQC_temp[ii][1] >> 21);
							if (dy < 4 && dy > -4) {
								RX_IQC[0] = ((RX_IQC_temp[i][0] >> 21) + (RX_IQC_temp[ii][0] >> 21)) / 2;
								RX_IQC[1] = ((RX_IQC_temp[i][1] >> 21) + (RX_IQC_temp[ii][1] >> 21)) / 2;
								RX0_finish = 1;
								break;
							}
						}
					}
				}
			}
			if (rx1_average >= 2) {
				for (i = 0; i < rx1_average; i++) {
					for (ii = i + 1; ii < rx1_average; ii++) {
						dx = (RX_IQC_temp[i][2] >> 21) - (RX_IQC_temp[ii][2] >> 21);
						if (dx < 4 && dx > -4) {
							dy = (RX_IQC_temp[i][3] >> 21) - (RX_IQC_temp[ii][3] >> 21);
							if (dy < 4 && dy > -4) {
								RX_IQC[2] = ((RX_IQC_temp[i][2] >> 21) + (RX_IQC_temp[ii][2] >> 21)) / 2;
								RX_IQC[3] = ((RX_IQC_temp[i][3] >> 21) + (RX_IQC_temp[ii][3] >> 21)) / 2;
								RX1_finish = 1;
								break;
							}
						}
					}
				}
			}
			RFDBG("[cal] rx0_average=%d rx1_average=%d RX0_finish=%d RX1_finish=%d\n",
			      rx0_average, rx1_average, RX0_finish, RX1_finish);
			if ((RX0_finish || !TX0_finish) && (RX1_finish || !TX1_finish))
				break;
			if ((cal0_retry + rx0_average) >= 10 || (cal1_retry + rx1_average) >= 10 ||
			    rx0_average == 3 || rx1_average == 3)
				break;
		}
		RFDBG("[cal] RXA_cal_retry=%d RXB_cal_retry=%d\n", cal0_retry, cal1_retry);
	}

	/* FillIQK Result */
	RFDBG("[cal] ==== Fill Path A ====\n");
	if (TX0_finish)
		iqk_tx_fill_iqc(h, RF_PATH_A, (unsigned)TX_IQC[0], (unsigned)TX_IQC[1]);
	else
		iqk_tx_fill_iqc(h, RF_PATH_A, 0x200, 0x0);
	if (RX0_finish)
		iqk_rx_fill_iqc(h, RF_PATH_A, (unsigned)RX_IQC[0], (unsigned)RX_IQC[1]);
	else
		iqk_rx_fill_iqc(h, RF_PATH_A, 0x200, 0x0);

	RFDBG("[cal] ==== Fill Path B ====\n");
	if (TX1_finish)
		iqk_tx_fill_iqc(h, RF_PATH_B, (unsigned)TX_IQC[2], (unsigned)TX_IQC[3]);
	else
		iqk_tx_fill_iqc(h, RF_PATH_B, 0x200, 0x0);
	if (RX1_finish)
		iqk_rx_fill_iqc(h, RF_PATH_B, (unsigned)RX_IQC[2], (unsigned)RX_IQC[3]);
	else
		iqk_rx_fill_iqc(h, RF_PATH_B, 0x200, 0x0);
}

/* ========================================================================= */
/* _phy_iq_calibrate_8812a : Backup -> Configure -> IQK -> Restore           */
/* ========================================================================= */
static void phy_iq_calibrate(libusb_device_handle *h, int is_5g)
{
	uint32_t MACBB_backup[MACBB_REG_NUM];
	uint32_t AFE_backup[AFE_REG_NUM] = {0};
	uint32_t RFA_backup[RF_REG_NUM] = {0};
	uint32_t RFB_backup[RF_REG_NUM] = {0};
	const uint32_t backup_macbb_reg[MACBB_REG_NUM] =
		{0x520, 0x550, 0x808, 0xa04, 0x90c, 0xc00, 0xe00, 0x838, 0x82c};
	const uint32_t backup_afe_reg[AFE_REG_NUM] =
		{0xc5c, 0xc60, 0xc64, 0xc68, 0xcb0, 0xcb4,
		 0xe5c, 0xe60, 0xe64, 0xe68, 0xeb0, 0xeb4};
	const uint32_t backup_rf_reg[RF_REG_NUM] = {0x65, 0x8f, 0x0};
	uint32_t reg_c1b8, reg_e1b8;

	iqk_backup_mac_bb(h, MACBB_backup, backup_macbb_reg, MACBB_REG_NUM);
	bb_set(h, 0x82c, BIT(31), 0x1);
	reg_c1b8 = bb_read(h, 0xcb8);
	reg_e1b8 = bb_read(h, 0xeb8);
	bb_set(h, 0x82c, BIT(31), 0x0);
	iqk_backup_afe(h, AFE_backup, backup_afe_reg, AFE_REG_NUM);
	iqk_backup_rf(h, RFA_backup, RFB_backup, backup_rf_reg, RF_REG_NUM);

	iqk_configure_mac(h);
	iqk_tx(h, is_5g);
	iqk_restore_rf(h, RF_PATH_A, backup_rf_reg, RFA_backup, RF_REG_NUM);
	iqk_restore_rf(h, RF_PATH_B, backup_rf_reg, RFB_backup, RF_REG_NUM);

	iqk_restore_afe(h, AFE_backup, backup_afe_reg, AFE_REG_NUM);
	bb_set(h, 0x82c, BIT(31), 0x1);
	bb_write(h, 0xcb8, reg_c1b8);
	bb_write(h, 0xeb8, reg_e1b8);
	bb_set(h, 0x82c, BIT(31), 0x0);
	iqk_restore_mac_bb(h, MACBB_backup, backup_macbb_reg, MACBB_REG_NUM);
}

/* ========================================================================= */
/* _phy_lc_calibrate_8812a                                                    */
/* ========================================================================= */
static void phy_lc_calibrate(libusb_device_handle *h)
{
	uint32_t reg0x914, lc_cal, tmp;

	/* Check continuous TX and Packet TX (0x914[18:16]) */
	reg0x914 = bb_read(h, REG_SINGLE_TONE_CONT_TX_JAGUAR);

	/* Backup RF reg18. */
	lc_cal = rf_get(h, RF_PATH_A, RF_0x18, RFREGOFFSETMASK);

	if ((reg0x914 & 0x70000) != 0) {
		/* ContTx: per the original, do NOT turn off (workaround). */
	} else {
		/* Packet Tx-ing: pause Tx. */
		bb_write1(h, REG_TXPAUSE, 0xFF);
	}

	/* Enter LCK mode */
	tmp = rf_get(h, RF_PATH_A, RF_0xb4, RFREGOFFSETMASK);
	rf_set(h, RF_PATH_A, RF_0xb4, RFREGOFFSETMASK, tmp | BIT(14));

	/* Read RF reg18 */
	lc_cal = rf_get(h, RF_PATH_A, RF_0x18, RFREGOFFSETMASK);

	/* Set LC calibration begin bit15 */
	rf_set(h, RF_PATH_A, RF_0x18, RFREGOFFSETMASK, lc_cal | 0x08000);

	mdelay(150); /* suggest by RFSI Binson */

	/* Leave LCK mode */
	tmp = rf_get(h, RF_PATH_A, RF_0xb4, RFREGOFFSETMASK);
	rf_set(h, RF_PATH_A, RF_0xb4, RFREGOFFSETMASK, tmp & ~BIT(14));

	/* Restore original situation.
	 * Note: the original checks (reg0x914 & 70000) here -- a deliberately
	 * carried-over quirk (decimal 70000, not 0x70000). Faithfully ported. */
	if ((reg0x914 & 70000) != 0) {
		/* ContTx case: no restore (workaround). */
	} else {
		bb_write1(h, REG_TXPAUSE, 0x00);
	}

	/* Recover channel number */
	rf_set(h, RF_PATH_A, RF_0x18, RFREGOFFSETMASK, lc_cal);
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

/* Determine the current band from RF-Reg 0x18 (channel in byte0). */
static int cal_is_5g(libusb_device_handle *h)
{
	uint32_t chnl = rf_get(h, RF_PATH_A, RF_0x18, 0xff);
	return (chnl > 14) ? 1 : 0;
}

int rtl_iqk(libusb_device_handle *h, int verbose)
{
	int is_5g;

	g_cal_err = 0;
	g_cal_verbose = verbose;

	is_5g = cal_is_5g(h);
	RFDBG("[cal] IQK start (band=%s, rfe_type=%d, ext_pa_2g=%d, ext_pa_5g=%d)\n",
	      is_5g ? "5G" : "2.4G", rtl_cal_rfe_type, rtl_cal_ext_pa_2g, rtl_cal_ext_pa_5g);

	phy_iq_calibrate(h, is_5g);

	RFDBG("[cal] IQK done (usb_errors=%d)\n", g_cal_err);
	return g_cal_err ? -1 : 0;
}

int rtl_lck(libusb_device_handle *h, int verbose)
{
	g_cal_err = 0;
	g_cal_verbose = verbose;

	RFDBG("[cal] LCK start\n");
	phy_lc_calibrate(h);
	RFDBG("[cal] LCK done (usb_errors=%d)\n", g_cal_err);
	return g_cal_err ? -1 : 0;
}
