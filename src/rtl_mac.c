/*
 * rtl_mac.c — MAC initialization for the RTL8812AU (macOS, libusb, userspace).
 *
 * ===========================================================================
 * INTEGRATION:
 * ---------------------------------------------------------------------------
 * Call order (after the chip is running and the FW is loaded):
 *
 *     rtl_open_first(...)        // USB handle, interface 0 claimed
 *     rtl_power_on(h, ...)       // CARDEMU_TO_ACT power-on sequence
 *     rtl_fw_download(h, ...)    // load NIC firmware
 *     rtl_mac_init(h, verbose);       // <-- THIS MODULE: full MAC bring-up
 *     rtl_mac_set_monitor(h, verbose);// <-- THIS MODULE: monitor-mode RX
 *     // afterwards: BB config (PHY_BBConfig8812), RF config, channel selection, RX URBs
 *
 * PRECONDITIONS:
 *   - The chip must be powered on before rtl_mac_init (rtl_power_on ok) and
 *     the firmware must already be downloaded/booting (rtl_fw_download ok).
 *     rtl_mac_init itself does NOT power the chip on.
 *   - Interface 0 must be claimed (register access via vendor requests).
 *
 * ORDERING NOTES (important!):
 *   - rtl_mac_init MUST run before BB/RF initialization. In the Linux HAL
 *     PHY_BBConfig8812/PHY_RFConfig follows ONLY after the MAC bring-up and after
 *     the final "CR |= MACTXEN|MACRXEN". Keep this order here.
 *   - Call rtl_mac_set_monitor AFTER rtl_mac_init (it overwrites the RCR set
 *     in rtl_mac_init via _InitWMACSetting with the promisc value and
 *     sets RXFLTMAP0/1/2 = 0xFFFF).
 *   - The endpoint count (bulk-OUT) is determined via libusb from the active
 *     config descriptor (matches _ConfigChipOutEP_8812). If the
 *     determination fails, 4 OUT-EPs are assumed (default for 8812AU).
 *
 * Ported (GPLv2 source, reference/rtl8812au — read only):
 *   - array_mp_8812a_mac_reg[] : halhwimg8812a_mac.c (taken VERBATIM)
 *   - Apply loop odm_read_and_config_mp_8812a_mac_reg (USB branch for 0x011)
 *   - usb_halinit.c: _InitQueueReservedPage/_InitTxBufferBoundary/
 *     _InitQueuePriority/_InitPageBoundary/_InitTransferPageSize/
 *     _InitDriverInfoSize/_InitNetworkType/_InitWMACSetting/_InitAdaptiveCtrl/
 *     _InitEDCA/_InitRetryFunction/_InitBurstPktLen (USB variants)
 *   - rtl8812a_hal_init.c: InitLLTTable8812A, _InitBeaconParameters_8812A,
 *     hw_var_set_monitor (Monitor-RCR)
 * ===========================================================================
 */

#include <stdio.h>
#include "rtl_usb.h"
#include "rtl_mac.h"

/* Types for the verbatim-adopted table. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

/* ===========================================================================
 * Register constants (from include/hal_com_reg.h, rtl8812a_spec.h,
 * rtl8812a_hal.h of the reference driver).
 * =========================================================================== */

/* System / CR / network type */
#define REG_SYS_FUNC_EN     0x0002
#define REG_RSV_CTRL        0x001C
#define REG_CR              0x0100
#define MSR                 (REG_CR + 2)    /* Media Status (0x0102) */
#define MSR_NOLINK          0x00
#define MACTXEN             BIT(6)
#define MACRXEN             BIT(7)
/* REG_CR DMA/block enables (hal_com_reg.h) — needed so RX/TX DMA runs. */
#define HCI_TXDMA_EN        BIT(0)
#define HCI_RXDMA_EN        BIT(1)
#define TXDMA_EN            BIT(2)
#define RXDMA_EN            BIT(3)
#define PROTOCOL_EN         BIT(4)
#define SCHEDULE_EN         BIT(5)
#define ENSEC               BIT(9)
#define CALTMR_EN           BIT(10)
#define _NETTYPE(x)         (((x) & 0x3) << 16)
#define MASK_NETTYPE        0x30000
#define NT_LINK_AP          0x2

/* Transfer page size / pkt buffer */
#define REG_PBP             0x0104
#define _PSTX(x)            ((x) << 4)
#define PBP_512             0x3

/* TXDMA / TRXFF */
#define REG_TRXDMA_CTRL     0x010C
#define REG_TRXFF_BNDY      0x0114
#define REG_TDECTRL         0x0208

/* RQPN / Reserved Pages (8812) */
#define REG_RQPN            0x0200
#define REG_RQPN_NPQ        0x0214
#define _HPQ(x)             ((x) & 0xFF)
#define _LPQ(x)             (((x) & 0xFF) << 8)
#define _PUBQ(x)            (((x) & 0xFF) << 16)
#define _NPQ(x)             ((x) & 0xFF)
#define LD_RQPN             BIT(31)

/* Page counts 8812 (from rtl8812a_hal.h; default build: no WOWLAN/NDPA/DBG).
 *   BCNQ_PAGE_NUM_8812        = MAX_BEACON_LEN/512 + 6 = 0x07  (comment from ref.)
 *   WOWLAN_PAGE_NUM_8812      = 0x00
 *   FW_NDPA_PAGE_NUM          = 0x00
 *   FW_DBG_MSG_PKT_PAGE_NUM   = 0x00
 *   TX_TOTAL_PAGE_NUMBER_8812 = 0xFF - 7 = 0xF8 (248)
 *   TX_PAGE_BOUNDARY_8812     = 0xF9 (249) */
#define BCNQ_PAGE_NUM_8812              0x07
#define TX_TOTAL_PAGE_NUMBER_8812      (0xFF - BCNQ_PAGE_NUM_8812)   /* 0xF8 */
#define TX_PAGE_BOUNDARY_8812          (TX_TOTAL_PAGE_NUMBER_8812 + 1) /* 0xF9 */
#define NORMAL_PAGE_NUM_LPQ_8812        0x10
#define NORMAL_PAGE_NUM_HPQ_8812        0x10
#define NORMAL_PAGE_NUM_NPQ_8812        0x00

