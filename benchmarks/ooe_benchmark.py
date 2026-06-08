#!/usr/bin/env python3
"""
OOE (Out-Of-Order) benchmark: RUDP vs TCP under loss.

Measures per-message delivery latency. Compares RUDP out-of-order
delivery against TCP in-order delivery (head-of-line blocking).

Outputs:
  ooe_results.csv   - per-trial summary of delivery percentiles
  ooe_raw_*.csv     - per-trial raw per-message timestamps
"""
import os
import sys
import time
import subprocess
import csv
from pathlib import Path

N_MESSAGES = 1000
MSG_SIZE = 1024
DROP_RATES = [0, 1, 5, 10]
TRIALS = 3
PER_TRIAL_TIMEOUT_S = 300
RECV_PORT_BASE = 22000
PROXY_PORT_BASE = 23000
RETV_DELAY_US = 100000

TMP_DIR = Path("/tmp/ooe_bench")
SCRIPT_DIR = Path(__file__).parent
RUDP_DIR = SCRIPT_DIR.parent / "rudp"
OOE_SEND = SCRIPT_DIR / "ooe_send"
OOE_RECV_RUDP = SCRIPT_DIR / "ooe_recv_rudp"
OOE_TCP_SEND = SCRIPT_DIR / "ooe_tcp_send"
OOE_TCP_RECV = SCRIPT_DIR / "ooe_tcp_recv"
LOSS_PROXY = TMP_DIR / "loss_proxy"
CC = ["gcc", "-Wall", "-Wextra", "-pedantic", "-std=c99", "-pthread", "-D_DEFAULT_SOURCE"]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def compile_all():
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    log("Compiling OOE tools...")
    subprocess.run(CC + ["-Irudp", "-o", str(OOE_SEND),
                         str(SCRIPT_DIR / "ooe_send.c"),
                         str(RUDP_DIR / "rudp.c"),
                         str(RUDP_DIR / "rudp_reliable.c"),
                         str(RUDP_DIR / "fec.c")], check=True, capture_output=True)
    subprocess.run(CC + ["-Irudp", "-o", str(OOE_RECV_RUDP),
                         str(SCRIPT_DIR / "ooe_recv_rudp.c"),
                         str(RUDP_DIR / "rudp.c"),
                         str(RUDP_DIR / "rudp_reliable.c"),
                         str(RUDP_DIR / "fec.c")], check=True, capture_output=True)
    subprocess.run(CC + ["-o", str(OOE_TCP_SEND),
                         str(SCRIPT_DIR / "ooe_tcp_send.c")], check=True, capture_output=True)
    subprocess.run(CC + ["-o", str(OOE_TCP_RECV),
                         str(SCRIPT_DIR / "ooe_tcp_recv.c")], check=True, capture_output=True)
    log("Compiling loss proxy...")
    subprocess.run(CC + ["-o", str(LOSS_PROXY),
                         str(SCRIPT_DIR / "loss_proxy.c")], check=True, capture_output=True)
    log("Compilation done.")


def start_proc(args, log_path):
    f = open(str(log_path), "w")
    return subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=f, stderr=f)


def start_proxy(protocol, listen_port, upstream_port, drop_pct, seed, log_path):
    if protocol == "rudp":
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


def kill_proc(proc):
    if proc is None: return
    try: proc.kill()
    except: pass
    try: proc.wait(timeout=2)
    except: pass


