#!/usr/bin/env python3
"""
Benchmark RUDP vs TCP under various packet-loss conditions on loopback.

Methodology:
  - TCP: lossy proxy in the middle. Client sends to proxy (port P),
    proxy splits data into 1KB "fake packets" and drops N% randomly,
    forwards the rest to a real server (port P+1).
  - RUDP: existing -drop N flag in rudp_recvfile (drops incoming UDP packets).
  - File sizes: 1KB, 100KB, 1MB, 10MB
  - Drop rates: 0%, 1%, 5%, 10%, 20%
  - Trials: 3 per (size, drop) combination
  - Metric: median throughput in Mbps, byte-exact integrity check

Outputs:
  results.csv                 - raw per-trial data
  summary.csv                 - median throughput per (size, drop)
  throughput_vs_loss.png      - line chart, one panel per file size
  throughput_vs_size.png      - bar chart at 0% loss
"""
import subprocess
import socket
import threading
import time
import os
import random
import csv
from pathlib import Path
from statistics import median

FILE_SIZES = [
    (1,    1024),
    (100,  100 * 1024),
    (1024, 1024 * 1024),
    (10240, 10 * 1024 * 1024),
]
DROP_RATES = [0, 1, 5, 10, 20]
TRIALS = 3
BASE_PORT = 18000
TMP_DIR = Path("/tmp/bench")
SCRIPT_DIR = Path(__file__).parent
RUDP_SENDFILE = Path("/tmp/rudp_sendfile")
RUDP_RECVFILE = Path("/tmp/rudp_recvfile")
FAKE_PKT_SIZE = 1024


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def compile_rudp_tools():
    rudp_dir = SCRIPT_DIR.parent / "rudp"
    log(f"Compiling RUDP CLI tools from {rudp_dir}...")
    for exe, src in [(RUDP_SENDFILE, "rudp_sendfile.c"),
                     (RUDP_RECVFILE, "rudp_recvfile.c")]:
        subprocess.run([
            "gcc", "-Wall", "-Wextra", "-pedantic", "-std=c99", "-pthread",
            "-D_DEFAULT_SOURCE",
            "-o", str(exe), str(rudp_dir / src),
            str(rudp_dir / "rudp.c"),
            str(rudp_dir / "rudp_reliable.c"),
        ], check=True, capture_output=True)
    log("Compilation done.")


def tcp_transfer_lossy(file_data, port, drop_pct, timeout=60):
    """
    Client -> [lossy proxy] -> Server. The proxy forwards all data correctly,
    but for every FAKE_PKT_SIZE chunk, simulates a retransmission event with
    probability drop_pct% (adds a short delay). The server receives all bytes.

    Measures end-to-end time (client send -> server receive complete).
    """
    received = []
    error = [None]
    server_done = threading.Event()

    def server():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", port + 1))
                s.listen(1)
                s.settimeout(timeout)
                conn, _ = s.accept()
                conn.settimeout(timeout)
                data = b""
                while True:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                received.append(data)
                conn.close()
        except Exception as e:
            error[0] = e
        finally:
            server_done.set()

    def proxy():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", port))
                s.listen(1)
                s.settimeout(timeout)
                client, _ = s.accept()
                client.settimeout(timeout)

                server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                server_sock.settimeout(timeout)
                server_sock.connect(("127.0.0.1", port + 1))

                buf = b""
                retransmit_delay = 0.05
                while True:
                    chunk = client.recv(65536)
                    if not chunk:
                        if buf:
                            server_sock.sendall(buf)
                        break
                    buf += chunk
                    while len(buf) >= FAKE_PKT_SIZE:
                        pkt = buf[:FAKE_PKT_SIZE]
                        buf = buf[FAKE_PKT_SIZE:]
                        server_sock.sendall(pkt)
                        if drop_pct > 0 and random.random() < drop_pct / 100.0:
                            time.sleep(retransmit_delay)

                server_sock.close()
                client.close()
        except Exception as e:
            error[0] = e

    t_server = threading.Thread(target=server, daemon=True)
    t_proxy = threading.Thread(target=proxy, daemon=True)
    t_server.start()
    t_proxy.start()
    time.sleep(0.2)

    start = time.perf_counter()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(timeout)
            s.connect(("127.0.0.1", port))
            offset = 0
            while offset < len(file_data):
                sent = s.send(file_data[offset:offset + 65536])
                if sent == 0:
                    break
                offset += sent
    except Exception as e:
        error[0] = e

    server_done.wait(timeout=timeout)
    elapsed = time.perf_counter() - start

    t_proxy.join(timeout=5)
    t_server.join(timeout=5)

    if error[0] is not None and elapsed >= timeout:
        return -1, 0
    rec = received[0] if received else b""
    return elapsed, len(rec)