/* TX buffer boundary registers */
#define REG_BCNQ_BDNY       0x0424
#define REG_MGQ_BDNY        0x0425
#define REG_WMAC_LBK_BF_HD  0x045D

/* RX DMA boundary (MAX_RX_DMA_BUFFER_SIZE_8812 0x3E80 - RESV 0 - 1) */
#define RX_DMA_BOUNDARY_8812   0x3E7F

/* Queue priority / OUT-EP selection */
#define _TXDMA_HIQ_MAP(x)   (((x) & 0x3) << 14)
#define _TXDMA_MGQ_MAP(x)   (((x) & 0x3) << 12)
#define _TXDMA_BKQ_MAP(x)   (((x) & 0x3) << 10)
#define _TXDMA_BEQ_MAP(x)   (((x) & 0x3) << 8)
#define _TXDMA_VIQ_MAP(x)   (((x) & 0x3) << 6)
#define _TXDMA_VOQ_MAP(x)   (((x) & 0x3) << 4)
#define QUEUE_EXTRA         0
#define QUEUE_LOW           1
#define QUEUE_NORMAL        2
#define QUEUE_HIGH          3
#define TX_SELE_HQ          BIT(0)
#define TX_SELE_LQ          BIT(1)
#define TX_SELE_NQ          BIT(2)
#define TX_SELE_EQ          BIT(3)
#define REG_HIQ_NO_LMT_EN   0x05A7

/* DriverInfo size */
#define REG_RX_DRVINFO_SZ   0x060F
#define DRVINFO_SZ          4       /* unit 8 bytes */

/* WMAC / RCR / RX filter */
#define REG_RCR             0x0608
#define REG_RX_PKT_LIMIT    0x060C
#define REG_MAR             0x0620
#define REG_RXFLTMAP0       0x06A0  /* management */
#define REG_RXFLTMAP1       0x06A2  /* control */
#define REG_RXFLTMAP2       0x06A4  /* data */

/* RCR bits (Jaguar/8812, offset 0x608, 32 bit) */
#define RCR_APPFCS          BIT(31) /* append FCS to payload */
#define RCR_APP_MIC         BIT(30)
#define RCR_APP_ICV         BIT(29)
#define RCR_APP_PHYST_RXFF  BIT(28) /* PHY status before RX packet in RXFF */
#define RCR_LSIGEN          BIT(23)
#define RCR_MFBEN           BIT(22)
#define RCR_HTC_LOC_CTRL    BIT(14)
#define RCR_AMF             BIT(13) /* accept management */
#define RCR_ACF             BIT(12) /* accept control */
#define RCR_ADF             BIT(11) /* accept data */
#define RCR_AICV            BIT(9)  /* accept ICV-error */
#define RCR_ACRC32          BIT(8)  /* accept CRC32-error */
#define RCR_CBSSID_BCN      BIT(7)  /* BSSID-match (beacon/probe rsp) */
#define RCR_CBSSID_DATA     BIT(6)  /* BSSID-match (data) */
#define RCR_APWRMGT         BIT(5)  /* accept power-mgmt */
#define RCR_ADD3            BIT(4)
#define RCR_AB              BIT(3)  /* accept broadcast */
#define RCR_AM              BIT(2)  /* accept multicast */
#define RCR_APM             BIT(1)  /* accept physical match */
#define RCR_AAP             BIT(0)  /* accept all (unicast/physical) */
#define ACRC32              BIT(8)
#define FORCEACK            BIT(26)

/* Adaptive Control (RRSR / SIFS / Retry) */
#define REG_FWHW_TXQ_CTRL   0x0420
#define EN_AMPDU_RTY_NEW    BIT(7)
#define REG_SPEC_SIFS       0x0428
#define REG_RETRY_LIMIT     0x042A
#define REG_RRSR            0x0440
#define RATE_BITMAP_ALL         0xFFFFF
#define RATE_RRSR_CCK_ONLY_1M   0xFFFF1
#define RATE_RRSR_WITHOUT_CCK   0xFFFF0
#define _SPEC_SIFS_CCK(x)   ((x) & 0xFF)
#define _SPEC_SIFS_OFDM(x)  (((x) & 0xFF) << 8)
#define BIT_SHIFT_SRL 8
#define BIT_MASK_SRL  0x3f
#define BIT_SRL(x)    (((x) & BIT_MASK_SRL) << BIT_SHIFT_SRL)
#define BIT_SHIFT_LRL 0
#define BIT_MASK_LRL  0x3f
#define BIT_LRL(x)    (((x) & BIT_MASK_LRL) << BIT_SHIFT_LRL)
#define RL_VAL_STA          0x30

/* EDCA */
#define REG_EDCA_VO_PARAM   0x0500
#define REG_EDCA_VI_PARAM   0x0504
#define REG_EDCA_BE_PARAM   0x0508
#define REG_EDCA_BK_PARAM   0x050C
#define REG_SIFS_CTX        0x0514
#define REG_SIFS_TRX        0x0516
#define REG_MAC_SPEC_SIFS   0x063A
#define REG_USTIME_TSF      0x055C
#define REG_USTIME_EDCA     0x0638

/* Retry */
#define REG_ACKTO           0x0640

/* Beacon parameters */
#define REG_BCNTCFG         0x0510
#define REG_TBTT_PROHIBIT   0x0540
#define REG_BCN_CTRL        0x0550
#define REG_DRVERLYINT      0x0558
#define REG_BCNDMATIM       0x0559
#define DIS_TSF_UDT         BIT(4)
#define TBTT_PROHIBIT_SETUP_TIME          0x04   /* 128us */
#define TBTT_PROHIBIT_HOLD_TIME_STOP_BCN  0x64   /* 3.2ms */
#define DRIVER_EARLY_INT_TIME_8812        0x05
#define BCN_DMA_ATIME_INT_TIME_8812       0x02

