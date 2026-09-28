#!/usr/bin/env python3
"""analyze.py - computes every A28/A21/A29 number and the plots from the run CSVs.

Usage: python3 analyze.py [experiments_dir] [plots_dir]
Seed PUTs (A4) are the first N request_ids (N = number of workload files = 4)
and are excluded from every metric.
"""
import csv, math, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

EXP = sys.argv[1] if len(sys.argv) > 1 else "experiments"
OUT = sys.argv[2] if len(sys.argv) > 2 else "."
SEED_ROWS = 4
FILES = ["small.txt", "medium.txt", "large.txt", "large_longlines.txt"]
RUNS = [("fcfs_ref", "FCFS\n(4 thr)"), ("sjf_ref", "SJF\n(4 thr)"),
        ("rr_ref", "RR\n(4 thr)"), ("drr_ref", "DRR\n(4 thr)"),
        ("fcfs_1thread", "FCFS\n(1 thr)"), ("rr_1thread", "RR\n(1 thr)")]
REF = ["fcfs_ref", "sjf_ref", "rr_ref", "drr_ref"]

def load(name):
    rows = []
    with open(os.path.join(EXP, name + ".csv")) as f:
        for r in csv.DictReader(f):
            for k in ("request_id", "bytes", "rounds", "forfeited_bytes",
                      "arrival_ns", "start_ns", "finish_ns"):
                r[k] = int(r[k])
            rows.append(r)
    return [r for r in rows if r["request_id"] > SEED_ROWS]

def pct(vals, p):                       # nearest-rank (A22)
    v = sorted(vals)
    return v[max(0, math.ceil(p / 100 * len(v)) - 1)]

def a14(name):
    with open(os.path.join(EXP, name + "_server.log")) as f:
        m = re.search(r"A14 overruns:\s+(\d+)", f.read())
    return int(m.group(1)) if m else None

data = {n: load(n) for n, _ in RUNS}
stats = {}
print("## Per-run results (A28)")
print("| run | N | wait p50 ms | wait p99 ms | resp p50 ms | resp p99 ms | throughput req/s |")
print("|---|---|---|---|---|---|---|")
for n, _ in RUNS:
    R = data[n]
    w = [r["start_ns"] - r["arrival_ns"] for r in R]
    t = [r["finish_ns"] - r["arrival_ns"] for r in R]
    thr = len(R) / ((max(r["finish_ns"] for r in R) - min(r["arrival_ns"] for r in R)) / 1e9)
    stats[n] = dict(w50=pct(w, 50)/1e6, w99=pct(w, 99)/1e6,
                    r50=pct(t, 50)/1e6, r99=pct(t, 99)/1e6, thr=thr)
    s = stats[n]
    print(f"| {n} | {len(R)} | {s['w50']:.3f} | {s['w99']:.3f} | {s['r50']:.3f} | {s['r99']:.3f} | {s['thr']:.2f} |")

print("\n## Normalised slowdown, ns/byte, reference config (A21)")
print("| file | policy | n | median | p99 |")
print("|---|---|---|---|---|")
slow = {}
for n in REF:
    for fn in FILES:
        v = [(r["finish_ns"] - r["arrival_ns"]) / r["bytes"] for r in data[n] if r["filename"] == fn]
        slow[(n, fn)] = pct(v, 50)
        print(f"| {fn} | {n} | {len(v)} | {pct(v,50):.2f} | {pct(v,99):.2f} |")

print("\n## Forfeited bytes (GET) and A14 (A29)")
forf = {}
for n in ("rr_ref", "drr_ref"):
    for fn in FILES:
        forf[(n, fn)] = sum(r["forfeited_bytes"] for r in data[n] if r["filename"] == fn and r["op"] == "GET")
print("| file | RR | DRR |\n|---|---|---|")
for fn in FILES:
    print(f"| {fn} | {forf[('rr_ref', fn)]:,} | {forf[('drr_ref', fn)]:,} |")
print(f"A14 fired: RR ref={a14('rr_ref')}  RR 1thr={a14('rr_1thread')}  DRR ref={a14('drr_ref')}")
print("PUT forfeited total (must be 0):", sum(r["forfeited_bytes"] for n in data for r in data[n] if r["op"] == "PUT"))
print("Rounds per file (mean), rr_ref / drr_ref:")
for fn in FILES:
    m = lambda n: sum(r["rounds"] for r in data[n] if r["filename"] == fn and r["op"] == "GET") / max(1, sum(1 for r in data[n] if r["filename"] == fn and r["op"] == "GET"))
    print(f"  {fn}: {m('rr_ref'):.2f} / {m('drr_ref'):.2f}")

