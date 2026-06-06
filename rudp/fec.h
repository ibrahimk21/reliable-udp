#ifndef RUDP_FEC_H
#define RUDP_FEC_H

#include <stdint.h>

#define FEC_K_DEFAULT 8
#define FEC_P_DEFAULT 1

void fec_xor_parity(const uint8_t *const *data_pkts, int n_pkts,
                    int pkt_len, uint8_t *parity);

int fec_xor_recover(uint8_t *missing, const uint8_t *const *present,
                    int n_present, int missing_idx, int pkt_len,
                    const uint8_t *parity);

#endif /* RUDP_FEC_H */
