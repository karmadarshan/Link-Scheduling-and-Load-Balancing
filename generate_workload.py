#!/usr/bin/env python3
"""
generate_workload.py — regenerates the Part A / A27 workload directory.

A27 requires a workload that spans two axes at once:
  - size:        small (~1KB), medium (~30KB), large (~150KB)
  - line length: ordinary short lines (60-80 bytes), and at least one file
                 containing lines longer than the scheduling quantum Q

This produces exactly the four files used in the report:
  small.txt              ~1 KB,   ordinary lines only
  medium.txt             ~30 KB,  ordinary lines only
  large.txt              ~150 KB, ordinary lines only
  large_longlines.txt    ~150 KB, ordinary lines interspersed with lines
                          longer than --quantum, so rr's A14 escape hatch
                          and drr's multi-round deficit accumulation (A16)
                          both get exercised.

Usage:
    python3 generate_workload.py --outdir workload --quantum 4096

Re-running with the same --seed produces byte-identical files, so the
workload is fully reproducible rather than just handed over as static
data.
"""

import argparse
import random
import string


def random_line(rng, min_len, max_len):
    """One line of printable characters, min_len..max_len bytes INCLUDING
    the trailing newline (matches how the assignment counts line length,
    A7: the terminating \\n counts toward every byte total)."""
    body_len = rng.randint(min_len, max_len) - 1  # leave room for '\n'
    body_len = max(body_len, 1)
    body = "".join(rng.choice(string.ascii_letters + " ") for _ in range(body_len))
    return body + "\n"


def make_ordinary_file(path, target_bytes, rng, min_len=60, max_len=80):
    written = 0
    with open(path, "w") as f:
        while written < target_bytes:
            line = random_line(rng, min_len, max_len)
            f.write(line)
            written += len(line)
    return written


def make_longlines_file(path, target_bytes, quantum, rng,
                         min_len=60, max_len=80, long_every=25):
    """Same overall size/ordinary-line mix as the plain 'large' file, but
    every `long_every` lines, insert a line whose length is somewhere
    between quantum+500 and quantum*1.5 bytes — comfortably over Q so
    rr must invoke A14, and drr must accumulate deficit over multiple
    rounds (A16) before it can send the line."""
    written = 0
    line_no = 0
    long_min = quantum + 500
    long_max = int(quantum * 1.5)
    with open(path, "w") as f:
        while written < target_bytes:
            line_no += 1
            if line_no % long_every == 0:
                line = random_line(rng, long_min, long_max)
            else:
                line = random_line(rng, min_len, max_len)
            f.write(line)
            written += len(line)
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--outdir", default="workload", help="output directory")
    ap.add_argument("--quantum", type=int, default=4096,
                     help="scheduling quantum Q used in the rr/drr experiments; "
                          "must match the --quantum passed to ./server (A27: keep "
                          "Q in the 2-16KB range)")
    ap.add_argument("--seed", type=int, default=42,
                     help="RNG seed, for reproducible byte-identical output")
    args = ap.parse_args()

    import os
    os.makedirs(args.outdir, exist_ok=True)

    rng = random.Random(args.seed)
    sizes = {
        "small.txt": 1 * 1024,
        "medium.txt": 30 * 1024,
        "large.txt": 150 * 1024,
    }

    for name, size in sizes.items():
        path = os.path.join(args.outdir, name)
        written = make_ordinary_file(path, size, rng)
        print(f"wrote {path}: {written} bytes")

    ll_path = os.path.join(args.outdir, "large_longlines.txt")
    written = make_longlines_file(ll_path, 150 * 1024, args.quantum, rng)
    print(f"wrote {ll_path}: {written} bytes (quantum={args.quantum})")


if __name__ == "__main__":
    main()
