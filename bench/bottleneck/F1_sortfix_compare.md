# Rayforce vs DuckDB — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (duckdb_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.246 | 0.539 | 1.16 |
| 3 | scalar agg | 2.389 | 1.064 | 1.12 |
| 4 | scalar agg | 0.223 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.663 | 11.721 | 0.68 |
| 6 | count distinct (whole table) | 15.737 | 12.660 | 1.14 |
| 7 | scalar agg | 3.246 | 0.646 | 1.24 |
| 8 | group-by single key | 0.808 | 0.900 | 0.99 |
| 9 | group-by + count distinct | 38.520 | 15.617 | 1.89 |
| 10 | group-by + count distinct | 42.816 | 28.874 | 1.36 |
| 11 | group-by + count distinct | 31.336 | 5.710 | 2.63 |
| 12 | group-by + count distinct | 0.016 | 5.030 | 0.67 |
| 13 | group-by single key | 26.958 | 16.164 | 1.41 |
| 14 | group-by + count distinct | 51.557 | 25.159 | 1.75 |
| 15 | group-by + count distinct | 26.918 | 15.457 | 1.45 |
| 16 | group-by single key | 16.142 | 13.028 | 1.14 |
| 17 | group-by single key | 18.889 | 30.286 | 0.72 |
| 18 | group-by single key | 17.724 | 27.336 | 0.74 |
| 19 | group-by composite computed key | 35.552 | 43.922 | 0.84 |
| 20 | point lookup | 0.803 | 0.465 | 1.03 |
| 21 | LIKE filter | 16.888 | 14.417 | 1.10 |
| 22 | LIKE filter | 38.296 | 16.738 | 1.81 |
| 23 | LIKE filter | 59.174 | 25.398 | 1.95 |
| 24 | ORDER BY + LIMIT | 17.890 | 23.860 | 0.82 |
| 25 | ORDER BY + LIMIT | 0.556 | 3.385 | 0.79 |
| 26 | ORDER BY + LIMIT | 2.031 | 2.812 | 0.94 |
| 27 | ORDER BY + LIMIT | 0.636 | 3.068 | 0.81 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 29.432 | 13.043 | 1.71 |
| 32 | group-by + filter | 31.062 | 15.277 | 1.62 |
| 33 | group-by single key | 42.258 | 49.180 | 0.88 |
| 34 | group-by single key | 19.723 | 54.622 | 0.46 |
| 35 | group-by composite computed key | 19.444 | 58.670 | 0.43 |
| 36 | filtered group with date range | 25.170 | 16.230 | 1.34 |
| 37 | filtered group with date range | 10.860 | 8.963 | 1.10 |
| 38 | filtered group + OFFSET | 3.350 | 3.415 | 1.00 |
| 39 | filtered group + OFFSET | 2.891 | 5.030 | 0.86 |
| 40 | filtered group + OFFSET | 22.082 | 20.004 | 1.07 |
| 41 | filtered group + OFFSET | 6.588 | 2.439 | 1.33 |
| 42 | filtered group + OFFSET | 6.768 | 3.067 | 1.28 |
| 43 | minute-bucket time series | 12.075 | 3.465 | 1.64 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| group-by + filter | 2 | 1.67 | 1.62 | 1.71 |
| minute-bucket time series | 1 | 1.64 | 1.64 | 1.64 |
| LIKE filter | 3 | 1.57 | 1.10 | 1.95 |
| group-by + count distinct | 6 | 1.50 | 0.67 | 2.63 |
| filtered group with date range | 2 | 1.21 | 1.10 | 1.34 |
| filtered group + OFFSET | 5 | 1.09 | 0.86 | 1.33 |
| scalar agg | 5 | 1.06 | 0.89 | 1.24 |
| point lookup | 1 | 1.03 | 1.03 | 1.03 |
| count distinct (whole table) | 2 | 0.88 | 0.68 | 1.14 |
| group-by single key | 7 | 0.86 | 0.46 | 1.41 |
| ORDER BY + LIMIT | 4 | 0.84 | 0.79 | 0.94 |
| group-by composite computed key | 2 | 0.60 | 0.43 | 0.84 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
