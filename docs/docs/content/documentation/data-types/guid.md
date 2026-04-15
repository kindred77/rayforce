# :material-identifier: GUID

!!! note ""
    Scalar Type Code: `-11`. Scalar Internal Name: `guid`. Vector Type Code: `11`. Vector Internal Name: `GUID`.

Represents a Globally Unique Identifier

```clj
↪ (as 'guid "a358b66b-9f6c-401d-b985-685a44d2d114")  ;; Scalar
a358b66b-9f6c-401d-b985-685a44d2d114
```

```clj
(guid 1)  ;; Vector (generate 1 random GUID)
[e3e0cbc4-cd46-4697-802e-74ec6551cbf0]
(guid 3)  ;; Vector (generate 3 random GUIDs)
[c708cebf-1c68-4a9c-b981-085c483e5a7a e6251773-4517-41e8-b9c2-5da39ced0406 e49df487-0cfe-4b36-9819-7395f0cb7aa2..]
```
