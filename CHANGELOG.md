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
