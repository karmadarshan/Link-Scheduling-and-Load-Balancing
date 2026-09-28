## Per-run results (A28)
| run | N | wait p50 ms | wait p99 ms | resp p50 ms | resp p99 ms | throughput req/s |
|---|---|---|---|---|---|---|
| fcfs_ref | 1500 | 0.032 | 2.584 | 0.503 | 6.170 | 2798.45 |
| sjf_ref | 1500 | 0.031 | 1.702 | 0.458 | 5.264 | 3204.78 |
| rr_ref | 1500 | 0.016 | 0.095 | 0.592 | 6.655 | 2515.06 |
| drr_ref | 1500 | 0.016 | 0.149 | 0.540 | 7.160 | 2580.88 |
| fcfs_1thread | 1500 | 2.015 | 14.104 | 2.955 | 15.783 | 1042.76 |
| rr_1thread | 1500 | 0.130 | 0.835 | 2.115 | 23.485 | 791.75 |

## Normalised slowdown, ns/byte, reference config (A21)
| file | policy | n | median | p99 |
|---|---|---|---|---|
| small.txt | fcfs_ref | 396 | 184.05 | 2940.93 |
| medium.txt | fcfs_ref | 360 | 24.18 | 121.46 |
| large.txt | fcfs_ref | 397 | 19.77 | 53.79 |
| large_longlines.txt | fcfs_ref | 347 | 10.64 | 32.56 |
| small.txt | sjf_ref | 382 | 171.93 | 1928.70 |
| medium.txt | sjf_ref | 368 | 18.58 | 73.25 |
| large.txt | sjf_ref | 369 | 4.65 | 43.37 |
| large_longlines.txt | sjf_ref | 381 | 10.01 | 26.34 |
| small.txt | rr_ref | 397 | 121.12 | 755.75 |
| medium.txt | rr_ref | 348 | 23.53 | 56.48 |
| large.txt | rr_ref | 364 | 24.68 | 45.50 |
| large_longlines.txt | rr_ref | 391 | 16.39 | 39.80 |
| small.txt | drr_ref | 404 | 120.21 | 584.14 |
| medium.txt | drr_ref | 376 | 20.93 | 72.11 |
| large.txt | drr_ref | 347 | 7.68 | 57.17 |
| large_longlines.txt | drr_ref | 373 | 13.65 | 47.24 |

## Forfeited bytes (GET) and A14 (A29)
| file | RR | DRR |
|---|---|---|
| small.txt | 0 | 0 |
| medium.txt | 51,504 | 0 |
| large.txt | 255,618 | 0 |
| large_longlines.txt | 10,796,352 | 0 |
A14 fired: RR ref=4466  RR 1thr=4642  DRR ref=0
PUT forfeited total (must be 0): 0
Rounds per file (mean), rr_ref / drr_ref:
  small.txt: 1.00 / 1.00
  medium.txt: 8.00 / 8.00
  large.txt: 38.00 / 38.00
  large_longlines.txt: 44.00 / 38.00

Throughput spread among reference runs: 27.4% (rerun close pairs per A28)

plots written to /home/abhiram/partA/plots_rerun