/* Burst pkt len (_InitBurstPktLen) */
#define REG_RXDMA_STATUS         0x0288
#define REG_RXDMA_PRO_8812       0x0290
#define REG_AMPDU_MAX_TIME_8812  0x0456
#define REG_AMPDU_MAX_LENGTH_8812 0x0458
#define REG_HT_SINGLE_AMPDU_8812 0x04C7
#define REG_PIFS                 0x0512
#define REG_MAX_AGGR_NUM         0x04CA
#define REG_FAST_EDCA_CTRL       0x0460
#define REG_ARFR0_8812           0x0444
#define REG_ARFR1_8812           0x044C
#define REG_ARFR2_8812           0x048C
#define REG_ARFR3_8812           0x0494

/* LLT init */
#define REG_LLT_INIT            0x01E0
#define _LLT_NO_ACTIVE         0x0
#define _LLT_WRITE_ACCESS      0x1
#define _LLT_INIT_DATA(x)      ((x) & 0xFF)
#define _LLT_INIT_ADDR(x)      (((x) & 0xFF) << 8)
#define _LLT_OP(x)             (((x) & 0x3) << 30)
#define _LLT_OP_VALUE(x)       (((x) >> 30) & 0x3)
#define POLLING_LLT_THRESHOLD  20
#define LAST_ENTRY_OF_TX_PKT_BUFFER_8812  255

/* USB interface identifier for the MAC-reg table condition (ODM_ITRF_USB) */
#define MAC_ITRF_USB   0x02
#define COND_ELSE      2
#define COND_ENDIF     3

/* ===========================================================================
 * MAC register table — VERBATIM from
 *   hal/phydm/rtl8812a/halhwimg8812a_mac.c : u32 array_mp_8812a_mac_reg[]
 * =========================================================================== */
static u32 array_mp_8812a_mac_reg[] = {
		0x010, 0x0000000C,
	0x80000200,	0x00000000,	0x40000000,	0x00000000,
		0x011, 0x00000066,
	0xA0000000,	0x00000000,
		0x011, 0x0000005A,
	0xB0000000,	0x00000000,
		0x025, 0x0000000F,
		0x072, 0x00000000,
		0x420, 0x00000080,
		0x428, 0x0000000A,
		0x429, 0x00000010,
		0x430, 0x00000000,
		0x431, 0x00000000,
		0x432, 0x00000000,
		0x433, 0x00000001,
		0x434, 0x00000002,
		0x435, 0x00000003,
		0x436, 0x00000005,
		0x437, 0x00000007,
		0x438, 0x00000000,
		0x439, 0x00000000,
		0x43A, 0x00000000,
		0x43B, 0x00000001,
		0x43C, 0x00000002,
		0x43D, 0x00000003,
		0x43E, 0x00000005,
		0x43F, 0x00000007,
		0x440, 0x0000005D,
		0x441, 0x00000001,
		0x442, 0x00000000,
		0x444, 0x00000010,
		0x445, 0x00000000,
		0x446, 0x00000000,
		0x447, 0x00000000,
		0x448, 0x00000000,
		0x449, 0x000000F0,
		0x44A, 0x0000000F,
		0x44B, 0x0000003E,
		0x44C, 0x00000010,
		0x44D, 0x00000000,
		0x44E, 0x00000000,
		0x44F, 0x00000000,
		0x450, 0x00000000,
		0x451, 0x000000F0,
		0x452, 0x0000000F,
		0x453, 0x00000000,
		0x45B, 0x00000080,
		0x460, 0x00000066,
		0x461, 0x00000066,
		0x4C8, 0x000000FF,
		0x4C9, 0x00000008,
		0x4CC, 0x000000FF,
		0x4CD, 0x000000FF,
		0x4CE, 0x00000001,
		0x500, 0x00000026,
		0x501, 0x000000A2,
		0x502, 0x0000002F,
		0x503, 0x00000000,
		0x504, 0x00000028,
		0x505, 0x000000A3,
		0x506, 0x0000005E,
		0x507, 0x00000000,
		0x508, 0x0000002B,
		0x509, 0x000000A4,
		0x50A, 0x0000005E,
		0x50B, 0x00000000,
		0x50C, 0x0000004F,
		0x50D, 0x000000A4,
		0x50E, 0x00000000,
		0x50F, 0x00000000,
		0x512, 0x0000001C,
		0x514, 0x0000000A,
		0x516, 0x0000000A,
		0x525, 0x0000004F,
		0x550, 0x00000010,
		0x551, 0x00000010,
		0x559, 0x00000002,
		0x55C, 0x00000050,
		0x55D, 0x000000FF,
		0x604, 0x00000009,
		0x605, 0x00000030,
		0x607, 0x00000003,
		0x608, 0x0000000E,
		0x609, 0x0000002A,
		0x620, 0x000000FF,
		0x621, 0x000000FF,
		0x622, 0x000000FF,
		0x623, 0x000000FF,
		0x624, 0x000000FF,
		0x625, 0x000000FF,
		0x626, 0x000000FF,
		0x627, 0x000000FF,
		0x638, 0x00000050,
		0x63C, 0x0000000A,
		0x63D, 0x0000000A,
		0x63E, 0x0000000E,
		0x63F, 0x0000000E,
		0x640, 0x00000080,
		0x642, 0x00000040,
		0x643, 0x00000000,
		0x652, 0x000000C8,
		0x66E, 0x00000005,
		0x700, 0x00000021,
		0x701, 0x00000043,
		0x702, 0x00000065,
		0x703, 0x00000087,
		0x708, 0x00000021,
		0x709, 0x00000043,
		0x70A, 0x00000065,
		0x70B, 0x00000087,
		0x718, 0x00000040,

};

/* ===========================================================================
 * Apply the MAC-reg table (PHY_MACConfig8812 ->
 * odm_read_and_config_mp_8812a_mac_reg). The table contains a
 * conditional block (IF interface==USB -> 0x011=0x66, ELSE 0x011=0x5A).
 * check_positive() is hard-wired to our USB interface. Every
 * non-condition entry is an 8-bit write (odm_config_mac_8812a =>
 * odm_write_1byte).
 * =========================================================================== */

/* Ported from check_positive() in halhwimg8812a_mac.c, fixed for USB interface
 * (support_interface = ODM_ITRF_USB), all remaining dm fields = 0/DONTCARE. */