def run_trial(protocol, drop_pct, trial):
    port = RECV_PORT_BASE + trial * 10
    recv_port = port
    proxy_port = port + PROXY_PORT_BASE - RECV_PORT_BASE
    seed = trial * 100003 + drop_pct * 101

    proxy_log = TMP_DIR / f"proxy_{protocol}_{trial}_{drop_pct}.log"
    recv_log  = TMP_DIR / f"recv_{protocol}_{trial}_{drop_pct}.log"
    send_log  = TMP_DIR / f"send_{protocol}_{trial}_{drop_pct}.log"
    raw_csv   = TMP_DIR / f"raw_{protocol}_{trial}_{drop_pct}.csv"

    proxy = start_proxy(protocol, proxy_port, recv_port, drop_pct, seed, proxy_log)
    time.sleep(0.2)

    if protocol == "rudp":
        recv = start_proc([str(OOE_RECV_RUDP), str(recv_port), str(N_MESSAGES)], recv_log)
    else:
        recv = start_proc([str(OOE_TCP_RECV), str(recv_port), str(N_MESSAGES)], recv_log)
    time.sleep(0.2)

    if protocol == "rudp":
        send_cmd = [str(OOE_SEND), "127.0.0.1", str(proxy_port), str(N_MESSAGES), str(MSG_SIZE)]
    else:
        send_cmd = [str(OOE_TCP_SEND), "127.0.0.1", str(proxy_port), str(N_MESSAGES), str(MSG_SIZE)]

    timed_out = False
    start_t = time.perf_counter()
    try:
        with open(str(send_log), "w") as f:
            subprocess.run(send_cmd, stdin=subprocess.DEVNULL, stdout=f, stderr=f,
                           timeout=PER_TRIAL_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        timed_out = True

    if not timed_out:
        deadline = time.perf_counter() + 35
        while recv.poll() is None and time.perf_counter() < deadline:
            time.sleep(0.1)
        if recv.poll() is None:
            timed_out = True

    elapsed = time.perf_counter() - start_t
    kill_proc(recv)
    kill_proc(proxy)
    time.sleep(0.5)

    results = None
    if raw_csv.exists():
        raw_csv.unlink()
    if recv_log.exists():
        # The CSV output goes to stdout which was redirected to recv_log
        pass
    # Parse the raw timestamps from receiver output
    arrivals = []
    try:
        with open(str(recv_log)) as f:
            for line in f:
                if "," in line:
                    parts = line.strip().split(",")
                    if len(parts) == 2:
                        seq = int(parts[0])
                        ts = int(parts[1])
                        if ts >= 0:
                            arrivals.append((seq, ts))
    except:
        pass

    if arrivals:
        pcts = [50, 90, 95, 99, 100]
        times = [a[1] for a in arrivals]
        times.sort()
        ms_delays = {}
        total = len(times)
        for p in pcts:
            idx = int(total * p / 100)
            if idx >= total: idx = total - 1
            ms_delays[f"p{p}_ms"] = round(times[idx] / 1000.0, 3)

        results = {
            "protocol": protocol,
            "drop_pct": drop_pct,
            "trial": trial,
            "n": total,
            "elapsed_s": round(elapsed, 3),
            "timed_out": timed_out,
            **ms_delays,
        }

        # Write raw CSV for this trial
        with open(str(raw_csv), "w") as f:
            f.write("seq,ts_us\n")
            for seq, ts in sorted(arrivals, key=lambda x: x[1]):
                f.write(f"{seq},{ts}\n")

    return results


def main():
    compile_all()

    all_results = []
    for drop_pct in DROP_RATES:
        for trial in range(TRIALS):
            for protocol in ("rudp", "tcp"):
                log(f"Trial {trial+1}/{TRIALS}: {protocol} @ {drop_pct}% drop")
                r = run_trial(protocol, drop_pct, trial)
                if r:
                    all_results.append(r)
                    n = r.get("n", 0)
                    p50 = r.get("p50_ms", "?")
                    p99 = r.get("p99_ms", "?")
                    to = "TIMEOUT" if r.get("timed_out") else "OK"
                    log(f"  -> {n} msgs, p50={p50}ms p99={p99}ms elapsed={r['elapsed_s']}s {to}")

    log("Writing results...")
    out_path = SCRIPT_DIR / "ooe_results.csv"
    if all_results:
        fieldnames = ["protocol", "drop_pct", "trial", "n", "elapsed_s",
                      "timed_out", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "p100_ms"]
        with open(str(out_path), "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for r in all_results:
                writer.writerow(r)
        log(f"Wrote {len(all_results)} results to {out_path}")
    else:
        log("No results collected!")

    # Compute summary
    from statistics import median
    print()
    print("=" * 80)
    print(f"Summary — {N_MESSAGES}x{MSG_SIZE}byte messages")
    print(f"{'Protocol':<8} {'Drop%':>5} {'p50_ms':>8} {'p90_ms':>8} {'p99_ms':>8} {'p100_ms':>9} {'n':>5}")
    print("-" * 55)
    for protocol in ("rudp", "tcp"):
        for drop_pct in DROP_RATES:
            trials_data = [r for r in all_results
                           if r["protocol"] == protocol and r["drop_pct"] == drop_pct]
            if not trials_data:
                continue
            n_avg = int(median(r["n"] for r in trials_data))
            p50 = round(median(r["p50_ms"] for r in trials_data), 1)
            p90 = round(median(r["p90_ms"] for r in trials_data), 1)
            p99 = round(median(r["p99_ms"] for r in trials_data), 1)
            p100 = round(median(r["p100_ms"] for r in trials_data), 1)
            print(f"{protocol:<8} {drop_pct:>5} {p50:>8} {p90:>8} {p99:>8} {p100:>9} {n_avg:>5}")
    print("=" * 80)


if __name__ == "__main__":
    main()
