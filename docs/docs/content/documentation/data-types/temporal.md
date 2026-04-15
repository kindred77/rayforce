# :octicons-clock-16: Temporal Types

RayforceDB offers 3 types of temporal types: Date, Time and Timestamp


## :date: Date

!!! note ""
    Scalar Type Code: `-7`. Scalar Internal Name: `date`. Vector Type Code: `7`. Vector Internal Name: `DATE`

    Format: `YYYY.MM.DD`

```clj
↪ (type 2025.01.02)  ;; Scalar
date
↪ (type [2025.01.01 2025.01.02])  ;; Vector
DATE
```

```clj
(date 'local)  ;; local date
2025.12.10
(date 'global)  ;; UTC date
2025.12.10
```


## :octicons-clock-16: Time

!!! note ""
    Scalar Type Code: `-8`. Scalar Internal Name: `time`. Vector Type Code: `8`. Vector Internal Name: `TIME`

    Format: `HH:MM:SS.ms`

```clj
↪ (type 20:00:00)  ;; Scalar
time
↪ (type [20:00:00 21:00:00])  ;; Vector
TIME
```

```clj
(time 'local)  ;; Get local time
14:15:56.273
(time 'global)  ;; Get UTC time
12:16:10.426
```

## :material-clock-time-ten: Timestamp

!!! note ""
    Scalar Type Code: `-9`. Scalar Internal Name: `timestamp`. Vector Type Code: `9`. Vector Internal Name: `TIMESTAMP`

    Format: `YYYY.MM.DD\DHH:MM:SS.mmm`

```clj
↪ (type 2025.12.10D15:10:24.058948000)  ;; Scalar
timestamp
↪ (type [2025.12.10D15:10:24.058948000 2025.12.10D15:10:24.058948000])  ;; Vector
TIMESTAMP
```

```clj
(timestamp 'local)  ;; Get local timestamp
2025.12.10D15:11:50.908518000
(timestamp 'global)  ;; Get UTC timestamp
2025.12.10D15:12:00.695995000
```