static int mac_check_positive(u32 cond1, u32 cond2, u32 cond3, u32 cond4)
{
	/* driver1: nur das Interface-Feld (Bits [11:8] = interface & 0x0F) ist
	 * fuer diese Tabelle relevant; cut/package/board sind hier DONTCARE. */
	u32 driver1 = (MAC_ITRF_USB & 0x0F) << 8;
	u32 driver2 = 0, driver4 = 0;

	(void)cond3;

	/* value-defined check (QFN [15:12] / cut [27:24]) */
	if (((cond1 & 0x0000F000) != 0) && ((cond1 & 0x0000F000) != (driver1 & 0x0000F000)))
		return 0;
	if (((cond1 & 0x0F000000) != 0) && ((cond1 & 0x0F000000) != (driver1 & 0x0F000000)))
		return 0;

	/* bit-defined check */
	cond1   &= 0x00FF0FFF;
	driver1 &= 0x00FF0FFF;

	if ((cond1 & driver1) == cond1) {
		u32 bit_mask = 0;

		if ((cond1 & 0x0F) == 0) /* board_type ist DONTCARE */
			return 1;

		if (cond1 & BIT(0)) bit_mask |= 0x000000FF;
		if (cond1 & BIT(1)) bit_mask |= 0x0000FF00;
		if (cond1 & BIT(2)) bit_mask |= 0x00FF0000;
		if (cond1 & BIT(3)) bit_mask |= 0xFF000000;

		if (((cond2 & bit_mask) == (driver2 & bit_mask)) &&
		    ((cond4 & bit_mask) == (driver4 & bit_mask)))
			return 1;
		return 0;
	}
	return 0;
}

