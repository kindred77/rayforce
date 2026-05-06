# Rayforce vs DuckDB — ClickBench, hot run

Ratio = (rayforce_hot + 10ms) / (duckdb_hot + 10ms). >1 means Rayforce is slower.

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
| 1 | scalar agg | 0.000 | 0.587 | 0.94 |
| 2 | scalar agg | 2.095 | 0.539 | 1.15 |
| 3 | scalar agg | 2.060 | 1.064 | 1.09 |
| 4 | scalar agg | 0.211 | 1.445 | 0.89 |
| 5 | count distinct (whole table) | 4.519 | 11.721 | 0.67 |
| 6 | count distinct (whole table) | 15.649 | 12.660 | 1.13 |
| 7 | scalar agg | 1.460 | 0.646 | 1.08 |
| 8 | group-by single key | 0.402 | 0.900 | 0.95 |
| 9 | group-by + count distinct | 31.916 | 15.617 | 1.64 |
| 10 | group-by + count distinct | 39.417 | 28.874 | 1.27 |
| 11 | group-by + count distinct | 30.837 | 5.710 | 2.60 |
| 12 | group-by + count distinct | 0.017 | 5.030 | 0.67 |
| 13 | group-by single key | 26.769 | 16.164 | 1.41 |
| 14 | group-by + count distinct | 51.421 | 25.159 | 1.75 |
| 15 | group-by + count distinct | 25.753 | 15.457 | 1.40 |
| 16 | group-by single key | 15.867 | 13.028 | 1.12 |
| 17 | group-by single key | 18.828 | 30.286 | 0.72 |
| 18 | group-by single key | 17.011 | 27.336 | 0.72 |
| 19 | group-by composite computed key | 35.718 | 43.922 | 0.85 |
| 20 | point lookup | 0.824 | 0.465 | 1.03 |
| 21 | LIKE filter | 16.057 | 14.417 | 1.07 |
| 22 | LIKE filter | 38.032 | 16.738 | 1.80 |
| 23 | LIKE filter | 59.083 | 25.398 | 1.95 |
| 24 | ORDER BY + LIMIT | 17.619 | 23.860 | 0.82 |
| 25 | ORDER BY + LIMIT | 24.622 | 3.385 | 2.59 |
| 26 | ORDER BY + LIMIT | 47.928 | 2.812 | 4.52 |
| 27 | ORDER BY + LIMIT | 29.492 | 3.068 | 3.02 |
| 28 | HAVING (unsupported) | null | 11.547 | — |
| 29 | REGEXP_REPLACE (unsupported) | null | 257.557 | — |
| 30 | wide expression (unsupported) | null | 4.542 | — |
| 31 | group-by + filter | 28.479 | 13.043 | 1.67 |
| 32 | group-by + filter | 30.486 | 15.277 | 1.60 |
| 33 | group-by single key | 39.326 | 49.180 | 0.83 |
| 34 | group-by single key | 21.300 | 54.622 | 0.48 |
| 35 | group-by composite computed key | 19.718 | 58.670 | 0.43 |
| 36 | filtered group with date range | 24.459 | 16.230 | 1.31 |
| 37 | filtered group with date range | 10.830 | 8.963 | 1.10 |
| 38 | filtered group + OFFSET | 3.432 | 3.415 | 1.00 |
| 39 | filtered group + OFFSET | 2.863 | 5.030 | 0.86 |
| 40 | filtered group + OFFSET | 15.763 | 20.004 | 0.86 |
| 41 | filtered group + OFFSET | 6.673 | 2.439 | 1.34 |
| 42 | filtered group + OFFSET | 6.802 | 3.067 | 1.29 |
| 43 | minute-bucket time series | 12.090 | 3.465 | 1.64 |

## Cluster aggregates (geomean ratio)

| Cluster | n | Geomean ratio | Best (lowest) | Worst (highest) |
| --- | --: | --: | --: | --: |
| ORDER BY + LIMIT | 4 | 2.32 | 0.82 | 4.52 |
| minute-bucket time series | 1 | 1.64 | 1.64 | 1.64 |
| group-by + filter | 2 | 1.64 | 1.60 | 1.67 |
| LIKE filter | 3 | 1.55 | 1.07 | 1.95 |
| group-by + count distinct | 6 | 1.44 | 0.67 | 2.60 |
| filtered group with date range | 2 | 1.20 | 1.10 | 1.31 |
| filtered group + OFFSET | 5 | 1.05 | 0.86 | 1.34 |
| point lookup | 1 | 1.03 | 1.03 | 1.03 |
| scalar agg | 5 | 1.03 | 0.89 | 1.15 |
| count distinct (whole table) | 2 | 0.87 | 0.67 | 1.13 |
| group-by single key | 7 | 0.85 | 0.48 | 1.41 |
| group-by composite computed key | 2 | 0.61 | 0.43 | 0.85 |

## Hard outliers (ratio ≥ 5.0)

| Q | Cluster | Rayforce ms | DuckDB ms | Ratio |
| --: | --- | --: | --: | --: |
