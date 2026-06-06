#!/usr/bin/env python3
"""
Generate matplotlib graphs from benchmark results.

Reads:
  summary.csv
    columns: size_bytes, drop_pct, rudp_mbps_median, tcp_mbps_median

Writes:
  throughput_vs_loss.png   - one panel per file size
  throughput_vs_size.png   - bar chart at 0% loss
"""
import sys
import csv
from pathlib import Path
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SCRIPT_DIR = Path(__file__).parent
SUMMARY_CSV = SCRIPT_DIR / "summary.csv"


def load_summary():
    rows = []
    with open(SUMMARY_CSV) as f:
        for row in csv.DictReader(f):
            row["size_bytes"] = int(row["size_bytes"])
            row["drop_pct"] = int(row["drop_pct"])
            row["rudp_mbps_median"] = float(row["rudp_mbps_median"])
            row["tcp_mbps_median"] = float(row["tcp_mbps_median"])
            rows.append(row)
    return rows


def label_for(size_bytes):
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes // (1024 * 1024)}MB"
    if size_bytes >= 1024:
        return f"{size_bytes // 1024}KB"
    return f"{size_bytes}B"


def plot_throughput_vs_loss(rows):
    by_size = defaultdict(list)
    for r in rows:
        by_size[r["size_bytes"]].append(r)

    sizes = sorted(by_size.keys())
    n = len(sizes)
    cols = min(n, 2)
    rows_grid = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows_grid, cols, figsize=(6 * cols, 4 * rows_grid),
                             squeeze=False)

    drop_pcts = sorted({r["drop_pct"] for r in rows})

    for idx, size_bytes in enumerate(sizes):
        ax = axes[idx // cols][idx % cols]
        sub = sorted(by_size[size_bytes], key=lambda r: r["drop_pct"])
        rudp = [r["rudp_mbps_median"] for r in sub]
        tcp  = [r["tcp_mbps_median"] for r in sub]
        ax.plot(drop_pcts[:len(sub)], rudp, "o-", label="RUDP", linewidth=2)
        ax.plot(drop_pcts[:len(sub)], tcp,  "s-", label="TCP",  linewidth=2)
        ax.set_title(f"{label_for(size_bytes)} file")
        ax.set_xlabel("Drop rate (%)")
        ax.set_ylabel("Throughput (Mbps)")
        ax.grid(True, alpha=0.3)
        ax.legend()

    for idx in range(n, rows_grid * cols):
        axes[idx // cols][idx % cols].axis("off")

    fig.suptitle("RUDP vs TCP: Throughput vs Loss Rate (medians, fair C-vs-C loopback test)",
                 fontsize=13)
    fig.tight_layout()
    out = SCRIPT_DIR / "throughput_vs_loss.png"
    fig.savefig(out, dpi=110)
    plt.close(fig)
    print(f"Wrote {out}")


def plot_throughput_vs_size(rows):
    by_drop = defaultdict(list)
    for r in rows:
        by_drop[r["drop_pct"]].append(r)

    if 0 not in by_drop:
        print("No 0% data; skipping throughput_vs_size.png")
        return

    sub = sorted(by_drop[0], key=lambda r: r["size_bytes"])
    labels = [label_for(r["size_bytes"]) for r in sub]
    rudp   = [r["rudp_mbps_median"] for r in sub]
    tcp    = [r["tcp_mbps_median"] for r in sub]

    fig, ax = plt.subplots(figsize=(8, 5))
    x = range(len(labels))
    w = 0.35
    ax.bar([i - w/2 for i in x], rudp, w, label="RUDP", color="tab:blue")
    ax.bar([i + w/2 for i in x], tcp,  w, label="TCP",  color="tab:orange")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_xlabel("File size")
    ax.set_ylabel("Throughput (Mbps)")
    ax.set_title("RUDP vs TCP at 0% Loss (median of trials)")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend()
    for i, (a, b) in enumerate(zip(rudp, tcp)):
        ax.text(i - w/2, a + max(rudp + tcp) * 0.01, f"{a:.0f}", ha="center", fontsize=8)
        ax.text(i + w/2, b + max(rudp + tcp) * 0.01, f"{b:.0f}", ha="center", fontsize=8)

    fig.tight_layout()
    out = SCRIPT_DIR / "throughput_vs_size.png"
    fig.savefig(out, dpi=110)
    plt.close(fig)
    print(f"Wrote {out}")


def main():
    if not SUMMARY_CSV.exists():
        print(f"Missing {SUMMARY_CSV}; run benchmark.py first.", file=sys.stderr)
        sys.exit(1)
    rows = load_summary()
    if not rows:
        print("summary.csv is empty; nothing to plot.", file=sys.stderr)
        sys.exit(1)
    plot_throughput_vs_loss(rows)
    plot_throughput_vs_size(rows)
    print("Done.")


if __name__ == "__main__":
    main()
