# sqlite-columnar Benchmark

Date: 2026-05-10

Single-run command:

```sh
make
make benchmarks
build/columnar-analytics-bench ./columnar 10000000 256
```

For variance-aware runs, use the repeatable benchmark suite:

```sh
make variance-bench VARIANCE_REPEATS=9 \
  VARIANCE_DATASETS="small:10000:64 medium:50000:128 wide:50000:512"
```

The variance suite emits machine-readable lines:

- `dataset,...` records load/analyze time, storage size, and zone-map coverage.
- `query,...` records row-store and columnar median/p95 timings, median/p95
  speedups, result row count, and normalized result hash.

Each query is warmed once before sampling and every sample verifies that the
columnar result hash matches the row-store result hash.

## Variance Benchmark Results

Command:

```sh
make variance-bench VARIANCE_REPEATS=3 \
  VARIANCE_DATASETS='large100k:100000:128 large1m:1000000:128 large10m:10000000:256'
```

With three repeats, `p95` is effectively the slowest sampled run. Use more
repeats for publishable tail-latency claims.

Load and analyze:

| Dataset | Row populate ms | Columnar populate ms | Analyze ms | Row bytes | Columnar bytes | Selected chunks |
|---|---:|---:|---:|---:|---:|---:|
| `large100k` | 112.364 | 482.392 | 479.474 | 31,596,544 | 42,725,376 | 1 / 2 |
| `large1m` | 908.474 | 4,514.633 | 5,287.540 | 315,879,424 | 428,773,376 | 1 / 16 |
| `large10m` | 23,528.265 | 65,145.171 | 67,766.382 | 5,867,487,232 | 7,038,214,144 | 3 / 153 |

10M query medians:

| Query | Row median ms | Columnar median ms | Median speedup | Row p95 ms | Columnar p95 ms | p95 speedup |
|---|---:|---:|---:|---:|---:|---:|
| `columnar_sum(v1)` | 4,176.359 | 0.032 | 130,582.95x | 4,492.231 | 0.139 | 32,337.75x |
| `columnar_avg(v3)` | 4,135.865 | 0.032 | 129,316.82x | 4,188.948 | 0.045 | 92,745.57x |
| `columnar_count(v1)` | 4,003.996 | 0.032 | 125,193.65x | 4,028.505 | 0.038 | 106,456.49x |
| Generic columnar `GROUP BY id1, sum(v1)` | 8,701.082 | 5,773.005 | 1.51x | 9,863.331 | 6,944.527 | 1.42x |
| `columnar_group_sum(id1, v1)` | 7,985.574 | 1,322.187 | 6.04x | 8,156.254 | 1,322.539 | 6.17x |
| `columnar_group_count(id1)` | 7,921.773 | 560.804 | 14.13x | 7,935.962 | 561.124 | 14.14x |
| `columnar_group_sum_avg_count(id1, v1)` | 8,476.238 | 1,320.321 | 6.42x | 8,592.908 | 1,339.498 | 6.42x |
| `columnar_group_min_max_count(id3, v1)` | 6,078.397 | 1,410.172 | 4.31x | 6,141.314 | 1,416.834 | 4.33x |
| Specialized clustered `WHERE ts BETWEEN 100000 AND 200000` | 4,302.623 | 17.287 | 248.89x | 4,892.107 | 17.294 | 282.88x |
| Specialized clustered `GROUP BY id1, sum/avg/count(v1)` | 3,962.001 | 14.502 | 273.20x | 4,122.624 | 14.648 | 281.45x |

Median speedup by dataset size:

| Query | 100k | 1M | 10M |
|---|---:|---:|---:|
| `columnar_sum(v1)` | 449.32x | 4,243.82x | 130,582.95x |
| `columnar_avg(v3)` | 536.95x | 4,484.11x | 129,316.82x |
| `columnar_count(v1)` | 480.88x | 4,060.56x | 125,193.65x |
| `columnar_group_sum(id1, v1)` | 2.13x | 2.89x | 6.04x |
| `columnar_group_count(id1)` | 4.15x | 6.74x | 14.13x |
| Specialized clustered range filter | 1.45x | 15.45x | 248.89x |
| Specialized clustered grouped `sum/avg/count` | 2.96x | 37.23x | 273.20x |

## Single-Run Analytical Benchmark

Dataset:

- Rows: 10,000,000
- Wide fact table with dimensions `ts`, `id1`..`id6`, measures `v1`..`v3`,
  and cold unused payload columns.
- Cold text payload: 2 columns x 256 bytes.
- `ts` is clustered/monotonic, which lets chunk zone maps prune rowid ranges.
- Chunk size: 65,536 rowids.

