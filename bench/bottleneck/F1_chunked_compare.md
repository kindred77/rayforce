# Rayforce vs DuckDB — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (duckdb_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.116 | 0.539 | 1.15 |
| 3 | scalar agg | 2.111 | 1.064 | 1.09 |
| 4 | scalar agg | 0.196 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.572 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 15.714 | 12.660 | 1.13 |
| 7 | scalar agg | 1.477 | 0.646 | 1.08 |
| 8 | group-by single key | 0.446 | 0.900 | 0.96 |
| 9 | group-by + count distinct | 32.080 | 15.617 | 1.64 |
| 10 | group-by + count distinct | 39.343 | 28.874 | 1.27 |
| 11 | group-by + count distinct | 32.039 | 5.710 | 2.68 |
| 12 | group-by + count distinct | 0.018 | 5.030 | 0.67 |
| 13 | group-by single key | 27.167 | 16.164 | 1.42 |
| 14 | group-by + count distinct | 56.848 | 25.159 | 1.90 |
| 15 | group-by + count distinct | 29.806 | 15.457 | 1.56 |
| 16 | group-by single key | 17.700 | 13.028 | 1.20 |
| 17 | group-by single key | 19.903 | 30.286 | 0.74 |
| 18 | group-by single key | 18.349 | 27.336 | 0.76 |
| 19 | group-by composite computed key | 36.320 | 43.922 | 0.86 |
| 20 | point lookup | 0.849 | 0.465 | 1.04 |
| 21 | LIKE filter | 16.634 | 14.417 | 1.09 |
| 22 | LIKE filter | 38.359 | 16.738 | 1.81 |
| 23 | LIKE filter | 59.426 | 25.398 | 1.96 |
| 24 | ORDER BY + LIMIT | 17.774 | 23.860 | 0.82 |
| 25 | ORDER BY + LIMIT | 25.520 | 3.385 | 2.65 |
| 26 | ORDER BY + LIMIT | 49.239 | 2.812 | 4.62 |
| 27 | ORDER BY + LIMIT | 31.091 | 3.068 | 3.14 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 29.245 | 13.043 | 1.70 |
| 32 | group-by + filter | 30.431 | 15.277 | 1.60 |
| 33 | group-by single key | 38.420 | 49.180 | 0.82 |
| 34 | group-by single key | 18.306 | 54.622 | 0.44 |
| 35 | group-by composite computed key | 19.033 | 58.670 | 0.42 |
| 36 | filtered group with date range | 24.579 | 16.230 | 1.32 |
| 37 | filtered group with date range | 11.055 | 8.963 | 1.11 |
| 38 | filtered group + OFFSET | 3.960 | 3.415 | 1.04 |
| 39 | filtered group + OFFSET | 2.884 | 5.030 | 0.86 |
| 40 | filtered group + OFFSET | 15.775 | 20.004 | 0.86 |
| 41 | filtered group + OFFSET | 6.539 | 2.439 | 1.33 |
| 42 | filtered group + OFFSET | 6.739 | 3.067 | 1.28 |
| 43 | minute-bucket time series | 12.063 | 3.465 | 1.64 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| ORDER BY + LIMIT | 4 | 2.37 | 0.82 | 4.62 |
| group-by + filter | 2 | 1.65 | 1.60 | 1.70 |
| minute-bucket time series | 1 | 1.64 | 1.64 | 1.64 |
| LIKE filter | 3 | 1.57 | 1.09 | 1.96 |
| group-by + count distinct | 6 | 1.49 | 0.67 | 2.68 |
| filtered group with date range | 2 | 1.21 | 1.11 | 1.32 |
| filtered group + OFFSET | 5 | 1.05 | 0.86 | 1.33 |
| point lookup | 1 | 1.04 | 1.04 | 1.04 |
| scalar agg | 5 | 1.03 | 0.89 | 1.15 |
| count distinct (whole table) | 2 | 0.87 | 0.67 | 1.13 |
| group-by single key | 7 | 0.85 | 0.44 | 1.42 |
| group-by composite computed key | 2 | 0.60 | 0.42 | 0.86 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
