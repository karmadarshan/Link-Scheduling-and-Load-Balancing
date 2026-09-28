## Per-run results (A28)
| run | N | wait p50 ms | wait p99 ms | resp p50 ms | resp p99 ms | throughput req/s |
|---|---|---|---|---|---|---|
| fcfs_ref | 1500 | 0.034 | 2.252 | 0.537 | 5.256 | 3247.63 |
| sjf_ref | 1500 | 0.031 | 2.049 | 0.503 | 5.290 | 2958.30 |
| rr_ref | 1500 | 0.016 | 0.109 | 0.605 | 7.551 | 2348.39 |
| drr_ref | 1500 | 0.016 | 0.112 | 0.572 | 6.749 | 2413.49 |
| fcfs_1thread | 1500 | 2.085 | 13.571 | 3.164 | 15.533 | 1043.33 |
| rr_1thread | 1500 | 0.111 | 0.864 | 1.923 | 24.833 | 831.94 |

## Normalised slowdown, ns/byte, reference config (A21)
| file | policy | n | median | p99 |
|---|---|---|---|---|
| small.txt | fcfs_ref | 379 | 176.00 | 2412.53 |
| medium.txt | fcfs_ref | 378 | 24.62 | 101.17 |
| large.txt | fcfs_ref | 363 | 5.09 | 40.95 |
| large_longlines.txt | fcfs_ref | 380 | 11.29 | 24.90 |
| small.txt | sjf_ref | 379 | 166.16 | 1732.92 |
| medium.txt | sjf_ref | 386 | 22.66 | 90.89 |
| large.txt | sjf_ref | 408 | 18.14 | 39.49 |
| large_longlines.txt | sjf_ref | 327 | 8.26 | 29.64 |
| small.txt | rr_ref | 376 | 132.98 | 571.22 |
| medium.txt | rr_ref | 347 | 24.26 | 85.33 |
| large.txt | rr_ref | 396 | 24.37 | 52.14 |
| large_longlines.txt | rr_ref | 381 | 17.04 | 45.90 |
| small.txt | drr_ref | 358 | 122.37 | 469.94 |
| medium.txt | drr_ref | 373 | 18.13 | 60.66 |
| large.txt | drr_ref | 420 | 7.19 | 55.00 |
| large_longlines.txt | drr_ref | 349 | 11.33 | 36.90 |

## Forfeited bytes (GET) and A14 (A29)
| file | RR | DRR |
|---|---|---|
| small.txt | 0 | 0 |
| medium.txt | 52,096 | 0 |
| large.txt | 265,946 | 0 |
| large_longlines.txt | 10,370,880 | 0 |
A14 fired: RR ref=4290  RR 1thr=4026  DRR ref=0
PUT forfeited total (must be 0): 0
Rounds per file (mean), rr_ref / drr_ref:
  small.txt: 1.00 / 1.00
  medium.txt: 8.00 / 8.00
  large.txt: 38.00 / 38.00
  large_longlines.txt: 44.00 / 38.00

Throughput spread among reference runs: 38.3% (rerun close pairs per A28)

plots written to /home/abhiram/partA

## Normalised slowdown split by operation, ns/byte, reference config (supplementary to A21)
| file | op | fcfs med | sjf med | rr med | drr med | fcfs p99 | sjf p99 | rr p99 | drr p99 |
|---|---|---|---|---|---|---|---|---|---|
| small.txt | GET | 232.20 | 206.27 | 153.81 | 141.97 | 2602.49 | 1932.88 | 680.96 | 546.52 |
| small.txt | PUT | 124.11 | 121.58 | 112.97 | 100.56 | 2362.80 | 1328.33 | 547.66 | 334.25 |
| medium.txt | GET | 36.00 | 32.18 | 36.32 | 36.06 | 118.52 | 107.60 | 121.72 | 61.02 |
| medium.txt | PUT | 7.30 | 7.07 | 7.65 | 7.98 | 87.24 | 90.34 | 27.34 | 21.24 |
| large.txt | GET | 27.14 | 26.44 | 34.07 | 33.45 | 41.59 | 42.98 | 52.63 | 58.43 |
| large.txt | PUT | 2.39 | 2.28 | 3.67 | 3.63 | 10.02 | 9.05 | 8.94 | 6.82 |
| large_longlines.txt | GET | 14.04 | 13.54 | 24.53 | 23.56 | 25.73 | 31.15 | 50.96 | 57.61 |
| large_longlines.txt | PUT | 2.60 | 2.62 | 3.59 | 3.76 | 14.50 | 8.60 | 11.97 | 12.31 |