thr = sorted((stats[n]["thr"], n) for n, _ in RUNS if n in REF)
print("\nThroughput spread among reference runs: %.1f%% (rerun close pairs per A28)" % (100*(thr[-1][0]-thr[0][0])/thr[0][0]))

os.makedirs(OUT, exist_ok=True)
labels = [l for _, l in RUNS]; x = range(len(RUNS)); bw = 0.35
def two_bar(k1, k2, title, ylab, fn, c1, c2):
    plt.figure(figsize=(8, 5))
    plt.bar([i - bw/2 for i in x], [stats[n][k1] for n, _ in RUNS], bw, label="p50", color=c1)
    plt.bar([i + bw/2 for i in x], [stats[n][k2] for n, _ in RUNS], bw, label="p99", color=c2)
    plt.xticks(list(x), labels); plt.ylabel(ylab); plt.title(title); plt.legend()
    plt.tight_layout(); plt.savefig(os.path.join(OUT, fn), dpi=150); plt.close()
two_bar("w50", "w99", "Waiting time (start - arrival) by run — A19/A22/A28", "Waiting time (ms)", "plot_waiting.png", "#4c72b0", "#c44e52")
two_bar("r50", "r99", "Response time (finish - arrival) by run — A19/A22/A28", "Response time (ms)", "plot_response.png", "#55a868", "#8172b3")

plt.figure(figsize=(8, 5))
b = plt.bar(list(x), [stats[n]["thr"] for n, _ in RUNS], color="#dd8452")
for i, n in enumerate([n for n, _ in RUNS]):
    plt.text(i, stats[n]["thr"], f"{stats[n]['thr']:.0f}", ha="center", va="bottom")
plt.xticks(list(x), labels); plt.ylabel("Throughput (requests/sec)"); plt.title("Throughput by run — A20/A28")
plt.tight_layout(); plt.savefig(os.path.join(OUT, "plot_throughput.png"), dpi=150); plt.close()

plt.figure(figsize=(10, 5.5)); w4 = 0.2
for j, n in enumerate(REF):
    plt.bar([i + (j - 1.5) * w4 for i in range(4)], [slow[(n, f)] for f in FILES], w4, label=n.split("_")[0].upper())
plt.yscale("log"); plt.xticks(range(4), FILES, rotation=15); plt.ylabel("Median normalised slowdown (ns/byte)")
plt.title("Normalised slowdown (median) by file and policy — A21\n(reference config)"); plt.legend()
plt.tight_layout(); plt.savefig(os.path.join(OUT, "plot_slowdown.png"), dpi=150); plt.close()

plt.figure(figsize=(8, 5))
plt.bar([i - bw/2 for i in range(4)], [forf[("rr_ref", f)] for f in FILES], bw, label="RR", color="#c44e52")
plt.bar([i + bw/2 for i in range(4)], [forf[("drr_ref", f)] for f in FILES], bw, label="DRR", color="#4c72b0")
plt.yscale("symlog"); plt.xticks(range(4), FILES, rotation=15); plt.ylabel("Total forfeited bytes (log scale)")
plt.title("Forfeited allowance by file: RR vs DRR — A29\n(quantum=4096B, reference config)"); plt.legend()
plt.tight_layout(); plt.savefig(os.path.join(OUT, "plot_forfeiture.png"), dpi=150); plt.close()
print("\nplots written to", os.path.abspath(OUT))
print("\n## Normalised slowdown split by operation, ns/byte, reference config (supplementary to A21)")
print("| file | op | fcfs med | sjf med | rr med | drr med | fcfs p99 | sjf p99 | rr p99 | drr p99 |")
print("|---|---|---|---|---|---|---|---|---|---|")
for fn in FILES:
    for op in ("GET", "PUT"):
        meds, p99s = [], []
        for n in REF:
            v = [(r["finish_ns"] - r["arrival_ns"]) / r["bytes"] for r in data[n]
                 if r["filename"] == fn and r["op"] == op]
            meds.append(f"{pct(v, 50):.2f}"); p99s.append(f"{pct(v, 99):.2f}")
        print(f"| {fn} | {op} | " + " | ".join(meds) + " | " + " | ".join(p99s) + " |")
