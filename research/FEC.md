# Forward Error Correction (FEC) for RUDP — Research Notes

> Research notes for the planned Phase 7 of the Reliable UDP project.
> Goal: understand erasure-coding FEC deeply enough to design and implement
> a simple parity-based FEC layer on top of the existing ARQ machinery.

---

## 1. The problem FEC solves

The current RUDP (Phases 1–6) is purely ARQ-based: when a packet is lost, the
sender retransmits it on RTO timeout (and, in a future fast-retransmit, on
duplicate SACKs).

That has a fundamental cost: **every loss costs at least one RTT**.

- Packet loss probability `p`, window `W` packets in flight, RTT `R`.
- Expected retransmits per window is roughly `p * W`, each costing `R`.
- Effective throughput drops roughly as `1 / (1 + p*W*R/S)` where `S` is the
  data-per-window.

For a clean network (`p` very small) ARQ is fine. For noisy networks
(`p = 1–10%`, common on wireless, mobile, congested links) ARQ thrashes:

- The sender keeps retransmitting the same lost packet
- Each retransmit itself can be lost
- The pipeline of "new data" stalls
- The `MIN_RTO_MS` floor of 100ms means every retransmit adds at least 100ms
  of wait to the wall clock, even if the actual RTT is sub-millisecond on
  loopback

FEC attacks the problem from a different angle: **send the answer in advance**.
The sender transmits redundant parity packets along with the data. The receiver
can recover from some losses **without any retransmission**, eliminating the
RTT penalty.

This is the technique used by every production RUDP-family protocol that
matters: QUIC's FEC extension, UDT, SRT, RaptorQ-based multicast (3GPP/5G),
and disk-storage systems (RAID-6, Backblaze Vaults).

---

## 2. Two flavors of FEC we care about

There are many erasure codes (Reed-Solomon, Fountain/Raptor, Tornado, LDPC,
Random Linear Network Coding, ...). For this project, only two are practical
to discuss:

### 2a. Simple XOR parity (the trivial case)

Send `K` data packets + 1 parity packet. The parity is the bitwise XOR of
the `K` data packets. The receiver can recover from **exactly 1 loss** in
the group of `K+1` packets, with no retransmit.

- Cost: `1 / (K+1)` ≈ 11% bandwidth overhead for K=8
- Compute: `K` XOR operations per parity, trivially fast
- Code: ~20 lines of C
- Limit: only handles single-loss-per-block

### 2b. Reed-Solomon over GF(2^8) (the general case)

Send `K` data packets + `P` parity packets. The parities are computed as a
matrix product over the Galois Field GF(256), a finite field with 256 elements
(the bytes themselves). The receiver can recover from **any combination of
up to P losses** in the group of `K+P` packets.

- Cost: `P / (K+P)` bandwidth overhead
- Compute: matrix multiplication over GF(256); ~O(K*P) ops
- Code: ~150–200 lines of C (with GF(256) lookup tables)
- Power: handles any loss pattern up to P per block

For this project, the plan is to start with **2a (XOR parity)** because:

- The code is small enough to write, test, and audit in a single sitting
- The semantic difference between "1-loss-recoverable" and "any-losses-up-to-P"
  is the same conceptual model, just with a different code
- Once XOR parity works, upgrading to Reed-Solomon is a localized change in
  the FEC computation; the protocol-level integration is identical

---

## 3. How XOR parity FEC works — worked example

Suppose we want to send 4 data packets, each 8 bytes:

