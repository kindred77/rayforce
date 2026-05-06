# Rayforce vs DuckDB — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (duckdb_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.242 | 0.539 | 1.16 |
| 3 | scalar agg | 2.226 | 1.064 | 1.10 |
| 4 | scalar agg | 0.195 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.545 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 17.601 | 12.660 | 1.22 |
| 7 | scalar agg | 1.444 | 0.646 | 1.07 |
| 8 | group-by single key | 0.399 | 0.900 | 0.95 |
| 9 | group-by + count distinct | 32.117 | 15.617 | 1.64 |
| 10 | group-by + count distinct | 41.056 | 28.874 | 1.31 |
| 11 | group-by + count distinct | 34.137 | 5.710 | 2.81 |
| 12 | group-by + count distinct | 0.018 | 5.030 | 0.67 |
| 13 | group-by single key | 27.400 | 16.164 | 1.43 |
| 14 | group-by + count distinct | 47.692 | 25.159 | 1.64 |
| 15 | group-by + count distinct | 27.421 | 15.457 | 1.47 |
| 16 | group-by single key | 17.181 | 13.028 | 1.18 |
| 17 | group-by single key | 20.128 | 30.286 | 0.75 |
| 18 | group-by single key | 17.511 | 27.336 | 0.74 |
| 19 | group-by composite computed key | 37.308 | 43.922 | 0.88 |
| 20 | point lookup | 0.769 | 0.465 | 1.03 |
| 21 | LIKE filter | 17.023 | 14.417 | 1.11 |
| 22 | LIKE filter | 38.565 | 16.738 | 1.82 |
| 23 | LIKE filter | 59.279 | 25.398 | 1.96 |
| 24 | ORDER BY + LIMIT | 18.119 | 23.860 | 0.83 |
| 25 | ORDER BY + LIMIT | 0.795 | 3.385 | 0.81 |
| 26 | ORDER BY + LIMIT | 2.150 | 2.812 | 0.95 |
| 27 | ORDER BY + LIMIT | 0.662 | 3.068 | 0.82 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 29.547 | 13.043 | 1.72 |
| 32 | group-by + filter | 31.322 | 15.277 | 1.63 |
| 33 | group-by single key | 39.447 | 49.180 | 0.84 |
| 34 | group-by single key | 18.044 | 54.622 | 0.43 |
| 35 | group-by composite computed key | 19.480 | 58.670 | 0.43 |
| 36 | filtered group with date range | 25.467 | 16.230 | 1.35 |
| 37 | filtered group with date range | 10.953 | 8.963 | 1.10 |
| 38 | filtered group + OFFSET | 3.480 | 3.415 | 1.00 |
| 39 | filtered group + OFFSET | 2.825 | 5.030 | 0.85 |
| 40 | filtered group + OFFSET | 16.902 | 20.004 | 0.90 |
| 41 | filtered group + OFFSET | 7.078 | 2.439 | 1.37 |
| 42 | filtered group + OFFSET | 7.058 | 3.067 | 1.31 |
| 43 | minute-bucket time series | 12.043 | 3.465 | 1.64 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| group-by + filter | 2 | 1.67 | 1.63 | 1.72 |
| minute-bucket time series | 1 | 1.64 | 1.64 | 1.64 |
| LIKE filter | 3 | 1.58 | 1.11 | 1.96 |
| group-by + count distinct | 6 | 1.46 | 0.67 | 2.81 |
| filtered group with date range | 2 | 1.22 | 1.10 | 1.35 |
| filtered group + OFFSET | 5 | 1.07 | 0.85 | 1.37 |
| scalar agg | 5 | 1.03 | 0.89 | 1.16 |
| point lookup | 1 | 1.03 | 1.03 | 1.03 |
| count distinct (whole table) | 2 | 0.90 | 0.67 | 1.22 |
| group-by single key | 7 | 0.85 | 0.43 | 1.43 |
| ORDER BY + LIMIT | 4 | 0.85 | 0.81 | 0.95 |
| group-by composite computed key | 2 | 0.61 | 0.43 | 0.88 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
