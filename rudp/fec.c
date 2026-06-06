#include "fec.h"
#include <string.h>

void fec_xor_parity(const uint8_t *const *data_pkts, int n_pkts,
                    int pkt_len, uint8_t *parity)
{
    memset(parity, 0, (size_t)pkt_len);
    for (int i = 0; i < n_pkts; i++) {
        const uint8_t *p = data_pkts[i];
        for (int j = 0; j < pkt_len; j++) {
            parity[j] ^= p[j];
        }
    }
}

int fec_xor_recover(uint8_t *missing, const uint8_t *const *present,
                    int n_present, int missing_idx, int pkt_len,
                    const uint8_t *parity)
{
    memcpy(missing, parity, (size_t)pkt_len);
    for (int i = 0; i < n_present; i++) {
        const uint8_t *p = present[i];
        for (int j = 0; j < pkt_len; j++) {
            missing[j] ^= p[j];
        }
    }
    (void)missing_idx;
    return 0;
}
