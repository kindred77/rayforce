# Rayforce vs baseline — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (baseline_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.364 | 0.539 | 1.17 |
| 3 | scalar agg | 2.597 | 1.064 | 1.14 |
| 4 | scalar agg | 0.195 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.806 | 11.721 | 0.68 |
| 6 | count distinct (whole table) | 15.643 | 12.660 | 1.13 |
| 7 | scalar agg | 1.632 | 0.646 | 1.09 |
| 8 | group-by single key | 0.368 | 0.900 | 0.95 |
| 9 | group-by + count distinct | 32.119 | 15.617 | 1.64 |
| 10 | group-by + count distinct | 39.504 | 28.874 | 1.27 |
| 11 | group-by + count distinct | 29.897 | 5.710 | 2.54 |
| 12 | group-by + count distinct | 0.020 | 5.030 | 0.67 |
| 13 | group-by single key | 26.452 | 16.164 | 1.39 |
| 14 | group-by + count distinct | 49.199 | 25.159 | 1.68 |
| 15 | group-by + count distinct | 25.357 | 15.457 | 1.39 |
| 16 | group-by single key | 16.799 | 13.028 | 1.16 |
| 17 | group-by single key | 19.537 | 30.286 | 0.73 |
| 18 | group-by single key | 17.303 | 27.336 | 0.73 |
| 19 | group-by composite computed key | 35.791 | 43.922 | 0.85 |
| 20 | point lookup | 0.829 | 0.465 | 1.03 |
| 21 | LIKE filter | 16.484 | 14.417 | 1.08 |
| 22 | LIKE filter | 40.671 | 16.738 | 1.90 |
| 23 | LIKE filter | 58.849 | 25.398 | 1.94 |
| 24 | ORDER BY + LIMIT | 17.765 | 23.860 | 0.82 |
| 25 | ORDER BY + LIMIT | 0.554 | 3.385 | 0.79 |
| 26 | ORDER BY + LIMIT | 1.993 | 2.812 | 0.94 |
| 27 | ORDER BY + LIMIT | 0.622 | 3.068 | 0.81 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 28.571 | 13.043 | 1.67 |
| 32 | group-by + filter | 29.690 | 15.277 | 1.57 |
| 33 | group-by single key | 39.682 | 49.180 | 0.84 |
| 34 | group-by single key | 18.207 | 54.622 | 0.44 |
| 35 | group-by composite computed key | 19.431 | 58.670 | 0.43 |
| 36 | filtered group with date range | 24.754 | 16.230 | 1.32 |
| 37 | filtered group with date range | 11.192 | 8.963 | 1.12 |
| 38 | filtered group + OFFSET | 3.454 | 3.415 | 1.00 |
| 39 | filtered group + OFFSET | 3.103 | 5.030 | 0.87 |
| 40 | filtered group + OFFSET | 20.302 | 20.004 | 1.01 |
| 41 | filtered group + OFFSET | 6.594 | 2.439 | 1.33 |
| 42 | filtered group + OFFSET | 6.790 | 3.067 | 1.28 |
| 43 | minute-bucket time series | 12.025 | 3.465 | 1.64 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| minute-bucket time series | 1 | 1.64 | 1.64 | 1.64 |
| group-by + filter | 2 | 1.62 | 1.57 | 1.67 |
| LIKE filter | 3 | 1.59 | 1.08 | 1.94 |
| group-by + count distinct | 6 | 1.42 | 0.67 | 2.54 |
| filtered group with date range | 2 | 1.22 | 1.12 | 1.32 |
| filtered group + OFFSET | 5 | 1.09 | 0.87 | 1.33 |
| scalar agg | 5 | 1.04 | 0.89 | 1.17 |
| point lookup | 1 | 1.03 | 1.03 | 1.03 |
| count distinct (whole table) | 2 | 0.88 | 0.68 | 1.13 |
| group-by single key | 7 | 0.84 | 0.44 | 1.39 |
| ORDER BY + LIMIT | 4 | 0.84 | 0.79 | 0.94 |
| group-by composite computed key | 2 | 0.60 | 0.43 | 0.85 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