```
D1 = 0x41 0x42 0x43 0x44 0x45 0x46 0x47 0x48   ("ABCDEFGH")
D2 = 0x49 0x4A 0x4B 0x4C 0x4D 0x4E 0x4F 0x50   ("IJKLMNOP")
D3 = 0x51 0x52 0x53 0x54 0x55 0x56 0x57 0x58   ("QRSTUVWX")
D4 = 0x59 0x5A 0x5B 0x5C 0x5D 0x5E 0x5F 0x60   ("YZ[\]^_`")
```

The sender computes the parity packet:

```
P = D1 ⊕ D2 ⊕ D3 ⊕ D4
```

For the first byte of each packet: `0x41 ⊕ 0x49 ⊕ 0x51 ⊕ 0x59 = 0x00`. For the
second: `0x42 ⊕ 0x4A ⊕ 0x52 ⊕ 0x5A = 0x00`. (Coincidentally zero for this
particular data, but the math is the same regardless of value.)

The sender transmits 5 packets: `D1, D2, D3, D4, P`. The receiver gets some
subset of them. Three cases:

**Case A: All 5 received.** Receiver discards P and delivers D1..D4. Done.

**Case B: One packet lost, say D2.** Receiver has `D1, D3, D4, P`. It
reconstructs:

```
D2 = D1 ⊕ D3 ⊕ D4 ⊕ P
```

This is the key equation. Since `P = D1 ⊕ D2 ⊕ D3 ⊕ D4`, XORing both sides
by `D1 ⊕ D3 ⊕ D4` gives `D2` on the right. The math is self-consistent.

**Case C: Two packets lost, say D2 and D3.** Receiver has `D1, D4, P`. It
can compute `D2 ⊕ D3 = D1 ⊕ D4 ⊕ P`, but that is a single 8-byte combined
packet, not the two separate packets. With **one** parity, you **cannot**
recover from 2 losses; you would have to NACK the second.

This is the fundamental tradeoff: with `P` parities you can recover from
**up to P losses per block of K+P**. With XOR (P=1), that is exactly
1 loss per K+1 packets.

---

## 4. How Reed-Solomon extends this — matrix view

XOR parity is a special case of Reed-Solomon with P=1. To go beyond P=1
you need arithmetic in a Galois Field (a finite number system where addition
and multiplication always stay in the set).

The elegant way to see Reed-Solomon is as **matrix multiplication**.

### 4a. Setup

Take your K data packets, each of fixed length L. Lay them out as K **rows**
of a matrix, where each row is L bytes wide:

```
            L bytes wide
           ┌───────────────┐
    D1 →   │ a b c d e f g h │
    D2 →   │ i j k l m n o p │
    D3 →   │ q r s t u v w x │
    D4 →   │ y z 0 1 2 3 4 5 │
           └───────────────┘
```

### 4b. Encoding

Multiply this matrix on the left by a `(K+P) × K` **coding matrix** G, where
the first K rows are the identity (so the data passes through unchanged) and
the last P rows are carefully chosen:

```
G =  ┌ 1 0 0 0 ┐
     │ 0 1 0 0 │
     │ 0 0 1 0 │
     │ 0 0 0 1 │
     ├─────────┤
     │ 1 1 1 1 │    ← P1: simple XOR
     │ 1 2 3 4 │    ← P2: Reed-Solomon (in GF(256), the "2", "3", "4"
     └─────────┘       are special field elements, not integers)