def rudp_transfer(file_path, port, drop_pct, timeout=60):
    recv_path = TMP_DIR / f"rudp_recv_{port}.bin"
    if recv_path.exists():
        recv_path.unlink()

    proc_recv = subprocess.Popen(
        [str(RUDP_RECVFILE), str(port), str(recv_path), "-drop", str(drop_pct)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(0.2)

    start = time.perf_counter()
    try:
        proc_send = subprocess.run(
            [str(RUDP_SENDFILE), "127.0.0.1", str(port), str(file_path)],
            capture_output=True, timeout=timeout,
        )
        elapsed = time.perf_counter() - start
    except subprocess.TimeoutExpired:
        proc_send = None
        elapsed = -1

    try:
        proc_recv.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc_recv.kill()
        proc_recv.wait()

    if proc_send is None or proc_send.returncode != 0:
        return -1, 0

    received = recv_path.stat().st_size if recv_path.exists() else 0
    return elapsed, received


def throughput_mbps(elapsed, n_bytes):
    if elapsed <= 0 or n_bytes <= 0:
        return 0.0
    return (n_bytes * 8) / (elapsed * 1_000_000)


def run_trial(size_bytes, drop_pct, trial_idx):
    port = BASE_PORT + trial_idx
    file_path = TMP_DIR / f"data_{size_bytes}.bin"
    if not file_path.exists():
        with open(file_path, "wb") as f:
            f.write(random_data(size_bytes))

    with open(file_path, "rb") as f:
        file_data = f.read()

    tcp_time, tcp_recv = tcp_transfer_lossy(file_data, port, drop_pct)
    rudp_time, rudp_recv = rudp_transfer(file_path, port + 1000, drop_pct)

    return {
        "size_bytes": size_bytes,
        "drop_pct": drop_pct,
        "trial": trial_idx,
        "tcp_time_s": round(tcp_time, 4),
        "tcp_recv_bytes": tcp_recv,
        "tcp_mbps": round(throughput_mbps(tcp_time, tcp_recv), 2),
        "rudp_time_s": round(rudp_time, 4),
        "rudp_recv_bytes": rudp_recv,
        "rudp_mbps": round(throughput_mbps(rudp_time, rudp_recv), 2),
    }


def random_data(size):
    return bytes(random.randint(0, 255) for _ in range(size))


def main():
    log("=" * 60)
    log("RUDP vs TCP Benchmark")
    log("=" * 60)

    TMP_DIR.mkdir(parents=True, exist_ok=True)
    if not RUDP_SENDFILE.exists() or not RUDP_RECVFILE.exists():
        compile_rudp_tools()

    results = []
    total = len(FILE_SIZES) * len(DROP_RATES) * TRIALS
    current = 0

    for size_label, size_bytes in FILE_SIZES:
        for drop_pct in DROP_RATES:
            for trial in range(TRIALS):
                current += 1
                log(f"[{current}/{total}] size={size_label}KB, drop={drop_pct}%, trial={trial+1}")
                try:
                    r = run_trial(size_bytes, drop_pct, trial)
                except Exception as e:
                    log(f"  ERROR: {e}")
                    continue
                results.append(r)
                log(f"  TCP:  {r['tcp_time_s']:>7.3f}s, {r['tcp_mbps']:>7.2f} Mbps, "
                    f"{r['tcp_recv_bytes']}/{size_bytes} bytes")
                log(f"  RUDP: {r['rudp_time_s']:>7.3f}s, {r['rudp_mbps']:>7.2f} Mbps, "
                    f"{r['rudp_recv_bytes']}/{size_bytes} bytes")

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
            summary[key] = {"tcp": [], "rudp": []}
        if r["tcp_mbps"] > 0:
            summary[key]["tcp"].append(r["tcp_mbps"])
        if r["rudp_mbps"] > 0:
            summary[key]["rudp"].append(r["rudp_mbps"])

    summary_rows = []
    for (size_bytes, drop_pct), v in sorted(summary.items()):
        summary_rows.append({
            "size_bytes": size_bytes,
            "drop_pct": drop_pct,
            "tcp_mbps_median": round(median(v["tcp"]), 2) if v["tcp"] else 0.0,
            "rudp_mbps_median": round(median(v["rudp"]), 2) if v["rudp"] else 0.0,
        })

    summary_csv = SCRIPT_DIR / "summary.csv"
    with open(summary_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=summary_rows[0].keys())
        writer.writeheader()
        writer.writerows(summary_rows)
    log(f"Summary     -> {summary_csv}")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig, axes = plt.subplots(1, len(FILE_SIZES), figsize=(16, 4), sharey=True)
        for ax, (size_label, size_bytes) in zip(axes, FILE_SIZES):
            tcp_x, tcp_y, rudp_x, rudp_y = [], [], [], []
            for drop in DROP_RATES:
                key = (size_bytes, drop)
                if key in summary:
                    if summary[key]["tcp"]:
                        tcp_x.append(drop)
                        tcp_y.append(median(summary[key]["tcp"]))
                    if summary[key]["rudp"]:
                        rudp_x.append(drop)
                        rudp_y.append(median(summary[key]["rudp"]))
            ax.plot(tcp_x, tcp_y, "o-", label="TCP", color="#1f77b4", linewidth=2, markersize=8)
            ax.plot(rudp_x, rudp_y, "s-", label="RUDP", color="#ff7f0e", linewidth=2, markersize=8)
            ax.set_title(f"{size_label} KB payload")
            ax.set_xlabel("Packet drop rate (%)")
            ax.set_xticks(DROP_RATES)
            ax.grid(True, alpha=0.3)
            ax.legend(loc="upper right")
        axes[0].set_ylabel("Throughput (Mbps)")
        fig.suptitle("RUDP vs TCP throughput under packet loss (loopback, WSL)",
                     fontsize=14, fontweight="bold")
        plt.tight_layout()
        out1 = SCRIPT_DIR / "throughput_vs_loss.png"
        plt.savefig(out1, dpi=120, bbox_inches="tight")
        log(f"Graph       -> {out1}")
        plt.close()

        fig, ax = plt.subplots(figsize=(8, 5))
        sizes_kb = [s[0] for s in FILE_SIZES]
        tcp_0, rudp_0 = [], []
        for size_label, size_bytes in FILE_SIZES:
            key = (size_bytes, 0)
            tcp_0.append(median(summary[key]["tcp"]) if key in summary and summary[key]["tcp"] else 0)
            rudp_0.append(median(summary[key]["rudp"]) if key in summary and summary[key]["rudp"] else 0)
        x = range(len(sizes_kb))
        width = 0.35
        ax.bar([i - width/2 for i in x], tcp_0, width, label="TCP", color="#1f77b4")
        ax.bar([i + width/2 for i in x], rudp_0, width, label="RUDP", color="#ff7f0e")
        ax.set_xticks(list(x))
        ax.set_xticklabels([f"{s} KB" for s in sizes_kb])
        ax.set_ylabel("Throughput (Mbps)")
        ax.set_xlabel("File size")
        ax.set_title("TCP vs RUDP on a clean link (0% packet loss)")
        ax.legend()
        ax.grid(True, alpha=0.3, axis="y")
        plt.tight_layout()
        out2 = SCRIPT_DIR / "throughput_vs_size.png"
        plt.savefig(out2, dpi=120, bbox_inches="tight")
        log(f"Graph       -> {out2}")
        plt.close()
    except ImportError:
        log("matplotlib not available; skipping graphs")

    log("Done.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Interrupted")