static int apply_mac_reg_table(libusb_device_handle *h, int verbose)
{
	u32  array_len = (u32)(sizeof(array_mp_8812a_mac_reg) / sizeof(u32));
	u32 *array = array_mp_8812a_mac_reg;
	u32  i = 0;
	u32  v1, v2, pre_v1 = 0, pre_v2 = 0;
	int  is_matched = 1, is_skipped = 0;
	int  rc;
	int  writes = 0;

	while ((i + 1) < array_len) {
		v1 = array[i];
		v2 = array[i + 1];

		if (v1 & (BIT(31) | BIT(30))) {          /* Bedingungseintrag */
			if (v1 & BIT(31)) {                  /* positive Bedingung */
				u8 c_cond = (u8)((v1 & (BIT(29) | BIT(28))) >> 28);
				if (c_cond == COND_ENDIF) {
					is_matched = 1;
					is_skipped = 0;
				} else if (c_cond == COND_ELSE) {
					is_matched = is_skipped ? 0 : 1;
				} else {                         /* IF / ELSE IF */
					pre_v1 = v1;
					pre_v2 = v2;
				}
			} else if (v1 & BIT(30)) {           /* negative Bedingung */
				if (!is_skipped) {
					if (mac_check_positive(pre_v1, pre_v2, v1, v2)) {
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
		} else {                                 /* normaler 8-bit-Write */
			if (is_matched) {
				rc = rtl_write8(h, (uint16_t)v1, (uint8_t)v2);
				if (rc)
					return rc;
				writes++;
			}
		}
		i += 2;
	}

	if (verbose)
		printf("[mac] MAC-Reg-Tabelle angewandt: %d Writes (USB-Zweig)\n", writes);
	return 0;
}

/* ===========================================================================
 * LLT-Init (rtl8812a_hal_init.c: _LLTWrite_8812A / InitLLTTable8812A)
 * =========================================================================== */
static int llt_write(libusb_device_handle *h, u32 address, u32 data)
{
	int rc;
	u32 count = 0;
	u32 value = _LLT_INIT_ADDR(address) | _LLT_INIT_DATA(data) | _LLT_OP(_LLT_WRITE_ACCESS);

	rc = rtl_write32(h, REG_LLT_INIT, value);
	if (rc)
		return rc;

	/* Polling */
	do {
		value = rtl_read32(h, REG_LLT_INIT, &rc);
		if (rc)
			return rc;
		if (_LLT_NO_ACTIVE == _LLT_OP_VALUE(value))
			return 0;
		if (count > POLLING_LLT_THRESHOLD)
			return 1;   /* Timeout */
	} while (++count);

	return 1;
}

static int init_llt_table(libusb_device_handle *h, u8 txpktbuf_bndy, int verbose)
{
	int rc;
	u32 i;
	u32 last = LAST_ENTRY_OF_TX_PKT_BUFFER_8812;

	for (i = 0; i < (u32)(txpktbuf_bndy - 1); i++) {
		rc = llt_write(h, i, i + 1);
		if (rc)
			return rc;
	}
	rc = llt_write(h, (u32)(txpktbuf_bndy - 1), 0xFF);   /* Listenende */
	if (rc)
		return rc;

	for (i = txpktbuf_bndy; i < last; i++) {             /* Ringpuffer */
		rc = llt_write(h, i, i + 1);
		if (rc)
			return rc;
	}
	rc = llt_write(h, last, txpktbuf_bndy);              /* Ring schliessen */
	if (rc)
		return rc;

	if (verbose)
		printf("[mac] LLT-Tabelle initialisiert (bndy=0x%02x)\n", txpktbuf_bndy);
	return 0;
}

/* ===========================================================================
 * OUT-Endpoint-Konfiguration (usb_halinit.c: _ConfigChipOutEP_8812).
 * Ermittelt die Bulk-OUT-EP-Anzahl per libusb aus dem aktiven Config-Deskriptor.
 * =========================================================================== */
static void config_out_ep(libusb_device_handle *h, int *out_num, u8 *queue_sel,
			  int verbose)
{
	int num_out = 0;
	libusb_device *dev = libusb_get_device(h);
	struct libusb_config_descriptor *cfg = NULL;

	if (dev && libusb_get_active_config_descriptor(dev, &cfg) == 0 && cfg) {
		int ii;
		for (ii = 0; ii < cfg->bNumInterfaces; ii++) {
			const struct libusb_interface *itf = &cfg->interface[ii];
			int a;
			for (a = 0; a < itf->num_altsetting; a++) {
				const struct libusb_interface_descriptor *id = &itf->altsetting[a];
				int e;
				/* nur AltSetting 0 (das geclaimte Interface) zaehlen */
				if (id->bAlternateSetting != 0)
					continue;
				for (e = 0; e < id->bNumEndpoints; e++) {
					const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
					int is_bulk = (ep->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK;
					int is_out  = (ep->bEndpointAddress & 0x80) == 0;
					if (is_bulk && is_out)
						num_out++;
				}
			}
		}
		libusb_free_config_descriptor(cfg);
	}

	if (num_out < 1 || num_out > 4)
		num_out = 4;   /* Standard fuer 8812AU falls Ermittlung scheitert */

	switch (num_out) {
	case 4: *queue_sel = TX_SELE_HQ | TX_SELE_LQ | TX_SELE_NQ | TX_SELE_EQ; break;
	case 3: *queue_sel = TX_SELE_HQ | TX_SELE_LQ | TX_SELE_NQ; break;
	case 2: *queue_sel = TX_SELE_HQ | TX_SELE_NQ; break;
	case 1: *queue_sel = TX_SELE_HQ; break;
	default: *queue_sel = 0; break;
	}
	*out_num = num_out;

	if (verbose)
		printf("[mac] OUT-EPs=%d, OutEpQueueSel=0x%02x\n", num_out, *queue_sel);
}

/* ===========================================================================
 * MAC-Bring-up-Schritte (usb_halinit.c, USB-Varianten)
 * =========================================================================== */

/* _InitQueueReservedPage_8812AUsb (wifi_spec=0) */
static int init_queue_reserved_page(libusb_device_handle *h, u8 queue_sel)
{
	int rc;
	u32 numHQ = 0, numLQ = 0, numNQ = 0, numPubQ;
	u32 value32;
	u8  value8;

	if (queue_sel & TX_SELE_HQ) numHQ = NORMAL_PAGE_NUM_HPQ_8812;
	if (queue_sel & TX_SELE_LQ) numLQ = NORMAL_PAGE_NUM_LPQ_8812;
	if (queue_sel & TX_SELE_NQ) numNQ = NORMAL_PAGE_NUM_NPQ_8812;

	numPubQ = TX_TOTAL_PAGE_NUMBER_8812 - numHQ - numLQ - numNQ;

	value8 = (u8)_NPQ(numNQ);
	rc = rtl_write8(h, REG_RQPN_NPQ, value8);
	if (rc) return rc;

	value32 = _HPQ(numHQ) | _LPQ(numLQ) | _PUBQ(numPubQ) | LD_RQPN;
	return rtl_write32(h, REG_RQPN, value32);
}

/* _InitTxBufferBoundary_8812AUsb (wifi_spec=0). Setzt u.a. REG_TRXFF_BNDY. */
static int init_tx_buffer_boundary(libusb_device_handle *h, u8 txpktbuf_bndy)
{
	int rc;
	rc = rtl_write8(h, REG_BCNQ_BDNY, txpktbuf_bndy);        if (rc) return rc;
	rc = rtl_write8(h, REG_MGQ_BDNY, txpktbuf_bndy);         if (rc) return rc;
	rc = rtl_write8(h, REG_WMAC_LBK_BF_HD, txpktbuf_bndy);   if (rc) return rc;
	rc = rtl_write8(h, REG_TRXFF_BNDY, txpktbuf_bndy);       if (rc) return rc;
	rc = rtl_write8(h, REG_TDECTRL + 1, txpktbuf_bndy);      return rc;
}

/* _InitNormalChipRegPriority_8812AUsb */
static int init_reg_priority(libusb_device_handle *h, u16 beQ, u16 bkQ, u16 viQ,
			     u16 voQ, u16 mgtQ, u16 hiQ)
{
	int rc;
	u16 value16 = (u16)(rtl_read16(h, REG_TRXDMA_CTRL, &rc) & 0x7);
	if (rc) return rc;

	value16 |= _TXDMA_BEQ_MAP(beQ) | _TXDMA_BKQ_MAP(bkQ) |
		   _TXDMA_VIQ_MAP(viQ) | _TXDMA_VOQ_MAP(voQ) |
		   _TXDMA_MGQ_MAP(mgtQ) | _TXDMA_HIQ_MAP(hiQ);

	return rtl_write16(h, REG_TRXDMA_CTRL, value16);
}

/* _InitQueuePriority_8812AUsb (wifi_spec=0), abhaengig von OutEpNumber */
static int init_queue_priority(libusb_device_handle *h, int out_num, u8 queue_sel)
{
	u16 beQ, bkQ, viQ, voQ, mgtQ, hiQ;
	int rc;

	switch (out_num) {
	case 2: {
		u16 valueHi = QUEUE_HIGH, valueLow = QUEUE_NORMAL;
		switch (queue_sel) {
		case (TX_SELE_HQ | TX_SELE_LQ): valueHi = QUEUE_HIGH;   valueLow = QUEUE_LOW;    break;
		case (TX_SELE_NQ | TX_SELE_LQ): valueHi = QUEUE_NORMAL; valueLow = QUEUE_LOW;    break;
		case (TX_SELE_HQ | TX_SELE_NQ): valueHi = QUEUE_HIGH;   valueLow = QUEUE_NORMAL; break;
		default: break;
		}
		beQ = valueLow; bkQ = valueLow; viQ = valueHi;
		voQ = valueHi;  mgtQ = valueHi; hiQ = valueHi;
		return init_reg_priority(h, beQ, bkQ, viQ, voQ, mgtQ, hiQ);
	}
	case 3:
		beQ = QUEUE_LOW; bkQ = QUEUE_LOW; viQ = QUEUE_NORMAL;
		voQ = QUEUE_HIGH; mgtQ = QUEUE_HIGH; hiQ = QUEUE_HIGH;
		return init_reg_priority(h, beQ, bkQ, viQ, voQ, mgtQ, hiQ);
	case 4:
		beQ = QUEUE_LOW; bkQ = QUEUE_LOW; viQ = QUEUE_NORMAL;
		voQ = QUEUE_NORMAL; mgtQ = QUEUE_EXTRA; hiQ = QUEUE_HIGH;
		rc = init_reg_priority(h, beQ, bkQ, viQ, voQ, mgtQ, hiQ);
		if (rc) return rc;
		/* init_hi_queue_config_8812a_usb */
		return rtl_write8(h, REG_HIQ_NO_LMT_EN, 0xFF);
	default:
		return 0;
	}
}

/* _InitPageBoundary_8812AUsb */
static int init_page_boundary(libusb_device_handle *h)
{
	return rtl_write16(h, REG_TRXFF_BNDY + 2, RX_DMA_BOUNDARY_8812);
}

/* _InitTransferPageSize_8812AUsb */
static int init_transfer_page_size(libusb_device_handle *h)
{
	return rtl_write8(h, REG_PBP, (u8)_PSTX(PBP_512));
}

/* _InitDriverInfoSize_8812A */
static int init_driver_info_size(libusb_device_handle *h)
{
	return rtl_write8(h, REG_RX_DRVINFO_SZ, DRVINFO_SZ);
}

/* _InitNetworkType_8812A: Netzwerktyp = AP (msr), ueber REG_CR[17:16] */
static int init_network_type(libusb_device_handle *h)
{
	int rc;
	u32 value32 = rtl_read32(h, REG_CR, &rc);
	if (rc) return rc;
	value32 = (value32 & ~MASK_NETTYPE) | _NETTYPE(NT_LINK_AP);
	return rtl_write32(h, REG_CR, value32);
}

/* _InitWMACSetting_8812A: RCR (Standard-Betrieb) + MAR + RXFLTMAP1.
 * Fuer Monitor-Mode wird RCR/RXFLTMAP spaeter von rtl_mac_set_monitor
 * ueberschrieben. */
static int init_wmac_setting(libusb_device_handle *h)
{
	int rc;
	u32 rcr;
	u16 value16;

	rcr = RCR_APM | RCR_AM | RCR_AB | RCR_CBSSID_DATA | RCR_CBSSID_BCN |
	      RCR_APP_ICV | RCR_AMF | RCR_HTC_LOC_CTRL | RCR_APP_MIC |
	      RCR_APP_PHYST_RXFF;
	rcr |= FORCEACK;
	rc = rtl_write32(h, REG_RCR, rcr);
	if (rc) return rc;

	rc = rtl_write32(h, REG_MAR, 0xFFFFFFFF);       if (rc) return rc;
	rc = rtl_write32(h, REG_MAR + 4, 0xFFFFFFFF);   if (rc) return rc;

	/* RxFilterMap: ps-poll maskieren (BIT10), wie in der Referenz. */
	value16 = BIT(10);
	return rtl_write16(h, REG_RXFLTMAP1, value16);
}

/* _InitAdaptiveCtrl_8812AUsb (wireless_mode ohne 11B -> without-CCK) */
static int init_adaptive_ctrl(libusb_device_handle *h)
{
	int rc;
	u32 value32;
	u16 value16;

	value32 = rtl_read32(h, REG_RRSR, &rc);
	if (rc) return rc;
	value32 &= ~RATE_BITMAP_ALL;
	value32 |= RATE_RRSR_WITHOUT_CCK;
	value32 |= RATE_RRSR_CCK_ONLY_1M;
	rc = rtl_write32(h, REG_RRSR, value32);
	if (rc) return rc;

	value16 = (u16)(_SPEC_SIFS_CCK(0x10) | _SPEC_SIFS_OFDM(0x10));
	rc = rtl_write16(h, REG_SPEC_SIFS, value16);
	if (rc) return rc;

	value16 = (u16)(BIT_LRL(RL_VAL_STA) | BIT_SRL(RL_VAL_STA));
	return rtl_write16(h, REG_RETRY_LIMIT, value16);
}

/* _InitEDCA_8812AUsb */
static int init_edca(libusb_device_handle *h)
{
	int rc;
	rc = rtl_write16(h, REG_SPEC_SIFS, 0x100a);       if (rc) return rc;
	rc = rtl_write16(h, REG_MAC_SPEC_SIFS, 0x100a);   if (rc) return rc;
	rc = rtl_write16(h, REG_SIFS_CTX, 0x100a);        if (rc) return rc;
	rc = rtl_write16(h, REG_SIFS_TRX, 0x100a);        if (rc) return rc;
	rc = rtl_write32(h, REG_EDCA_BE_PARAM, 0x005EA42B); if (rc) return rc;
	rc = rtl_write32(h, REG_EDCA_BK_PARAM, 0x0000A44F); if (rc) return rc;
	rc = rtl_write32(h, REG_EDCA_VI_PARAM, 0x005EA324); if (rc) return rc;
	rc = rtl_write32(h, REG_EDCA_VO_PARAM, 0x002FA226); if (rc) return rc;
	rc = rtl_write8(h, REG_USTIME_TSF, 0x50);         if (rc) return rc;
	rc = rtl_write8(h, REG_USTIME_EDCA, 0x50);        return rc;
}

/* _InitRetryFunction_8812A */
static int init_retry_function(libusb_device_handle *h)
{
	int rc;
	u8 value8 = rtl_read8(h, REG_FWHW_TXQ_CTRL, &rc);
	if (rc) return rc;
	value8 |= EN_AMPDU_RTY_NEW;
	rc = rtl_write8(h, REG_FWHW_TXQ_CTRL, value8);
	if (rc) return rc;
	return rtl_write8(h, REG_ACKTO, 0x80);
}

/* _InitBeaconParameters_8812A (ohne BT-Coexist) */
static int init_beacon_parameters(libusb_device_handle *h)
{
	int rc;
	u8  val8 = DIS_TSF_UDT;
	u16 val16 = (u16)(val8 | (val8 << 8));   /* port0 und port1 */
	u8  tmp;

	rc = rtl_write16(h, REG_BCN_CTRL, val16);                 if (rc) return rc;
	rc = rtl_write8(h, REG_TBTT_PROHIBIT, TBTT_PROHIBIT_SETUP_TIME); if (rc) return rc;
	rc = rtl_write8(h, REG_TBTT_PROHIBIT + 1, TBTT_PROHIBIT_HOLD_TIME_STOP_BCN & 0xFF); if (rc) return rc;
	tmp = rtl_read8(h, REG_TBTT_PROHIBIT + 2, &rc);           if (rc) return rc;
	rc = rtl_write8(h, REG_TBTT_PROHIBIT + 2,
			(u8)((tmp & 0xF0) | (TBTT_PROHIBIT_HOLD_TIME_STOP_BCN >> 8))); if (rc) return rc;
	rc = rtl_write8(h, REG_DRVERLYINT, DRIVER_EARLY_INT_TIME_8812);   if (rc) return rc;
	rc = rtl_write8(h, REG_BCNDMATIM, BCN_DMA_ATIME_INT_TIME_8812);   if (rc) return rc;
	return rtl_write16(h, REG_BCNTCFG, 0x4413);
}

/* _InitBurstPktLen (usb_halinit.c). AMPDUBurstMode/8821 nicht relevant fuer
 * 8812AU; USB-Speed wird zur Laufzeit aus Reg 0xff/0xfe17 gelesen. */
static int init_burst_pkt_len(libusb_device_handle *h)
{
	int rc;
	u8  speedvalue, provalue, temp;

	rc = rtl_write8(h, 0xf050, 0x01);        if (rc) return rc;  /* usb3 rx interval */
	rc = rtl_write16(h, REG_RXDMA_STATUS, 0x7400); if (rc) return rc; /* burst len=4 */
	rc = rtl_write8(h, 0x289, 0xf5);         if (rc) return rc;  /* rxdma control */

	rc = rtl_write8(h, REG_AMPDU_MAX_TIME_8812, 0x70);   if (rc) return rc;
	rc = rtl_write32(h, REG_AMPDU_MAX_LENGTH_8812, 0xffffffff); if (rc) return rc;
	rc = rtl_write8(h, REG_USTIME_TSF, 0x50);   if (rc) return rc;
	rc = rtl_write8(h, REG_USTIME_EDCA, 0x50);  if (rc) return rc;

	speedvalue = rtl_read8(h, 0xff, &rc);       if (rc) return rc; /* SS 0xff bit7 */

	if (speedvalue & BIT(7)) {                  /* USB2/1.1 */
		temp = rtl_read8(h, 0xfe17, &rc);   if (rc) return rc;
		if (((temp >> 4) & 0x03) == 0) {
			provalue = rtl_read8(h, REG_RXDMA_PRO_8812, &rc); if (rc) return rc;
			rc = rtl_write8(h, REG_RXDMA_PRO_8812,
					(u8)((provalue | BIT(4) | BIT(3) | BIT(2) | BIT(1)) & (~BIT(5)))); /* 512B */
			if (rc) return rc;
		} else {
			provalue = rtl_read8(h, REG_RXDMA_PRO_8812, &rc); if (rc) return rc;
			rc = rtl_write8(h, REG_RXDMA_PRO_8812,
					(u8)((provalue | BIT(5) | BIT(3) | BIT(2) | BIT(1)) & (~BIT(4)))); /* 64B */
			if (rc) return rc;
		}
	} else {                                    /* USB3 */
		provalue = rtl_read8(h, REG_RXDMA_PRO_8812, &rc); if (rc) return rc;
		rc = rtl_write8(h, REG_RXDMA_PRO_8812,
				(u8)((provalue | BIT(3) | BIT(2) | BIT(1)) & (~(BIT(5) | BIT(4))))); /* 1k */
		if (rc) return rc;
		temp = rtl_read8(h, 0xf008, &rc);   if (rc) return rc;
		rc = rtl_write8(h, 0xf008, (u8)(temp & 0xE7)); if (rc) return rc; /* U1/U2 aus */
	}

	rc = rtl_write8(h, REG_TDECTRL, 0x10);      if (rc) return rc; /* !CONFIG_USB_TX_AGGREGATION */

	temp = rtl_read8(h, REG_SYS_FUNC_EN, &rc);  if (rc) return rc;
	rc = rtl_write8(h, REG_SYS_FUNC_EN, (u8)(temp & (~BIT(10)))); if (rc) return rc; /* reset 8051 */

	temp = rtl_read8(h, REG_HT_SINGLE_AMPDU_8812, &rc); if (rc) return rc;
	rc = rtl_write8(h, REG_HT_SINGLE_AMPDU_8812, (u8)(temp | BIT(7))); if (rc) return rc;
	rc = rtl_write8(h, REG_RX_PKT_LIMIT, 0x18);  if (rc) return rc; /* VHT 11K */
	rc = rtl_write8(h, REG_PIFS, 0x00);          if (rc) return rc;

	rc = rtl_write16(h, REG_MAX_AGGR_NUM, 0x1f1f); if (rc) return rc;
	temp = rtl_read8(h, REG_FWHW_TXQ_CTRL, &rc);   if (rc) return rc;
	rc = rtl_write8(h, REG_FWHW_TXQ_CTRL, (u8)(temp & (~BIT(7)))); if (rc) return rc;

	temp = rtl_read8(h, REG_RSV_CTRL, &rc);      if (rc) return rc; /* 0x1c */
	rc = rtl_write8(h, REG_RSV_CTRL, (u8)(temp | BIT(5) | BIT(6))); if (rc) return rc;

	/* ARFB-Tabellen 9-12 */
	rc = rtl_write32(h, REG_ARFR0_8812, 0x00000010);     if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR0_8812 + 4, 0xfffff000); if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR1_8812, 0x00000010);     if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR1_8812 + 4, 0x003ff000); if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR2_8812, 0x00000015);     if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR2_8812 + 4, 0x003ff000); if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR3_8812, 0x00000015);     if (rc) return rc;
	rc = rtl_write32(h, REG_ARFR3_8812 + 4, 0xffcff000); return rc;
}