```

`Encoded = G · Data` is a `(K+P) × L` matrix — the same K data rows plus
2 parity rows. The result of `P2` is **not** a simple XOR; it is a different
linear combination that uses GF(256) multiplication and addition, which is
why it has different error-correcting properties than P1.

### 4c. Decoding (when some rows are lost)

Say rows 2 and 4 are lost. The receiver has 4 of 6 rows. To decode:

1. Take the coding matrix G
2. Delete the rows corresponding to lost packets
3. Invert the resulting square matrix; call it `G_inv`
4. Multiply: `Recovered = G_inv · Received`

The math works because G was constructed to be invertible in **any** K×K
submatrix. That property is the definition of a **Maximum Distance Separable
(MDS)** code, which Reed-Solomon is.

For a 4+2 RS code, you can lose any 2 of 6 packets. For a 4+1 XOR code, you
can lose any 1 of 5. The general formula: an `(K+P)` code recovers from any
P losses.

### 4d. Why GF(256)?

Because each row of our matrix is bytes. We want arithmetic that operates on
bytes, gives bytes back, and has the algebraic properties we need (every
nonzero element has a multiplicative inverse, etc.). GF(256) is the standard
choice.

- The "+" operation in GF(256) is XOR.
- The "·" operation is polynomial multiplication modulo an irreducible
  polynomial, typically `x^8 + x^4 + x^3 + x^2 + 1` = `0x11D`.
- For implementation, you precompute two 256-entry tables: `log_table[i]`
  and `exp_table[i]`. Multiplication becomes:
  `a · b = exp[(log[a] + log[b]) mod 255]`.

This is also why the visual "`1+1=0`" rule in GF(2) extends: in GF(256),
`a ⊕ a = 0` for any `a` (because addition is XOR), and `a · b` is a
lookup-table operation.

---

## 5. The Galois Field, in 60 seconds

You can read every other sentence of an RS tutorial and still feel lost,
because the term "Galois Field" carries baggage. The minimum you need to
know:

- A field is a number system with +, −, ×, ÷ that behaves like the reals
  or rationals.
- GF(2) is `{0, 1}` with XOR as + and AND as ×. The "finiteness" is that
  there are only 2 elements.
- GF(2^m) is a field with `2^m` elements. GF(2^8) = GF(256) has 256 elements.
- In GF(256), each element is 8 bits. The "+" operation is XOR. The "·"
  operation is polynomial multiplication mod an irreducible polynomial
  (typically `0x11D`).
- For implementation, you precompute two 256-entry tables: `log_table[i]`
  and `exp_table[i]`. Multiplication becomes:
  `a · b = exp[(log[a] + log[b]) mod 255]`. Addition is just `a ⊕ b`.

That is it. You do not need to understand the polynomial theory to
implement it. The tables are well-known and ~1KB total.

---

## 6. FEC inside a reliable transport protocol — design choices

The RUDP context has its own constraints. Here are the design questions and
our answers:

### 6a. Block size K

How many data packets per FEC block?

- Small K (4–8): low latency to first correctable block, more overhead
  (`1/(K+1)` is bigger for small K).
- Large K (32–64): less overhead (`1/33` for K=32), but you wait longer
  to recover from a single loss.

**Choice for our project: K=8.** Compromise between overhead (~11%) and
recovery granularity. Tunable later.

### 6b. Number of parities P

How many losses can we tolerate per block?

- P=1 (XOR): recovers from 1 loss per 9 packets. Simple, fast, no GF math.
- P=2 (RS-2): recovers from 2 losses per 10 packets. Needs GF(256) tables.
- P=4 (RS-4): recovers from 4 losses per 12 packets. Same tables, more compute.

**Choice for our project: P=1 (XOR) for v1, P=2 as a possible follow-up.**
Per the discussion in the project log, the goal is to add FEC as a
differentiator, not to implement the most powerful code. XOR demonstrates
the principle. RS is the upgrade.

### 6c. Block boundaries — how does the receiver know?

The sender numbers each data packet with a sequence number (we already have
this in the 14-byte RUDP header). The parity packet carries:

- The sequence number of the **first** data packet in its block
- A flag indicating "I am a parity packet"
- The block size K and parity index P (so the receiver knows how to
  assemble the block)

We need a new packet type, `RUDP_FEC`, defined in `rudp.h`:

```c
typedef enum {
    RUDP_DATA   = 0x01,
    RUDP_ACK    = 0x02,
    RUDP_SACK   = 0x03,
    RUDP_FEC    = 0x04,   // NEW: parity packet
    RUDP_SYN    = 0x05,
    RUDP_FIN    = 0x06
} rudp_packet_type_t;
```

The header of an `RUDP_FEC` packet reuses the existing 14-byte header but
interprets the `seq` field as the sequence number of the **first** data packet
in the block, and the first byte of the payload encodes `(K, P, parity_index)`.

### 6d. Interaction with ARQ

FEC and ARQ are **complementary**, not exclusive. The hybrid is called
**Hybrid ARQ** (HARQ) and is what every production system uses:

1. Sender transmits block of K data + P parity
2. Receiver attempts to recover what it can from FEC alone
3. If a block is missing ≤ P packets, **reconstruct from parity, no NACK
   sent, no retransmit**
4. If a block is missing > P packets, fall back to ARQ: send a NACK (our
   existing SACK mechanism) for the unrecoverable losses, the sender
   retransmits

For Phase 7:

- The ARQ machinery (SACK, sliding window, RTO) **stays unchanged**
- We add a new "FEC layer" between the application and ARQ
- When a block is fully received (≤ P losses), the FEC layer delivers the
  recovered data to the application **without any retransmit**
- When a block has > P losses, the FEC layer passes the missing sequence
  numbers to ARQ for normal retransmission

### 6e. Pipelining

We do not wait for the receiver to ACK a block before sending the next.
Blocks are pipelined: while the receiver is decoding block N, the sender is
transmitting block N+1, N+2, etc. This is identical to sliding-window ARQ;
the existing infrastructure handles it.

### 6f. Out-of-order block delivery

If the sender transmits blocks `{1, 2, 3, 4}` back-to-back and block 2's
parity is lost, the receiver has block 1 complete (delivers), block 2
incomplete (1 loss recoverable from parity), block 3 complete (delivers
after block 2?), block 4 complete (delivers after block 3?).

**Application-level policy:** for a file transfer, in-order delivery to the
application is simplest and matches what users expect. So the FEC layer
buffers block 3 and 4 until block 2 is fully recoverable. Once recovered,
blocks 1, 2, 3, 4 are delivered in order.

This buffering is what the existing out-of-order receive buffer already
does for sliding-window ARQ. We reuse it.

---

## 7. Why this is interesting for RUDP — the design argument

Adding congestion control and fast retransmit (the original Phase 7 plan)
would make our RUDP behave more like TCP. That produces a worse TCP. The
benchmark would show RUDP "catching up," which is not a compelling story.

Adding FEC is different. **TCP does not have FEC.** TCP is purely ARQ-based.
Every lost packet triggers a retransmit, costing one RTT.

- For low-loss networks, ARQ (TCP-style) is fine — retransmits are rare.
- For high-loss networks (wireless, mobile, satellite, congested WANs),
  FEC-based protocols dominate because they avoid the RTT penalty.

So:

- **0% loss**: RUDP-FEC ≈ RUDP-ARQ (parity packets are overhead with no
  benefit)
- **1–5% loss**: RUDP-FEC >> RUDP-ARQ, because we recover from losses
  without waiting for RTO
- **10–20% loss**: RUDP-FEC > RUDP-ARQ until P losses per block are
  exceeded, then both fall back to ARQ for the rest

The benchmark will show: at low loss, RUDP-ARQ wins on bandwidth
efficiency. At moderate loss, RUDP-FEC wins on wall-clock time. This is a
real, defensible engineering tradeoff.

---

## 8. What we are going to implement — the concrete plan

This is the Phase 7 design. Subject to change, but this is the target.

### 8a. Header changes

Add `RUDP_FEC = 0x04` packet type. The 14-byte header stays the same shape;
FEC packets repurpose the `seq` field as the start-of-block sequence number
and use the first byte of payload to encode `(K, P, parity_index)`.

### 8b. Sender changes

```c
// Pseudocode for the sender side
block_buf[8][1024];        // buffer 8 data packets
block_filled = 0;
block_start_seq = next_seq;

