# Rayforce vs baseline — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (baseline_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.204 | 0.539 | 1.16 |
| 3 | scalar agg | 2.219 | 1.064 | 1.10 |
| 4 | scalar agg | 0.198 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.560 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 15.822 | 12.660 | 1.14 |
| 7 | scalar agg | 2.951 | 0.646 | 1.22 |
| 8 | group-by single key | 0.697 | 0.900 | 0.98 |
| 9 | group-by + count distinct | 33.317 | 15.617 | 1.69 |
| 10 | group-by + count distinct | 42.112 | 28.874 | 1.34 |
| 11 | group-by + count distinct | 32.301 | 5.710 | 2.69 |
| 12 | group-by + count distinct | 0.016 | 5.030 | 0.67 |
| 13 | group-by single key | 27.183 | 16.164 | 1.42 |
| 14 | group-by + count distinct | 52.015 | 25.159 | 1.76 |
| 15 | group-by + count distinct | 27.165 | 15.457 | 1.46 |
| 16 | group-by single key | 16.230 | 13.028 | 1.14 |
| 17 | group-by single key | 18.908 | 30.286 | 0.72 |
| 18 | group-by single key | 16.967 | 27.336 | 0.72 |
| 19 | group-by composite computed key | 37.040 | 43.922 | 0.87 |
| 20 | point lookup | 0.878 | 0.465 | 1.04 |
| 21 | LIKE filter | 16.139 | 14.417 | 1.07 |
| 22 | LIKE filter | 37.904 | 16.738 | 1.79 |
| 23 | LIKE filter | 58.672 | 25.398 | 1.94 |
| 24 | ORDER BY + LIMIT | 18.234 | 23.860 | 0.83 |
| 25 | ORDER BY + LIMIT | 25.175 | 3.385 | 2.63 |
| 26 | ORDER BY + LIMIT | 46.463 | 2.812 | 4.41 |
| 27 | ORDER BY + LIMIT | 29.765 | 3.068 | 3.04 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 29.168 | 13.043 | 1.70 |
| 32 | group-by + filter | 29.272 | 15.277 | 1.55 |
| 33 | group-by single key | 40.087 | 49.180 | 0.85 |
| 34 | group-by single key | 18.665 | 54.622 | 0.44 |
| 35 | group-by composite computed key | 19.889 | 58.670 | 0.44 |
| 36 | filtered group with date range | 26.464 | 16.230 | 1.39 |
| 37 | filtered group with date range | 11.193 | 8.963 | 1.12 |
| 38 | filtered group + OFFSET | 3.419 | 3.415 | 1.00 |
| 39 | filtered group + OFFSET | 2.954 | 5.030 | 0.86 |
| 40 | filtered group + OFFSET | 15.857 | 20.004 | 0.86 |
| 41 | filtered group + OFFSET | 6.718 | 2.439 | 1.34 |
| 42 | filtered group + OFFSET | 6.796 | 3.067 | 1.29 |
| 43 | minute-bucket time series | 16.117 | 3.465 | 1.94 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| ORDER BY + LIMIT | 4 | 2.33 | 0.83 | 4.41 |
| minute-bucket time series | 1 | 1.94 | 1.94 | 1.94 |
| group-by + filter | 2 | 1.63 | 1.55 | 1.70 |
| LIKE filter | 3 | 1.55 | 1.07 | 1.94 |
| group-by + count distinct | 6 | 1.48 | 0.67 | 2.69 |
| filtered group with date range | 2 | 1.25 | 1.12 | 1.39 |
| scalar agg | 5 | 1.06 | 0.89 | 1.22 |
| filtered group + OFFSET | 5 | 1.05 | 0.86 | 1.34 |
| point lookup | 1 | 1.04 | 1.04 | 1.04 |
| count distinct (whole table) | 2 | 0.87 | 0.67 | 1.14 |
| group-by single key | 7 | 0.85 | 0.44 | 1.42 |
| group-by composite computed key | 2 | 0.62 | 0.44 | 0.87 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | Baseline ms | Ratio |
| --: | --- | --: | --: | --: |