Load and analyze:

| Metric | Value |
|---|---:|
| Row-store populate | 17,523.098 ms |
| Columnar populate | 62,640.616 ms |
| Initial `columnar_analyze()` with global stats + chunk zone maps | 66,763.592 ms |
| Incremental `columnar_analyze()` no-op | 2.114 ms |
| Incremental `columnar_analyze()` after one inserted row | 82.998 ms |
| Row-store bytes | 5,867,487,232 |
| Columnar bytes | 7,038,214,144 |
| Metadata after initial analyze | row_count=10,000,000, chunk_count=153, dirty_count=0, stats_valid=1 |
| `ts` zone-map chunks selected | 3 / 153 |
| `ts` full-cover chunks eligible for aggregate pushdown | 1 / 153 |
| Dirty entries after one inserted row | 14 |
| Metadata dirty count after one inserted row | 14 |
| Dirty entries after incremental analyze | 0 |
| Metadata after incremental analyze | row_count=10,000,001, chunk_count=153, dirty_count=0, stats_valid=1 |

Query results:

| Query | Row-store ms | Columnar ms | Speedup |
|---|---:|---:|---:|
| `sum(v1)` via `columnar_sum` | 4,014.135 | 0.049 | 81,800.48x |
| `avg(v3)` via `columnar_avg` | 3,771.010 | 0.032 | 117,908.83x |
| `count(v1)` via `columnar_count` | 3,621.597 | 0.031 | 116,803.63x |
| Generic columnar `GROUP BY id1, sum(v1)` | 8,243.531 | 7,116.800 | 1.16x |
| `columnar_group_sum(id1, v1)` | 7,927.167 | 1,325.468 | 5.98x |
| `columnar_group_avg(id1, v3)` | 7,837.215 | 1,456.862 | 5.38x |
| `columnar_group_count(id1)` | 7,483.165 | 575.004 | 13.01x |
| `columnar_group_sum_avg_count(id1, v1)` | 7,802.118 | 1,383.348 | 5.64x |
| Generic `GROUP BY id3, sum(v1), avg(v3)` | 5,921.353 | 5,385.596 | 1.10x |
| Generic `GROUP BY id3, max(v1)-min(v2)` | 5,744.765 | 6,754.225 | 0.85x |
| `columnar_group_min_max_count(id3, v1)` | 6,171.251 | 1,754.662 | 3.52x |
| `columnar_group_range(id3, v1, v2)` | 6,119.328 | 3,485.148 | 1.76x |
| Generic `WHERE id2 BETWEEN 10 AND 20` | 4,428.708 | 3,461.585 | 1.28x |
| Specialized `WHERE id2 BETWEEN 10 AND 20` | 3,982.078 | 1,477.895 | 2.69x |
| Generic clustered `WHERE ts BETWEEN 100000 AND 200000` | 3,931.485 | 3,133.416 | 1.25x |
| Specialized clustered `WHERE ts BETWEEN 100000 AND 200000` | 3,819.239 | 24.971 | 152.95x |
| Specialized clustered `GROUP BY id1, sum(v1)` | 3,932.212 | 18.469 | 212.91x |
| Specialized clustered `GROUP BY id1, sum/avg/count(v1)` | 3,691.288 | 14.654 | 251.90x |

Interpretation:

- Global `sum`/`avg`/`count` are accelerated by precomputed stats.
- Generic virtual-table scans benefit from reading fewer columns but still pay
  SQLite row materialization and generic aggregation costs.
- Specialized grouped functions avoid generic row materialization.
- Range-filtered functions use chunk zone maps. They help modestly on uniformly
  distributed filters such as `id2`, and dramatically on clustered filters such
  as `ts`, where only 3 of 153 chunks are scanned.
- Scalar range-filtered aggregates use precomputed chunk `sum`/`count` for
  full-cover chunks. In this run, 1 of the 3 selected `ts` chunks is served from
  chunk stats instead of row scans.
- `columnar_analyze()` is incremental after the initial bootstrap. Persistent
  metadata tracks `row_count`, `chunk_count`, `dirty_count`, and `stats_valid`.
  A no-op analyze on this 10M-row table now returns from metadata in 2.114 ms,
  and reanalyzing the dirty metadata after a single inserted row took 82.998 ms.
- The current weak spots remain load time, `columnar_analyze()` cost, and larger
  on-disk size due to one SQLite B-tree per column plus zone-map metadata. The
  expensive analyze cost now applies primarily to the first bootstrap or to
  large dirty ranges.