for each data packet to send:
    block_buf[block_filled] = packet;
    rudp_send_sliding(packet);
    block_filled++;

    if block_filled == 8:
        parity = xor_parity(block_buf, 8);
        rudp_send_fec(parity, block_start_seq, K=8, P=1, index=0);
        block_start_seq += 8;
        block_filled = 0;
```

### 8c. Receiver changes

```c
// Pseudocode for the receiver side
block_buf[8][1024];        // buffer up to 8 data packets
block_received[8] = {false};
block_start = expected_block_start;

on each data packet received:
    if packet.seq is in [block_start, block_start + 7]:
        offset = packet.seq - block_start;
        block_buf[offset] = packet.payload;
        block_received[offset] = true;
        try_deliver_block();

on each parity packet received:
    block_start = packet.seq;
    block_parity = packet.payload;
    // store, but only USE when needed
    try_deliver_block();

try_deliver_block():
    if all 8 received:
        deliver in order
        advance block_start += 8
    elif block_parity known AND exactly 1 missing:
        missing_idx = index of false in block_received;
        block_buf[missing_idx] = xor(block_buf, except missing);
        // now all 8 received
        deliver in order
        advance block_start += 8
    // else: still missing data, wait
```

### 8d. What stays the same

- 14-byte packet header (just one new type value)
- Sliding window (`rudp_send_sliding` / `rudp_recv_sliding`)
- SACK and RTT/RTO
- File transfer CLI tools

### 8e. Estimated effort

- New `fec.h` and `fec.c`: ~50 LOC for XOR encoder/decoder
- Updates to `rudp_reliable.c`: ~30 LOC for the block-buffering logic
- Updates to `rudp_file.c` / CLI tools: ~10 LOC for the `-fec K` flag
- Phase 7 section in `RUDP_PROJECT.md`: ~150 lines
- Updated benchmark: re-run with FEC enabled, add a "FEC vs no-FEC" row
  per scenario

Total: about a sitting of focused work.

---

## 9. References

### Primary (read these in order)

1. **Rizzo, L. (1997). "Effective Erasure Codes for Reliable Computer
   Communication Protocols."** ACM SIGCOMM Computer Communication Review.
   The foundational paper. Short, readable, contains the first practical
   implementation. Key quote: "The key idea behind erasure codes is that
   k blocks of source data are encoded at the sender to produce n blocks
   of encoded data, in such a way that any subset of k encoded blocks
   suffices to reconstruct the source data."

   - PDF: https://www.cs.utexas.edu/~lam/395t/2010%20papers/FEC-rizzo.pdf
   - ACM: https://dl.acm.org/doi/10.1145/263876.263881

2. **Lacan, J., Roca, V., Peltotalo, J., Peltotalo, S. (2009).
   "Reed-Solomon Forward Error Correction (FEC) Schemes." RFC 5510.**
   The IETF standardization of the Rizzo-style RS scheme. The canonical
   reference for "how do I actually use RS in a network protocol."

   - https://www.rfc-editor.org/rfc/rfc5510

3. **Backblaze blog: "Erasure Coding: Reed-Solomon"** (2015). The most
   accessible explanation of the matrix view, with a worked 4+2 example.
   The diagrams in Section 4 above are adapted from this.

   - https://www.backblaze.com/blog/reed-solomon/

### Secondary (good for context, less essential)

4. **Wikipedia: Reed-Solomon error correction.** Canonical definition.
   The "Erasure coding" subsection is the relevant part for our use case;
   the bit-error sections are for storage/comm channels, not packet networks.

   - https://en.wikipedia.org/wiki/Reed%E2%80%93Solomon_error_correction

5. **Wikipedia: Fountain code.** A different but related idea (rateless
   codes, used in 3GPP/5G multicast). Worth knowing about for context,
   not what we are implementing.

   - https://en.wikipedia.org/wiki/Fountain_code

6. **Wikipedia: Forward error correction.** General background.

   - https://en.wikipedia.org/wiki/Forward_error_correction

### Implementation references (for Phase 7, when we code it)

7. **klauspost/reedsolomon** (Go library). A high-quality open-source RS
   implementation. Useful for cross-checking the code.

   - https://github.com/klauspost/reedsolomon

8. **Backblaze JavaReedSolomon.** The reference Java implementation that
   klauspost's Go port is based on. The "Reed-Solomon Encoding Matrix
   Example" section of the blog post mirrors this code.

   - https://github.com/Backblaze/JavaReedSolomon

9. **zfec** (Zooko / Tahoe-LAFS). The classic Python/C implementation of
   Rizzo's scheme. Public domain. Could be a reference port target.

   - https://github.com/tahoe-lafs/zfec

10. **Cody Planteen's Reed-Solomon notes.** Practical GF(256) tables and
    primitive-element references. Useful for the RS upgrade.

    - https://codyplanteen.com/notes/rs

### Historical (for context only)

11. **Reed, I. S., and Solomon, G. (1960). "Polynomial Codes over Certain
    Finite Fields."** The original 4-page paper. Read it for the historical
    lineage; it is dense but the structure is recognizable.

    - https://faculty.math.illinois.edu/~duursma/CT/RS-1960.pdf

12. **Rizzo, L. (1997). "Dummynet: A simple approach to the evaluation of
    network protocols."** Same author as the FEC paper. Cited 1297 times.

    - https://scholar.google.com/citations?hl=en&user=KdiIYYwAAAAJ

### What we explicitly are NOT reading (and why)

- The CMU page on symbol errors vs. bit errors. It is about storage, not
  packet networks. Wrong context for us.
- The Wikipedia "Error correction code" article in full. Too general,
  mostly covers bit-error cases (CDs, satellites). The "forward error
  correction" subsection is what we want.
- Any work on LDPC, Turbo codes, or Polar codes. These are bit-level
  codes for noisy channels (deep-space comms, 5G). Not the right tool
  for packet erasure, which is what we have.

---

## 10. Self-test questions

Before we start implementing, you should be able to answer these:

1. Why does XOR parity recover from exactly 1 loss, not more?
2. What is the bandwidth overhead of a `(K=8, P=1)` code?
3. What does the receiver do if a block has 2 losses when P=1?
4. Why does TCP not have FEC?
5. What is the relationship between Reed-Solomon and XOR parity?
6. In GF(256), what is the "addition" operation?
7. Why do we need a new packet type `RUDP_FEC`?
8. What is the difference between the sender's `K` data packets and the
   receiver's `K` data packets in an FEC scheme? (Hint: nothing physical
   — they are the same bytes — but the design is receiver-driven.)
9. Why does FEC eliminate the RTT cost of retransmission?
10. If we add RS with P=2 to our RUDP, how many losses per 10-packet block
    can we tolerate?

If any of these are unclear, re-read the relevant section above or the
Rizzo paper. The implementation will be much easier if these answers are
intuitive before writing C.