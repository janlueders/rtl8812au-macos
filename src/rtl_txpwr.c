#include "rtl_txpwr.h"
#include "rtl_bb.h"
#include "rtl_rf.h"
#include <stdint.h>
#include <stdio.h>

/* Logical efuse layout for the 2.4G TX-power PG (power-group) block, RF path
 * A (include/hal_pg.h, hal/hal_com_phycfg.c hal_load_pg_txpwr_info_path_2g):
 *   offset 0x10, 18 bytes:
 *     [0..5]  IndexCCK_Base  per channel-group (0..5)
 *     [6..10] IndexBW40_Base per channel-group (0..4 -- group 5/ch14 has none)
 *     [11]    packed nibbles: hi=BW20_Diff[1TX] (unused here), lo=OFDM_Diff[1TX]
 * Channel groups (2.4G): {1,2}=0 {3,4,5}=1 {6,7,8}=2 {9,10,11}=3 {12,13}=4 {14}=5 */
#define EFUSE_PG_2G_OFFSET      0x10
#define TXGI_MAX                63   /* hal_spec->txgi_max for RTL8812A ("Jaguar") */

#define R_TXAGC_A_CCK11_CCK1     0x0c20  /* byte0=1M byte1=2M byte2=5.5M byte3=11M */
#define R_TXAGC_A_OFDM18_OFDM6   0x0c24  /* byte0=6M byte1=9M byte2=12M byte3=18M */
#define R_TXAGC_A_OFDM54_OFDM24  0x0c28  /* byte0=24M byte1=36M byte2=48M byte3=54M */

static int channel_group_24g(int ch) {
    if (ch <= 2)  return 0;
    if (ch <= 5)  return 1;
    if (ch <= 8)  return 2;
    if (ch <= 11) return 3;
    if (ch <= 13) return 4;
    return 5; /* channel 14 */
}

static int8_t nibble_to_s8(uint8_t nibble4) {
    nibble4 &= 0x0F;
    return (nibble4 & 0x8) ? (int8_t)(nibble4 | 0xF0) : (int8_t)nibble4;
}

/* ---------------------------------------------------------------------
 * Thermal tracking (odm_txpowertracking_callback_thermal_meter,
 * hal/phydm/halrf/halphyrf_ce.c + the RFE-3 delta-swing tables from
 * hal/phydm/rtl8812a/halhwimg8812a_rf.c odm_read_and_config_mp_8812a_
 * txpowertrack_rfe3 -- these exact tables, not invented ones, confirmed
 * applicable because this unit's own efuse reads rfe_type=3, verified via
 * efuseinfo). As the external PA's temperature drifts from its efuse-
 * calibrated baseline (offset 0xBA, "eeprom_thermal" -- 0xFF there means
 * unprogrammed, and the real driver skips tracking entirely in that case,
 * so we do too), its gain drifts too; this periodically re-reads the RF
 * thermal meter (RF path A, register 0x42, bits[15:10]) and nudges the
 * OFDM TX-AGC index by the table-specified amount to compensate. Only the
 * OFDM registers are adjusted (6M/54M -- what this driver actually
 * transmits); CCK is left at its efuse base since we never send CCK data.
 * Table only for rfe_type 3 (this unit); if a future unit reads a
 * different rfe_type, tracking is skipped rather than applying an
 * unverified table -- matches the "don't guess" rule used for the base
 * calibration in rtl_txpwr_apply(). */
#define SWING_TABLE_SIZE 30
static const uint8_t swing_2ga_p_rfe3[SWING_TABLE_SIZE] = { /* temp ABOVE baseline */
    0,0,1,1,1,2,2,3,3,4,4,4,5,5,5,6,6,7,7,8,8,9,9,10,10,11,11,11,11,11
};
static const uint8_t swing_2ga_n_rfe3[SWING_TABLE_SIZE] = { /* temp BELOW baseline */
    0,1,1,2,2,3,4,5,6,6,6,7,7,8,8,9,10,10,11,11,12,12,13,13,13,13,14,14,15,15
};

static uint8_t g_cck_base = 0xFF, g_ofdm_base = 0xFF;   /* efuse-calibrated bases, set by rtl_txpwr_apply */
static uint8_t g_eeprom_thermal = 0xFF;                 /* efuse 0xBA; 0xFF = unprogrammed -> tracking skipped */
static int g_rfe_type_cached = -1;
static int g_thermal_base_read = 0;
static int g_thermal_last_applied = -1;

