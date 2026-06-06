# Changelog

Development log for the RUDP project. Each phase was built, tested, and verified before moving to the next. The protocol evolved from a single checksum routine to a full sliding-window transport with adaptive RTO and a file-transfer application.

---

## Phase 1 — Header & Checksum (Mon May 4)

Started with the foundation: a custom 14-byte packed binary header and the Internet checksum.

- Defined `struct rudp_header` with `__attribute__((packed))` for stable wire format.
- Implemented `rudp_checksum` — 16-bit one's-complement sum, big-endian word interpretation, odd-byte handling.
- Implemented `rudp_header_encode` / `rudp_header_decode` for network-byte-order conversion.
- Implemented `rudp_build_packet` (header + payload + checksum) and `rudp_parse_packet` (validate + zero-copy payload pointer).
- **17 tests pass**, including the RFC 1071 `"123456789"` checksum vector and bit-flip detection.

**Bug found & fixed**: original `RUDP_HEADER_SIZE` was 12 (forgot `type` + `window` bytes). Measured the actual packed size, fixed to 14.

## Phase 2 — Socket Wrappers (Tue May 5)

Built the on-the-wire plumbing: a single function to send a header+payload atomically, a single function to receive and validate one.

- `rudp_sendto` — build packet, single `sendto()` call.
- `rudp_recvfrom` — `recvfrom()` into stack buffer, parse + checksum-validate, bounded copy to caller.
- **13 loopback echo tests pass**: payload integrity, checksum rejection of tampered packets, empty-payload edge case, max-payload edge case.

## Phase 3 — Stop-and-Wait ARQ (Wed May 6)

First reliable-delivery protocol. One packet in flight at a time, retransmit on timeout, give up after `MAX_RETRANSMITS = 10`.

- Sender state machine: IDLE → WAIT_ACK → IDLE, with timeout-driven retransmit loop.
- Receiver: cumulative ACK, duplicate detection, out-of-order handling.
- Used `poll()` with timeout to avoid busy-waiting.
- **16 tests pass** across 0/10/30/50/100% packet drop scenarios.

**Limitation identified**: only one packet in flight. At RTT = 100ms that's 10 pkts/sec — too slow for bulk transfer. Motivates Phase 4.

## Phase 4 — Sliding Window + SACK (Thu May 7)

Pipeline up to 32 unACKed packets. Use Selective ACK to acknowledge multiple gaps in a single packet.

- Sender: per-packet slots (cached packet + seq + send timestamp + retransmit count), window-bound send loop.
- Receiver: 32-bit bitmap starting at `next_expected`, out-of-order buffer, in-order delivery walk.
- SACK packet type: `ack_num` = cumulative ACK, payload = 32-bit bitmap of received-after-ack gaps.
- **9 tests pass** at 0/20/40% drop, including out-of-order delivery and window-full scenarios.

**Bug found & fixed**: receiver's "return-early-when-all-bytes-received" check ran before `send_sack`, so the final SACK was never emitted under heavy loss, hanging the sender. Moved `send_sack` to before the early-return.

**Bug found & fixed**: inner drain loop used `poll(sockfd, 0)` to peek for more packets, but with kernel scheduling delays, sometimes a SACK we just sent would race with an incoming packet. Replaced with `MSG_DONTWAIT` on `recvfrom`.

## Phase 5 — Adaptive RTT/RTO (Fri May 8)

Replaced the static 500ms RTO with a dynamic value that adapts to measured round-trip time.

- Implemented RFC 6298 Jacobson/Karels algorithm: `SRTT` and `RTTVAR` smoothed estimators, `RTO = SRTT + 4·RTTVAR`.
- Used **fixed-point arithmetic** (3-bit shift, alpha = 1/8, beta = 1/4) for reproducibility and speed.
- Implemented **Karn's algorithm**: skip RTT samples for any packet that has been retransmitted (you can't tell whether the ACK is for the original or the retransmit).
- RTO clamped to `[100ms, 10000ms]`.
- **8 tests pass**: first-sample initialization, convergence under stable RTT, response to RTT spike/drop, Karn's algorithm, RTO clamping, loopback stress.

## Phase 6 — File Transfer Application (Sat May 9)

Wired the protocol to a real application: sending a file from one process to another.

