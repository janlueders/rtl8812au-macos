/*
 * rtl_init — power-on sequence (card enable) for RTL8812AU, USB.
 *
 * Ported from:
 *   include/Hal8812PwrSeq.h   (RTL8812_TRANS_CARDEMU_TO_ACT)
 *   hal/HalPwrSeqCmd.c        (interpreter semantics)
 *
 * WRITE  : v = read8(off); v = (v & ~mask) | (value & mask); write8(off, v)
 * POLLING: repeat read8(off) until (v & mask) == (value & mask)
 * DELAY  : microseconds from the offset field
 *
 * Only the USB-relevant steps (PWR_INTF_ALL includes USB).
 */
#include "rtl_usb.h"
#include <stdio.h>
#include <unistd.h>

#define BIT(n) (1u << (n))

enum { CMD_WRITE = 1, CMD_POLLING = 2, CMD_DELAY = 3, CMD_END = 4 };

typedef struct {
    uint16_t offset;
    uint8_t  cmd;
    uint8_t  mask;
    uint8_t  value;
    const char *note;
} pwr_step;

/* RTL8812_TRANS_CARDEMU_TO_ACT (USB), BITn expanded to numbers. */
static const pwr_step card_enable[] = {
    {0x0005, CMD_WRITE,   BIT(2), 0,      "disable SW LPS 0x04[10]=0"},
    {0x0006, CMD_POLLING, BIT(1), BIT(1), "wait power ready 0x04[17]=1"},
    {0x0005, CMD_WRITE,   BIT(3), 0,      "disable WL suspend"},
    {0x0005, CMD_WRITE,   BIT(0), BIT(0), "0x04[8]=1 (APFM_ONMAC)"},
    {0x0005, CMD_POLLING, BIT(0), 0,      "wait 0x04[8]=0 (MAC on)"},
    {0x0024, CMD_WRITE,   BIT(1), 0,      "0x24[1]=0 xosc buffer"},
    {0x0028, CMD_WRITE,   BIT(3), 0,      "0x28[3]=0 xosc buffer"},
    {0x0000, CMD_END,     0,      0,      NULL},
};

#define POLL_TRIES   1000
#define POLL_DELAY_US 10

int rtl_power_on(libusb_device_handle *h, int verbose) {
    for (int i = 0; card_enable[i].cmd != CMD_END; i++) {
        const pwr_step *s = &card_enable[i];
        int rc = 0;

        if (s->cmd == CMD_WRITE) {
            uint8_t v = rtl_read8(h, s->offset, &rc);
            if (rc != 0) { if (verbose) printf("  [%d] READ 0x%04x FEHLER\n", i, s->offset); return rc; }
            v = (uint8_t)((v & ~s->mask) | (s->value & s->mask));
            rc = rtl_write8(h, s->offset, v);
            if (rc != 0) { if (verbose) printf("  [%d] WRITE 0x%04x FEHLER\n", i, s->offset); return rc; }
            if (verbose) printf("  [%d] WRITE   0x%04x mask=0x%02x val=0x%02x  (%s)\n",
                                i, s->offset, s->mask, s->value, s->note);

        } else if (s->cmd == CMD_POLLING) {
            int ok = 0;
            uint8_t v = 0;
            for (int t = 0; t < POLL_TRIES; t++) {
                v = rtl_read8(h, s->offset, &rc);
                if (rc != 0) { if (verbose) printf("  [%d] POLL READ FEHLER\n", i); return rc; }
                if ((v & s->mask) == (s->value & s->mask)) { ok = 1; break; }
                usleep(POLL_DELAY_US);
            }
            if (!ok) {
                if (verbose) printf("  [%d] POLLING 0x%04x mask=0x%02x erwartet=0x%02x "
                                    "letzter=0x%02x  TIMEOUT  (%s)\n",
                                    i, s->offset, s->mask, s->value, v & s->mask, s->note);
                return i + 1; /* >0: number of the failed step */
            }
            if (verbose) printf("  [%d] POLLING 0x%04x mask=0x%02x == 0x%02x  OK  (%s)\n",
                                i, s->offset, s->mask, s->value & s->mask, s->note);

        } else if (s->cmd == CMD_DELAY) {
            usleep(s->offset);
        }
    }
    return 0;
}
