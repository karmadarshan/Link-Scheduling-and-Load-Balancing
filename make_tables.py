#!/usr/bin/env python3
"""make_tables.py - renders the A28/A21/A29 tables (and a p99 slowdown plot) as PNGs.

Usage: python3 make_tables.py [experiments_dir] [rerun_dir] [out_dir]
Seed PUTs (first 4 request_ids) are excluded, as in analyze.py.
"""
import csv, math, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

EXP = sys.argv[1] if len(sys.argv) > 1 else "experiments"
RER = sys.argv[2] if len(sys.argv) > 2 else "experiments_rerun"
OUT = sys.argv[3] if len(sys.argv) > 3 else "."
SEED_ROWS = 4
FILES = ["small.txt", "medium.txt", "large.txt", "large_longlines.txt"]
RUNS = ["fcfs_ref", "sjf_ref", "rr_ref", "drr_ref", "fcfs_1thread", "rr_1thread"]
REF = RUNS[:4]
NICE = {"fcfs_ref": "FCFS (4 thr)", "sjf_ref": "SJF (4 thr)", "rr_ref": "RR (4 thr)",
        "drr_ref": "DRR (4 thr)", "fcfs_1thread": "FCFS (1 thr)", "rr_1thread": "RR (1 thr)"}

def load(d, name):
    rows = []
    with open(os.path.join(d, name + ".csv")) as f:
        for r in csv.DictReader(f):
            for k in ("request_id", "bytes", "rounds", "forfeited_bytes", "arrival_ns", "start_ns", "finish_ns"):
                r[k] = int(r[k])
            rows.append(r)
    return [r for r in rows if r["request_id"] > SEED_ROWS]

def pct(vals, p):
    v = sorted(vals)
    return v[max(0, math.ceil(p / 100 * len(v)) - 1)]

def a14(d, name):
    with open(os.path.join(d, name + "_server.log")) as f:
        m = re.search(r"A14 overruns:\s+(\d+)", f.read())
    return int(m.group(1)) if m else -1

def run_stats(rows):
    w = [r["start_ns"] - r["arrival_ns"] for r in rows]
    t = [r["finish_ns"] - r["arrival_ns"] for r in rows]
    thr = len(rows) / ((max(r["finish_ns"] for r in rows) - min(r["arrival_ns"] for r in rows)) / 1e9)
    return [pct(w, 50)/1e6, pct(w, 99)/1e6, pct(t, 50)/1e6, pct(t, 99)/1e6, thr]

def slow(rows, fn, op=None):
    return [(r["finish_ns"] - r["arrival_ns"]) / r["bytes"] for r in rows
            if r["filename"] == fn and (op is None or r["op"] == op)]

def render(title, header, body, fname, colw=None, bold_rows=()):
    os.makedirs(OUT, exist_ok=True)
    h = 0.5 + 0.34 * (len(body) + 1)
    fig, ax = plt.subplots(figsize=(max(7, 1.5 * len(header)), h))
    ax.axis("off")
    ax.set_title(title, fontsize=11, fontweight="bold", pad=8)
    t = ax.table(cellText=body, colLabels=header, loc="center", cellLoc="center")
    t.auto_set_font_size(False); t.set_fontsize(8.5); t.scale(1, 1.35)
    t.auto_set_column_width(list(range(len(header))))
    for j in range(len(header)):
        t[0, j].set_facecolor("#dfe7f3"); t[0, j].set_text_props(fontweight="bold")
    plt.savefig(os.path.join(OUT, fname), dpi=170, bbox_inches="tight"); plt.close()

data = {n: load(EXP, n) for n in RUNS}

# 1. A28 per-run table
body = [[NICE[n]] + [f"{x:.3f}" for x in run_stats(data[n])[:4]] + [f"{run_stats(data[n])[4]:.0f}"] for n in RUNS]
render("Per-run results (A19/A20/A22/A28)",
       ["run", "wait p50 (ms)", "wait p99 (ms)", "resp p50 (ms)", "resp p99 (ms)", "throughput (req/s)"],
       body, "table_runs.png")

