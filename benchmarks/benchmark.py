#!/usr/bin/env python3
"""
Fair RUDP vs RUDP-FEC vs RUDP-FECv2 vs TCP benchmark on loopback (WSL).

All four protocols are tested with the same C code path, the same I/O pattern,
and the same loss proxy. The only difference is the transport itself.

Methodology:
  - RUDP, RUDP-FEC, RUDP-FECv2, and TCP CLI tools are all written in C.
  - A single C loss_proxy injects packet loss symmetrically.
  - For UDP (RUDP, RUDP-FEC, RUDP-FECv2): real packet drops in the sender->receiver direction.
    SACK/FIN/ACK flow freely so the receiver can drive retransmits.
  - For TCP: delay-based "loss" cost (proxy sleeps 100ms then forwards),
    approximating the RTO retransmit cost of a real lossy link.
    (WSL has no kernel-level loss injection: no tc netem, no iptables.)
  - Same loss seed for all protocols per trial, deterministic pattern.
  - Per-trial timeout: 60 s. Failures recorded as throughput=0.
  - Trials: 3 per (size, drop) combination.
  - Drops: 0, 1, 5, 10 %. (20% omitted to keep runtime within 30 min.)

Outputs:
  results.csv                 - raw per-trial data
  summary.csv                 - median throughput per (size, drop)
  throughput_vs_loss.png      - line chart, one panel per file size
  throughput_vs_size.png      - bar chart at 0% loss
"""
import os
import sys
import time
import random
import subprocess
import csv
import signal
from pathlib import Path
from statistics import median

FILE_SIZES = [
    ("100KB",  100 * 1024),
    ("1MB",    1024 * 1024),
    ("10MB",   10 * 1024 * 1024),
]
DROP_RATES = [0, 1, 5, 10]
TRIALS = 3
PER_TRIAL_TIMEOUT_S = 60
RECV_SETTLE_S = 0.3
RECV_PORT_BASE = 21000
PROXY_PORT_BASE = 21100
RETV_DELAY_US = 100000
PROTOCOLS = ("rudp", "rudp_fec", "rudp_fec_v2", "tcp")

TMP_DIR = Path("/tmp/bench")
SCRIPT_DIR = Path(__file__).parent
RUDP_DIR = SCRIPT_DIR.parent / "rudp"
RUDP_SENDFILE = TMP_DIR / "rudp_sendfile"
RUDP_RECVFILE = TMP_DIR / "rudp_recvfile"
TCP_SENDFILE = TMP_DIR / "tcp_sendfile"
TCP_RECVFILE = TMP_DIR / "tcp_recvfile"
LOSS_PROXY   = TMP_DIR / "loss_proxy"
CC = ["gcc", "-Wall", "-Wextra", "-pedantic", "-std=c99", "-pthread", "-D_DEFAULT_SOURCE"]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def compile_all():
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    log("Compiling RUDP CLI tools...")
    for exe, src in [(RUDP_SENDFILE, "rudp_sendfile.c"),
                     (RUDP_RECVFILE, "rudp_recvfile.c")]:
        subprocess.run(CC + ["-o", str(exe),
                              str(RUDP_DIR / src),
                              str(RUDP_DIR / "rudp.c"),
                              str(RUDP_DIR / "rudp_reliable.c"),
                              str(RUDP_DIR / "fec.c")],
                       check=True, capture_output=True)
    log("Compiling TCP CLI tools...")
    for exe, src in [(TCP_SENDFILE, "tcp_sendfile.c"),
                     (TCP_RECVFILE, "tcp_recvfile.c")]:
        subprocess.run(CC + ["-o", str(exe),
                              str(SCRIPT_DIR / src)],
                       check=True, capture_output=True)
    log("Compiling loss proxy...")
    subprocess.run(CC + ["-o", str(LOSS_PROXY),
                          str(SCRIPT_DIR / "loss_proxy.c")],
                   check=True, capture_output=True)
    log("Compilation done.")


def gen_data_file(path, size_bytes):
    if path.exists() and path.stat().st_size == size_bytes:
        return
    with open(path, "wb") as f:
        f.write(os.urandom(size_bytes))


def md5sum(path):
    out = subprocess.run(["md5sum", str(path)],
                         capture_output=True, check=True)
    return out.stdout.decode().split()[0]


