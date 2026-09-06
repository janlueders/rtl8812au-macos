#include <stdio.h>
#include "rtl_ccmp.h"
int main(void) {
    int rc = rtl_ccmp_selftest();
    printf("AES-CCM Selbsttest (NIST SP 800-38C, Beispiel 1): %s\n", rc==0 ? "PASS" : "FAIL");
    return rc;
}