# 2. A21 combined slowdown table
body = []
for fn in FILES:
    row = [fn]
    for n in REF:
        v = slow(data[n], fn); row.append(f"{pct(v,50):.1f} / {pct(v,99):.1f}")
    body.append(row)
render("Normalised slowdown, ns/byte — median / p99, reference config (A21)",
       ["file"] + [NICE[n].split()[0] for n in REF], body, "table_slowdown.png")

# 3. GET/PUT split
body = []
for fn in FILES:
    for op in ("GET", "PUT"):
        row = [fn, op]
        for n in REF:
            v = slow(data[n], fn, op); row.append(f"{pct(v,50):.1f} / {pct(v,99):.1f}")
        body.append(row)
render("Normalised slowdown by operation, ns/byte — median / p99, reference config (A21, supplementary)",
       ["file", "op"] + [NICE[n].split()[0] for n in REF], body, "table_slowdown_split.png")

# 4. A29 rr vs drr
body = []
for fn in FILES:
    fr = sum(r["forfeited_bytes"] for r in data["rr_ref"] if r["filename"] == fn and r["op"] == "GET")
    fd = sum(r["forfeited_bytes"] for r in data["drr_ref"] if r["filename"] == fn and r["op"] == "GET")
    sr, sd = slow(data["rr_ref"], fn), slow(data["drr_ref"], fn)
    gr = [r["rounds"] for r in data["rr_ref"] if r["filename"] == fn and r["op"] == "GET"]
    gd = [r["rounds"] for r in data["drr_ref"] if r["filename"] == fn and r["op"] == "GET"]
    body.append([fn, f"{fr:,}", f"{fd:,}", f"{sum(gr)/len(gr):.1f} / {sum(gd)/len(gd):.1f}",
                 f"{pct(sr,50):.1f} / {pct(sr,99):.1f}", f"{pct(sd,50):.1f} / {pct(sd,99):.1f}"])
body.append(["A14 fired (whole run)", f"{a14(EXP,'rr_ref'):,}", f"{a14(EXP,'drr_ref'):,}", "", "", ""])
render("RR vs DRR at the reference config (A29), Q = 4096 B",
       ["file", "RR forfeited (B)", "DRR forfeited (B)", "mean GET rounds RR / DRR",
        "RR slowdown med / p99", "DRR slowdown med / p99"], body, "table_a29.png")

# 5. close pairs: run vs rerun (A28)
if os.path.isdir(RER):
    body = []
    for n in RUNS:
        a, b = run_stats(data[n]), run_stats(load(RER, n))
        body.append([NICE[n], f"{a[4]:.0f} / {b[4]:.0f}", f"{a[1]:.3f} / {b[1]:.3f}", f"{a[3]:.2f} / {b[3]:.2f}"])
    render("Run / rerun values (A28: close pairs reported with both values)",
           ["run", "throughput req/s (run / rerun)", "wait p99 ms (run / rerun)", "resp p99 ms (run / rerun)"],
           body, "table_rerun.png")

# 6. p99 slowdown plot
plt.figure(figsize=(10, 5.5)); w4 = 0.2
for j, n in enumerate(REF):
    plt.bar([i + (j - 1.5) * w4 for i in range(4)], [pct(slow(data[n], f), 99) for f in FILES], w4, label=NICE[n].split()[0])
plt.yscale("log"); plt.xticks(range(4), FILES, rotation=15); plt.ylabel("p99 normalised slowdown (ns/byte)")
plt.title("Normalised slowdown (p99) by file and policy — A21\n(reference config)"); plt.legend()
plt.tight_layout(); plt.savefig(os.path.join(OUT, "plot_slowdown_p99.png"), dpi=150); plt.close()
print("wrote table_runs.png table_slowdown.png table_slowdown_split.png table_a29.png",
      "table_rerun.png" if os.path.isdir(RER) else "(no rerun dir)", "plot_slowdown_p99.png", "->", os.path.abspath(OUT))