/* ===========================================================================
 * Oeffentliche API
 * =========================================================================== */
int rtl_mac_init(libusb_device_handle *h, int verbose)
{
	int rc;
	int out_num = 4;
	u8  queue_sel = TX_SELE_HQ | TX_SELE_LQ | TX_SELE_NQ | TX_SELE_EQ;
	u8  txpktbuf_bndy = TX_PAGE_BOUNDARY_8812;
	u8  value8;

	config_out_ep(h, &out_num, &queue_sel, verbose);

	/* 0) _InitPowerOn_8812AU: MAC-DMA/WMAC/SCHEDULE/SEC-Block freischalten.
	 * OHNE diese CR-DMA-Enables liefert der Chip KEINE RX-Daten ueber USB
	 * (Symptom: 0 Bytes auf 0x81). Muss vor LLT/MAC-Config passieren. */
	rtl_write16(h, REG_CR, 0x0000);
	{
		uint16_t cr = rtl_read16(h, REG_CR, NULL);
		cr |= (HCI_TXDMA_EN | HCI_RXDMA_EN | TXDMA_EN | RXDMA_EN
		       | PROTOCOL_EN | SCHEDULE_EN | ENSEC | CALTMR_EN);
		rc = rtl_write16(h, REG_CR, cr);
		if (rc) { if (verbose) printf("[mac] CR-DMA-Enable fehlgeschlagen (%d)\n", rc); return rc; }
		if (verbose) printf("[mac] CR-DMA-Block freigeschaltet -> CR=0x%04x\n", cr);
	}

	/* 1) LLT-Tabelle (im HAL vor der MAC-Config, direkt nach Power-On). */
	rc = init_llt_table(h, txpktbuf_bndy, verbose);
	if (rc) { if (verbose) printf("[mac] LLT-Init fehlgeschlagen (%d)\n", rc); return rc; }

	/* 2) PHY_MACConfig8812: MAC-Register-Tabelle anwenden. */
	rc = apply_mac_reg_table(h, verbose);
	if (rc) { if (verbose) printf("[mac] MAC-Reg-Tabelle fehlgeschlagen (%d)\n", rc); return rc; }

	/* 3) Queues / Pages / Prioritaeten */
	rc = init_queue_reserved_page(h, queue_sel);   if (rc) return rc;
	rc = init_tx_buffer_boundary(h, txpktbuf_bndy); if (rc) return rc;
	rc = init_queue_priority(h, out_num, queue_sel); if (rc) return rc;
	rc = init_page_boundary(h);                     if (rc) return rc;
	rc = init_transfer_page_size(h);                if (rc) return rc;

	/* 4) DriverInfo / Netzwerktyp / WMAC / EDCA / Retry / Beacon */
	rc = init_driver_info_size(h);   if (rc) return rc;
	rc = init_network_type(h);       if (rc) return rc;
	rc = init_wmac_setting(h);       if (rc) return rc;
	rc = init_adaptive_ctrl(h);      if (rc) return rc;
	rc = init_edca(h);               if (rc) return rc;
	rc = init_retry_function(h);     if (rc) return rc;
	rc = init_beacon_parameters(h);  if (rc) return rc;

	/* 5) Burst-Pkt-Len (USB) */
	rc = init_burst_pkt_len(h);      if (rc) return rc;

	/* 6) MACTXEN/MACRXEN NACH REG_TRXFF_BNDY setzen (HW-Bug-Workaround). */
	value8 = rtl_read8(h, REG_CR, &rc); if (rc) return rc;
	rc = rtl_write8(h, REG_CR, (u8)(value8 | MACTXEN | MACRXEN));
	if (rc) return rc;

	if (verbose)
		printf("[mac] MAC-Init abgeschlossen (CR=0x%02x)\n",
		       rtl_read8(h, REG_CR, NULL));
	return 0;
}

