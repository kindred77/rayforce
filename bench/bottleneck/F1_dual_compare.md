# Rayforce vs baseline — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (baseline_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.368 | 0.539 | 1.17 |
| 3 | scalar agg | 2.401 | 1.064 | 1.12 |
| 4 | scalar agg | 0.188 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.527 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 15.687 | 12.660 | 1.13 |
| 7 | scalar agg | 1.662 | 0.646 | 1.10 |
| 8 | group-by single key | 0.520 | 0.900 | 0.97 |
| 9 | group-by + count distinct | 32.481 | 15.617 | 1.66 |
| 10 | group-by + count distinct | 39.208 | 28.874 | 1.27 |
| 11 | group-by + count distinct | 31.454 | 5.710 | 2.64 |
| 12 | group-by + count distinct | 0.018 | 5.030 | 0.67 |
| 13 | group-by single key | 27.416 | 16.164 | 1.43 |
| 14 | group-by + count distinct | 50.540 | 25.159 | 1.72 |
| 15 | group-by + count distinct | 27.093 | 15.457 | 1.46 |
| 16 | group-by single key | 15.961 | 13.028 | 1.13 |
| 17 | group-by single key | 18.586 | 30.286 | 0.71 |
| 18 | group-by single key | 17.234 | 27.336 | 0.73 |
| 19 | group-by composite computed key | 35.371 | 43.922 | 0.84 |
| 20 | point lookup | 0.640 | 0.465 | 1.02 |
| 21 | LIKE filter | 16.103 | 14.417 | 1.07 |
| 22 | LIKE filter | 37.217 | 16.738 | 1.77 |
| 23 | LIKE filter | 57.918 | 25.398 | 1.92 |
| 24 | ORDER BY + LIMIT | 17.693 | 23.860 | 0.82 |
| 25 | ORDER BY + LIMIT | 24.978 | 3.385 | 2.61 |
| 26 | ORDER BY + LIMIT | 46.072 | 2.812 | 4.38 |
| 27 | ORDER BY + LIMIT | 29.152 | 3.068 | 3.00 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 28.722 | 13.043 | 1.68 |
| 32 | group-by + filter | 30.181 | 15.277 | 1.59 |
| 33 | group-by single key | 39.548 | 49.180 | 0.84 |
| 34 | group-by single key | 17.960 | 54.622 | 0.43 |
| 35 | group-by composite computed key | 19.088 | 58.670 | 0.42 |
| 36 | filtered group with date range | 25.309 | 16.230 | 1.35 |
| 37 | filtered group with date range | 11.500 | 8.963 | 1.13 |
| 38 | filtered group + OFFSET | 3.490 | 3.415 | 1.01 |
| 39 | filtered group + OFFSET | 2.919 | 5.030 | 0.86 |
| 40 | filtered group + OFFSET | 16.033 | 20.004 | 0.87 |
| 41 | filtered group + OFFSET | 6.651 | 2.439 | 1.34 |
| 42 | filtered group + OFFSET | 6.845 | 3.067 | 1.29 |
| 43 | minute-bucket time series | 12.127 | 3.465 | 1.64 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| ORDER BY + LIMIT | 4 | 2.30 | 0.82 | 4.38 |
| minute-bucket time series | 1 | 1.64 | 1.64 | 1.64 |
| group-by + filter | 2 | 1.63 | 1.59 | 1.68 |
| LIKE filter | 3 | 1.54 | 1.07 | 1.92 |
| group-by + count distinct | 6 | 1.45 | 0.67 | 2.64 |
| filtered group with date range | 2 | 1.24 | 1.13 | 1.35 |
| filtered group + OFFSET | 5 | 1.05 | 0.86 | 1.34 |
| scalar agg | 5 | 1.04 | 0.89 | 1.17 |
| point lookup | 1 | 1.02 | 1.02 | 1.02 |
| count distinct (whole table) | 2 | 0.87 | 0.67 | 1.13 |
| group-by single key | 7 | 0.84 | 0.43 | 1.43 |
| group-by composite computed key | 2 | 0.60 | 0.42 | 0.84 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
