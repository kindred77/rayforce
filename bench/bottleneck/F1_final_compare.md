# Rayforce vs baseline — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (baseline_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.271 | 0.539 | 1.16 |
| 3 | scalar agg | 2.116 | 1.064 | 1.10 |
| 4 | scalar agg | 0.196 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.549 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 15.849 | 12.660 | 1.14 |
| 7 | scalar agg | 1.534 | 0.646 | 1.08 |
| 8 | group-by single key | 0.426 | 0.900 | 0.96 |
| 9 | group-by + count distinct | 32.940 | 15.617 | 1.68 |
| 10 | group-by + count distinct | 40.705 | 28.874 | 1.30 |
| 11 | group-by + count distinct | 29.810 | 5.710 | 2.53 |
| 12 | group-by + count distinct | 0.020 | 5.030 | 0.67 |
| 13 | group-by single key | 27.227 | 16.164 | 1.42 |
| 14 | group-by + count distinct | 44.452 | 25.159 | 1.55 |
| 15 | group-by + count distinct | 25.765 | 15.457 | 1.40 |
| 16 | group-by single key | 15.885 | 13.028 | 1.12 |
| 17 | group-by single key | 20.592 | 30.286 | 0.76 |
| 18 | group-by single key | 17.657 | 27.336 | 0.74 |
| 19 | group-by composite computed key | 36.063 | 43.922 | 0.85 |
| 20 | point lookup | 0.805 | 0.465 | 1.03 |
| 21 | LIKE filter | 16.424 | 14.417 | 1.08 |
| 22 | LIKE filter | 37.756 | 16.738 | 1.79 |
| 23 | LIKE filter | 57.645 | 25.398 | 1.91 |
| 24 | ORDER BY + LIMIT | 18.015 | 23.860 | 0.83 |
| 25 | ORDER BY + LIMIT | 0.557 | 3.385 | 0.79 |
| 26 | ORDER BY + LIMIT | 2.076 | 2.812 | 0.94 |
| 27 | ORDER BY + LIMIT | 0.704 | 3.068 | 0.82 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 28.265 | 13.043 | 1.66 |
| 32 | group-by + filter | 31.391 | 15.277 | 1.64 |
| 33 | group-by single key | 39.665 | 49.180 | 0.84 |
| 34 | group-by single key | 17.680 | 54.622 | 0.43 |
| 35 | group-by composite computed key | 19.961 | 58.670 | 0.44 |
| 36 | filtered group with date range | 25.414 | 16.230 | 1.35 |
| 37 | filtered group with date range | 10.843 | 8.963 | 1.10 |
| 38 | filtered group + OFFSET | 3.378 | 3.415 | 1.00 |
| 39 | filtered group + OFFSET | 2.893 | 5.030 | 0.86 |
| 40 | filtered group + OFFSET | 16.131 | 20.004 | 0.87 |
| 41 | filtered group + OFFSET | 6.679 | 2.439 | 1.34 |
| 42 | filtered group + OFFSET | 7.197 | 3.067 | 1.32 |
| 43 | minute-bucket time series | 12.542 | 3.465 | 1.67 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| minute-bucket time series | 1 | 1.67 | 1.67 | 1.67 |
| group-by + filter | 2 | 1.65 | 1.64 | 1.66 |
| LIKE filter | 3 | 1.55 | 1.08 | 1.91 |
| group-by + count distinct | 6 | 1.42 | 0.67 | 2.53 |
| filtered group with date range | 2 | 1.22 | 1.10 | 1.35 |
| filtered group + OFFSET | 5 | 1.06 | 0.86 | 1.34 |
| point lookup | 1 | 1.03 | 1.03 | 1.03 |
| scalar agg | 5 | 1.03 | 0.89 | 1.16 |
| count distinct (whole table) | 2 | 0.87 | 0.67 | 1.14 |
| group-by single key | 7 | 0.85 | 0.43 | 1.42 |
| ORDER BY + LIMIT | 4 | 0.84 | 0.79 | 0.94 |
| group-by composite computed key | 2 | 0.61 | 0.44 | 0.85 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