int rtl_mac_set_monitor(libusb_device_handle *h, int verbose)
{
	int rc;
	u32 rcr;
	u32 value32;

	/*
	 * Promiskuitiver Monitor-RCR. Basis ist hw_var_set_monitor() aus
	 * rtl8812a_hal_init.c:
	 *     RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_APWRMGT
	 *   | RCR_ADF | RCR_ACF | RCR_AMF | RCR_APP_PHYST_RXFF | RCR_APPFCS
	 * Zusaetzlich (laut Aufgabenstellung, in der Referenz per #if 0
	 * deaktiviert, weil deren Stack CRC/ICV-Frames spaeter verwirft):
	 *     RCR_ACRC32 | RCR_AICV      -> auch fehlerhafte Frames annehmen.
	 *
	 *   AAP=BIT0  APM=BIT1  AM=BIT2  AB=BIT3  APWRMGT=BIT5
	 *   ACRC32=BIT8  AICV=BIT9  ADF=BIT11  ACF=BIT12  AMF=BIT13
	 *   APP_PHYST_RXFF=BIT28  APPFCS=BIT31
	 *   => 0x90003B2F
	 */
	rcr = RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_APWRMGT |
	      RCR_ACRC32 | RCR_AICV |
	      RCR_ADF | RCR_ACF | RCR_AMF |
	      RCR_APP_PHYST_RXFF | RCR_APPFCS;

	/* Netzwerktyp Port0 -> NoLink (Set_MSR(NOLINK)): MSR[1:0] loeschen. */
	{
		u8 msr = rtl_read8(h, MSR, &rc);
		if (rc) return rc;
		msr = (u8)((msr & 0x0C) | MSR_NOLINK);   /* Port0-Netztyp = 0 */
		rc = rtl_write8(h, MSR, msr);
		if (rc) return rc;
	}

	rc = rtl_write32(h, REG_RCR, rcr);
	if (rc) return rc;

	/* Alle mgmt/ctrl/data-Subtypen durchlassen. */
	rc = rtl_write16(h, REG_RXFLTMAP0, 0xFFFF); if (rc) return rc; /* management */
	rc = rtl_write16(h, REG_RXFLTMAP1, 0xFFFF); if (rc) return rc; /* control */
	rc = rtl_write16(h, REG_RXFLTMAP2, 0xFFFF); if (rc) return rc; /* data */

	if (verbose) {
		value32 = rtl_read32(h, REG_RCR, NULL);
		printf("[mac] Monitor-Mode: RCR=0x%08x (Soll 0x90003B2F), "
		       "RXFLTMAP0/1/2=0xFFFF\n", value32);
	}
	return 0;
}