- `rudp_file_metadata` struct — magic (`'RUPF'`), file size, filename.
- Sender protocol: `RUDP_SYN` with metadata payload, sliding-window data, `RUDP_FIN`.
- `rudp_sendfile` and `rudp_recvfile` CLI tools.
- `rudp_recvfile` supports `-drop N` for server-side packet-drop testing.
- **12 tests pass** in `test_file.c`: 50KB payload at 0/10/30/50% drop, sender return value, receiver byte count, byte-for-byte integrity check.
- **End-to-end CLI verified**: 100KB random file, 20% drop, MD5 matches on both sides.

## Final — Documentation (Sun May 10)

Wrote the README, the full technical deep-dive (`RUDP_PROJECT.md`), and this changelog. The deep-dive covers header byte layout, all algorithms in pseudocode, seven concrete bugs with postmortems, the tuning-constants table, and an interview Q&A section.

**Final tally: 75/75 tests pass. ~1,000 lines of C, zero external libraries.**

## Phase 7 — Forward Erasure Correction (XOR FEC)

Added an optional Forward Erasure Correction layer to the file-transfer application. Each block of K=8 data packets is augmented with a single XOR parity packet; the receiver can recover one lost packet per block without waiting for ARQ retransmit.

- New packet type `RUDP_FEC = 0x05`. `RUDP_FIN` stays at `0x04`.
- `rudp/fec.h` and `rudp/fec.c` — XOR encoder/decoder, ~50 lines, no malloc.
- `rudp_send_fec_sliding` / `rudp_recv_fec_sliding` — sender/receiver variants for FEC mode.
- Sender pads files smaller than one block up to K packets and sends parity. Final partial block has no parity (handled on RUDP_FIN).
- `-fec [K=N]` flag added to `rudp_sendfile` / `rudp_recvfile`. Default K=8.
- **8 unit tests pass** in `test_fec.c`: encoder identity, single-bit recovery, multi-bit recovery from K=4, padding round-trip, all-zero payload, random data integrity, parity regeneration, large buffer (8 KB).
- **All 89 unit tests pass** (Phase 1-6 + Phase 7).

**Bug found & fixed — sender deadlock**: initial FEC receiver design used a block-based bitmap, so the SACK payload was `0` for the first 32 packets in a block. Sender's `if (ca >= send_base)` branch never fired, the sender's bitmap-processing saw no set bits, and the RTO loop could only retransmit the oldest missing slot — never advancing the window. Rewrote `rudp_receiver` to keep a sliding-window bitmap (offsets `[next_expected, next_expected+32)`) for SACK. Block delivery now shifts the window by K and resets the bitmap. All retransmits/SACK logic now work as in the non-FEC path.

**Bug found & fixed — block_start off-by-one**: after the first block is delivered, `fec_block_start += K` made it 8 (should be 9). The next block's parity was rejected as out-of-range. Fixed by setting `fec_block_start = r->next_expected` after the window shift.

**Benchmark finding**: integrated RUDP-FEC into the existing C-vs-C benchmark. Across 3 file sizes × 4 drop rates × 3 protocols × 3 trials (108 trials, 27.2 min), RUDP-FEC is *slower* than RUDP at 1-10% loss. The reason: the sender still uses per-packet SACK-based flow control, and block delivery adds state-management overhead. Naive XOR FEC layered on sliding-window ARQ is a pessimization for the 1-10% loss range. Documented in `benchmarks/RESULTS.md`.

## Final — Phase 7 (Mon Jun 1)

Updated `benchmarks/RESULTS.md` with the 3-way comparison, regenerated graphs via `analyze.py`. Updated `README.md` and `RUDP_PROJECT.md` to mention Phase 7. Pushed to GitHub.

**Tally: 89/89 unit tests pass. ~1,400 lines of C, zero external libraries.**

---

## Tuning constants

| Constant | Value | Justification |
|---|---|---|
| `WINDOW_SIZE` | 32 | Fits bitmap in one `uint32_t`; large enough to stress the window-full path. |
| `INITIAL_RTO_MS` | 500 | RFC 6298 recommendation for first sample. |
| `MIN_RTO_MS` | 100 | Prevents spin on loopback where actual RTT < 1ms. |
| `MAX_RTO_MS` | 10000 | Sanity bound. |
| `MAX_RETRANSMITS` | 10 | Sender gives up after this many failed retries. |
| `MAX_PAYLOAD_SIZE` | 1024 | Comfortably under typical MTU (1500). |
| `RTO_SHIFT` | 3 | Fixed-point scale factor for `SRTT`/`RTTVAR` (×8). |
| alpha (1/8) | 1/8 | RFC 6298 default. |
| beta (1/4) | 1/4 | RFC 6298 default. |