def kill_proc(proc):
    if proc is None:
        return
    try:
        proc.kill()
    except Exception:
        pass
    try:
        proc.wait(timeout=2)
    except Exception:
        pass


def start_proc(args, log_path):
    f = open(log_path, "w")
    return subprocess.Popen(
        args,
        stdin=subprocess.DEVNULL,
        stdout=f,
        stderr=subprocess.DEVNULL,
        preexec_fn=os.setsid,
    )


def start_proxy(protocol, listen_port, upstream_port, drop_pct, seed, log_path):
    if protocol.startswith("rudp"):
        return start_proc([
            str(LOSS_PROXY), "-proto", "udp",
            "-listen", str(listen_port),
            "-upstream", f"127.0.0.1:{upstream_port}",
            "-drop", str(drop_pct), "-seed", str(seed),
        ], log_path)
    else:
        return start_proc([
            str(LOSS_PROXY), "-proto", "tcp",
            "-listen", str(listen_port),
            "-upstream", f"127.0.0.1:{upstream_port}",
            "-drop", str(drop_pct), "-seed", str(seed),
            "-retx-delay", str(RETV_DELAY_US),
        ], log_path)


def cleanup_all(*procs):
    for p in procs:
        kill_proc(p)


def run_trial(protocol, size_bytes, drop_pct, trial, file_path):
    port = RECV_PORT_BASE + trial * 10
    recv_port = port
    proxy_port = port + PROXY_PORT_BASE - RECV_PORT_BASE
    recv_path = TMP_DIR / f"recv_{protocol}_{trial}_{size_bytes}.bin"

    seed = trial * 100003 + drop_pct * 101 + size_bytes

    proxy_log = TMP_DIR / f"proxy_{protocol}_{trial}.log"
    recv_log  = TMP_DIR / f"recv_{protocol}_{trial}.log"
    send_log  = TMP_DIR / f"send_{protocol}_{trial}.log"

    proxy = start_proxy(protocol, proxy_port, recv_port, drop_pct, seed, proxy_log)
    time.sleep(0.2)

    if protocol.startswith("rudp"):
        if protocol == "rudp_fec":
            recv = start_proc([str(RUDP_RECVFILE), str(recv_port), str(recv_path),
                               "-fec", "K=8"], recv_log)
        elif protocol == "rudp_fec_v2":
            recv = start_proc([str(RUDP_RECVFILE), str(recv_port), str(recv_path),
                               "-fecv2", "K=8"], recv_log)
        else:
            recv = start_proc([str(RUDP_RECVFILE), str(recv_port), str(recv_path)],
                              recv_log)
    else:
        recv = start_proc([str(TCP_RECVFILE), str(recv_port), str(recv_path)],
                          recv_log)
    time.sleep(0.2)

    if protocol.startswith("rudp"):
        if protocol == "rudp_fec":
            send_cmd = [str(RUDP_SENDFILE), "127.0.0.1", str(proxy_port),
                        str(file_path), "-fec", "K=8"]
        elif protocol == "rudp_fec_v2":
            send_cmd = [str(RUDP_SENDFILE), "127.0.0.1", str(proxy_port),
                        str(file_path), "-fecv2", "K=8"]
        else:
            send_cmd = [str(RUDP_SENDFILE), "127.0.0.1", str(proxy_port), str(file_path)]
    else:
        send_cmd = [str(TCP_SENDFILE), "127.0.0.1", str(proxy_port), str(file_path)]

    timed_out = False
    start = time.perf_counter()
    try:
        with open(send_log, "w") as f:
            subprocess.run(send_cmd, stdin=subprocess.DEVNULL,
                           stdout=f, stderr=subprocess.DEVNULL,
                           timeout=PER_TRIAL_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        timed_out = True

    if not timed_out:
        deadline = time.perf_counter() + 5
        while recv.poll() is None and time.perf_counter() < deadline:
            time.sleep(0.05)
        if recv.poll() is None:
            timed_out = True
    elapsed = time.perf_counter() - start

    cleanup_all(recv, proxy)
    time.sleep(RECV_SETTLE_S)

    if recv_path.exists():
        received = recv_path.stat().st_size
    else:
        received = 0

    return elapsed, received, timed_out


def throughput_mbps(elapsed, n_bytes):
    if elapsed <= 0 or n_bytes <= 0:
        return 0.0
    return (n_bytes * 8) / (elapsed * 1_000_000)


def main():
    log("=" * 60)
    log("Fair RUDP vs RUDP-FEC vs RUDP-FECv2 vs TCP benchmark (all C, symmetric loss proxy)")
    log("=" * 60)

    compile_all()

    for label, size_bytes in FILE_SIZES:
        gen_data_file(TMP_DIR / f"data_{size_bytes}.bin", size_bytes)

    results = []
    total = len(FILE_SIZES) * len(DROP_RATES) * TRIALS * len(PROTOCOLS)
    current = 0
    bench_start = time.perf_counter()

    for size_label, size_bytes in FILE_SIZES:
        file_path = TMP_DIR / f"data_{size_bytes}.bin"
        for drop_pct in DROP_RATES:
            for trial in range(TRIALS):
                for protocol in PROTOCOLS:
                    current += 1
                    log(f"[{current}/{total}] {protocol:4s} size={size_label:5s} "
                        f"drop={drop_pct:2d}% trial={trial+1}")
                    elapsed, received, timed_out = run_trial(
                        protocol, size_bytes, drop_pct, trial, file_path)
                    mbps = throughput_mbps(elapsed, received)
                    results.append({
                        "protocol": protocol,
                        "size_bytes": size_bytes,
                        "drop_pct": drop_pct,
                        "trial": trial + 1,
                        "elapsed_s": round(elapsed, 4),
                        "received_bytes": received,
                        "mbps": round(mbps, 2),
                        "timed_out": timed_out,
                    })
                    tag = "TIMEOUT" if timed_out else f"{elapsed:7.3f}s"
                    log(f"  -> {tag}, {received}/{size_bytes} bytes, {mbps:7.2f} Mbps")

                    elapsed_min = (time.perf_counter() - bench_start) / 60
                    if elapsed_min > 28:
                        log(f"  (Approaching 30-min budget at {elapsed_min:.1f} min, "
                            "stopping early)")
                        break
                else:
                    continue
                break
            else:
                continue
            break
        else:
            continue
        break

    raw_csv = SCRIPT_DIR / "results.csv"
    with open(raw_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)
    log(f"Raw results -> {raw_csv}")

    summary = {}
    for r in results:
        key = (r["size_bytes"], r["drop_pct"])
        if key not in summary:
            summary[key] = {"rudp": [], "rudp_fec": [], "rudp_fec_v2": [], "tcp": []}
        if r["mbps"] > 0:
            summary[key][r["protocol"]].append(r["mbps"])

    summary_rows = []
    for (size_bytes, drop_pct) in sorted(summary.keys()):
        row = {
            "size_bytes": size_bytes,
            "drop_pct": drop_pct,
            "rudp_mbps_median": (round(median(summary[(size_bytes, drop_pct)]["rudp"]), 2)
                                  if summary[(size_bytes, drop_pct)]["rudp"] else 0.0),
            "rudp_fec_mbps_median": (round(median(summary[(size_bytes, drop_pct)]["rudp_fec"]), 2)
                                      if summary[(size_bytes, drop_pct)]["rudp_fec"] else 0.0),
            "rudp_fec_v2_mbps_median": (round(median(summary[(size_bytes, drop_pct)]["rudp_fec_v2"]), 2)
                                         if summary[(size_bytes, drop_pct)]["rudp_fec_v2"] else 0.0),
            "tcp_mbps_median": (round(median(summary[(size_bytes, drop_pct)]["tcp"]), 2)
                                 if summary[(size_bytes, drop_pct)]["tcp"] else 0.0),
        }
        summary_rows.append(row)

    summary_csv = SCRIPT_DIR / "summary.csv"
    with open(summary_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=summary_rows[0].keys())
        writer.writeheader()
        writer.writerows(summary_rows)
    log(f"Summary     -> {summary_csv}")

    log(f"Total elapsed: {(time.perf_counter() - bench_start) / 60:.1f} min")
    log("Run analyze.py separately to generate graphs.")
    log("Done.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Interrupted; cleaning up...")
        subprocess.run(["pkill", "-f", "loss_proxy"])
        subprocess.run(["pkill", "-f", "rudp_recvfile"])
        subprocess.run(["pkill", "-f", "rudp_sendfile"])
        subprocess.run(["pkill", "-f", "tcp_recvfile"])
        subprocess.run(["pkill", "-f", "tcp_sendfile"])
        sys.exit(1)
