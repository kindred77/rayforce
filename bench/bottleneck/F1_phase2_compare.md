# Rayforce vs baseline — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (baseline_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.216 | 0.539 | 1.16 |
| 3 | scalar agg | 2.331 | 1.064 | 1.11 |
| 4 | scalar agg | 0.193 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.566 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 15.912 | 12.660 | 1.14 |
| 7 | scalar agg | 1.426 | 0.646 | 1.07 |
| 8 | group-by single key | 0.336 | 0.900 | 0.95 |
| 9 | group-by + count distinct | 31.963 | 15.617 | 1.64 |
| 10 | group-by + count distinct | 39.885 | 28.874 | 1.28 |
| 11 | group-by + count distinct | 30.743 | 5.710 | 2.59 |
| 12 | group-by + count distinct | 0.019 | 5.030 | 0.67 |
| 13 | group-by single key | 27.076 | 16.164 | 1.42 |
| 14 | group-by + count distinct | 50.069 | 25.159 | 1.71 |
| 15 | group-by + count distinct | 26.408 | 15.457 | 1.43 |
| 16 | group-by single key | 15.966 | 13.028 | 1.13 |
| 17 | group-by single key | 19.377 | 30.286 | 0.73 |
| 18 | group-by single key | 17.406 | 27.336 | 0.73 |
| 19 | group-by composite computed key | 35.025 | 43.922 | 0.84 |
| 20 | point lookup | 0.681 | 0.465 | 1.02 |
| 21 | LIKE filter | 16.953 | 14.417 | 1.10 |
| 22 | LIKE filter | 37.244 | 16.738 | 1.77 |
| 23 | LIKE filter | 57.170 | 25.398 | 1.90 |
| 24 | ORDER BY + LIMIT | 18.032 | 23.860 | 0.83 |
| 25 | ORDER BY + LIMIT | 24.241 | 3.385 | 2.56 |
| 26 | ORDER BY + LIMIT | 45.613 | 2.812 | 4.34 |
| 27 | ORDER BY + LIMIT | 28.348 | 3.068 | 2.93 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 29.006 | 13.043 | 1.69 |
| 32 | group-by + filter | 29.402 | 15.277 | 1.56 |
| 33 | group-by single key | 39.004 | 49.180 | 0.83 |
| 34 | group-by single key | 18.099 | 54.622 | 0.43 |
| 35 | group-by composite computed key | 18.993 | 58.670 | 0.42 |
| 36 | filtered group with date range | 25.366 | 16.230 | 1.35 |
| 37 | filtered group with date range | 24.062 | 8.963 | 1.80 |
| 38 | filtered group + OFFSET | 23.987 | 3.415 | 2.53 |
| 39 | filtered group + OFFSET | 5.831 | 5.030 | 1.05 |
| 40 | filtered group + OFFSET | 16.782 | 20.004 | 0.89 |
| 41 | filtered group + OFFSET | 6.783 | 2.439 | 1.35 |
| 42 | filtered group + OFFSET | 6.721 | 3.067 | 1.28 |
| 43 | minute-bucket time series | 16.112 | 3.465 | 1.94 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| ORDER BY + LIMIT | 4 | 2.28 | 0.83 | 4.34 |
| minute-bucket time series | 1 | 1.94 | 1.94 | 1.94 |
| group-by + filter | 2 | 1.62 | 1.56 | 1.69 |
| filtered group with date range | 2 | 1.56 | 1.35 | 1.80 |
| LIKE filter | 3 | 1.55 | 1.10 | 1.90 |
| group-by + count distinct | 6 | 1.44 | 0.67 | 2.59 |
| filtered group + OFFSET | 5 | 1.33 | 0.89 | 2.53 |
| scalar agg | 5 | 1.03 | 0.89 | 1.16 |
| point lookup | 1 | 1.02 | 1.02 | 1.02 |
| count distinct (whole table) | 2 | 0.88 | 0.67 | 1.14 |
| group-by single key | 7 | 0.84 | 0.43 | 1.42 |
| group-by composite computed key | 2 | 0.59 | 0.42 | 0.84 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