int rtl_txpwr_apply(libusb_device_handle *h, int channel, int verbose) {
    uint8_t map[EFUSE_MAP_LEN];
    int rc = rtl_efuse_read_map(h, map, EFUSE_MAP_LEN);
    if (rc != 0) return rc;

    const uint8_t *pg = &map[EFUSE_PG_2G_OFFSET];
    int group = channel_group_24g(channel);

    (void)verbose; /* status line always printed: this is safety-relevant, not just diagnostic noise */
    uint8_t cck_base = pg[group];
    if (cck_base > TXGI_MAX) {
        printf("[txpwr] efuse PG data unprogrammed/invalid (CCK base 0x%02x, group %d) "
               "-- leaving chip at its current power, not guessing.\n", cck_base, group);
        return 0;
    }

    uint8_t bw40_base = (group <= 4) ? pg[6 + group] : cck_base; /* group 5 (ch14) has no BW40_Base entry */
    if (bw40_base > TXGI_MAX) bw40_base = cck_base;

    int8_t ofdm_diff = nibble_to_s8(pg[11]);
    int ofdm_idx = bw40_base + ofdm_diff;
    if (ofdm_idx < 0) ofdm_idx = 0;
    if (ofdm_idx > TXGI_MAX) ofdm_idx = TXGI_MAX;

    bb_set(h, R_TXAGC_A_CCK11_CCK1, 0xff, cck_base);
    bb_set(h, R_TXAGC_A_OFDM18_OFDM6, 0xff, (uint32_t)ofdm_idx);
    bb_set(h, R_TXAGC_A_OFDM54_OFDM24, 0xff000000, (uint32_t)ofdm_idx);
    g_cck_base = cck_base; g_ofdm_base = (uint8_t)ofdm_idx; /* remembered for thermal tracking */
    g_thermal_base_read = 0; g_thermal_last_applied = -1;   /* re-read thermal baseline on next track call */

    printf("[txpwr] efuse-calibrated: channel %d group %d  CCK=%u  OFDM_base=%u OFDM_diff=%d -> OFDM=%d\n",
           channel, group, cck_base, bw40_base, ofdm_diff, ofdm_idx);

    /* Read back immediately: confirm the write actually reached the chip and
     * wasn't silently dropped (bb_set()'s return value is not checked above,
     * and a USB error there would otherwise be invisible). */
    rtl_txpwr_readback(h, "after apply");
    return 0;
}

void rtl_txpwr_readback(libusb_device_handle *h, const char *label) {
    uint32_t cck  = bb_get(h, R_TXAGC_A_CCK11_CCK1, 0xff);
    uint32_t ofdm6  = bb_get(h, R_TXAGC_A_OFDM18_OFDM6, 0xff);
    uint32_t ofdm54 = bb_get(h, R_TXAGC_A_OFDM54_OFDM24, 0xff000000);
    printf("[txpwr] readback (%s): CCK1M=%u  OFDM6M=%u  OFDM54M=%u\n", label, cck, ofdm6, ofdm54);
}

void rtl_txpwr_thermal_track(libusb_device_handle *h) {
    if (g_ofdm_base > TXGI_MAX) return;   /* rtl_txpwr_apply never got a valid base -- nothing to track */

    if (!g_thermal_base_read) {
        uint8_t map[EFUSE_MAP_LEN];
        if (rtl_efuse_read_map(h, map, EFUSE_MAP_LEN) != 0) return;
        g_eeprom_thermal = map[0xBA];
        g_rfe_type_cached = (map[0xCA] == 0xFF) ? -1 : (map[0xCA] & 0x3F);
        g_thermal_base_read = 1;
        if (g_eeprom_thermal > TXGI_MAX)
            printf("[txpwr] thermal-track: efuse 0xBA unprogrammed -- skipping (matches reference behavior).\n");
        else if (g_rfe_type_cached != 3)
            printf("[txpwr] thermal-track: rfe_type=%d has no verified swing table here -- skipping.\n", g_rfe_type_cached);
    }
    if (g_eeprom_thermal > TXGI_MAX || g_rfe_type_cached != 3) return;

    int rc = 0;
    uint32_t raw = rtl_rf_read(h, 0, 0x42, &rc);
    if (rc) return;
    uint8_t thermal = (uint8_t)((raw & 0xfc00u) >> 10);
    if (thermal > 63) return; /* sanity: field is 6 bits */

    int above = thermal > g_eeprom_thermal;
    int delta = above ? thermal - g_eeprom_thermal : g_eeprom_thermal - thermal;
    if (delta > SWING_TABLE_SIZE - 1) delta = SWING_TABLE_SIZE - 1;
    int offset = delta ? (above ? swing_2ga_p_rfe3[delta] : -swing_2ga_n_rfe3[delta]) : 0;

    int new_ofdm = g_ofdm_base + offset;
    if (new_ofdm < 0) new_ofdm = 0;
    if (new_ofdm > TXGI_MAX) new_ofdm = TXGI_MAX;
    if (new_ofdm == g_thermal_last_applied) return;   /* no change -- skip redundant register writes */

    bb_set(h, R_TXAGC_A_OFDM18_OFDM6, 0xff, (uint32_t)new_ofdm);
    bb_set(h, R_TXAGC_A_OFDM54_OFDM24, 0xff000000, (uint32_t)new_ofdm);
    printf("[txpwr] thermal-track: now=0x%02x base=0x%02x delta=%s%d -> OFDM %u->%d\n",
           thermal, g_eeprom_thermal, above ? "+" : "-", delta, g_ofdm_base, new_ofdm);
    g_thermal_last_applied = new_ofdm;
}
