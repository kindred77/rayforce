# Rayforce vs DuckDB — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (duckdb_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.172 | 0.539 | 1.15 |
| 3 | scalar agg | 2.184 | 1.064 | 1.10 |
| 4 | scalar agg | 0.201 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.414 | 11.721 | 0.66 |
| 6 | count distinct (whole table) | 15.490 | 12.660 | 1.12 |
| 7 | scalar agg | 1.475 | 0.646 | 1.08 |
| 8 | group-by single key | 5.354 | 0.900 | 1.41 |
| 9 | group-by + count distinct | 32.039 | 15.617 | 1.64 |
| 10 | group-by + count distinct | 39.197 | 28.874 | 1.27 |
| 11 | group-by + count distinct | 30.541 | 5.710 | 2.58 |
| 12 | group-by + count distinct | 0.018 | 5.030 | 0.67 |
| 13 | group-by single key | 25.595 | 16.164 | 1.36 |
| 14 | group-by + count distinct | 51.099 | 25.159 | 1.74 |
| 15 | group-by + count distinct | 27.251 | 15.457 | 1.46 |
| 16 | group-by single key | 16.110 | 13.028 | 1.13 |
| 17 | group-by single key | 19.147 | 30.286 | 0.72 |
| 18 | group-by single key | 17.329 | 27.336 | 0.73 |
| 19 | group-by composite computed key | 35.711 | 43.922 | 0.85 |
| 20 | point lookup | 0.596 | 0.465 | 1.01 |
| 21 | LIKE filter | 15.994 | 14.417 | 1.06 |
| 22 | LIKE filter | 37.351 | 16.738 | 1.77 |
| 23 | LIKE filter | 58.578 | 25.398 | 1.94 |
| 24 | ORDER BY + LIMIT | 18.078 | 23.860 | 0.83 |
| 25 | ORDER BY + LIMIT | 23.444 | 3.385 | 2.50 |
| 26 | ORDER BY + LIMIT | 45.934 | 2.812 | 4.37 |
| 27 | ORDER BY + LIMIT | 29.238 | 3.068 | 3.00 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 28.000 | 13.043 | 1.65 |
| 32 | group-by + filter | 29.075 | 15.277 | 1.55 |
| 33 | group-by single key | 38.642 | 49.180 | 0.82 |
| 34 | group-by single key | 17.958 | 54.622 | 0.43 |
| 35 | group-by composite computed key | 18.995 | 58.670 | 0.42 |
| 36 | filtered group with date range | 25.499 | 16.230 | 1.35 |
| 37 | filtered group with date range | 23.935 | 8.963 | 1.79 |
| 38 | filtered group + OFFSET | 23.563 | 3.415 | 2.50 |
| 39 | filtered group + OFFSET | 6.213 | 5.030 | 1.08 |
| 40 | filtered group + OFFSET | 16.973 | 20.004 | 0.90 |
| 41 | filtered group + OFFSET | 6.536 | 2.439 | 1.33 |
| 42 | filtered group + OFFSET | 6.700 | 3.067 | 1.28 |
| 43 | minute-bucket time series | 17.543 | 3.465 | 2.05 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| ORDER BY + LIMIT | 4 | 2.28 | 0.83 | 4.37 |
| minute-bucket time series | 1 | 2.05 | 2.05 | 2.05 |
| group-by + filter | 2 | 1.60 | 1.55 | 1.65 |
| filtered group with date range | 2 | 1.56 | 1.35 | 1.79 |
| LIKE filter | 3 | 1.54 | 1.06 | 1.94 |
| group-by + count distinct | 6 | 1.44 | 0.67 | 2.58 |
| filtered group + OFFSET | 5 | 1.33 | 0.90 | 2.50 |
| scalar agg | 5 | 1.03 | 0.89 | 1.15 |
| point lookup | 1 | 1.01 | 1.01 | 1.01 |
| group-by single key | 7 | 0.88 | 0.43 | 1.41 |
| count distinct (whole table) | 2 | 0.86 | 0.66 | 1.12 |
| group-by composite computed key | 2 | 0.60 | 0.42 | 0.85 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
