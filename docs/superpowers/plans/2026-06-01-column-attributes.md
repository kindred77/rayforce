# Column Attributes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add first-class, verified, persisted column attributes (`sorted`, `unique`, `grouped`, `parted`) on top of the existing accelerator-index layer, and wire the asof-join executor to skip its mandatory sort when inputs already carry the right attributes.

**Architecture:** `sorted` is a cheap marker bit (`RAY_ATTR_SORTED = 0x20`, the one `attrs` bit free across all vector types) — no allocation. `unique` is a marker stored in the index block's spare byte (block allocated on demand). `grouped` reuses the existing `RAY_IDX_HASH` builder; `parted` is a new `RAY_IDX_PART` index kind (value → contiguous `[start,len)` ranges). A new `.attr.*` builtin family sets/reads/drops attributes with strict verify-on-set. Propagation is conservative: only trivially-preserving operators keep attributes. The asof executor reads these attributes and skips per-side sorts when valid.

**Tech Stack:** C (C11), the Rayforce engine. Build: `make test` (debug + ASan/UBSan, runs the C unit suite and auto-discovers `test/rfl/**/*.rfl`). New `src/ops/*.c` files are auto-globbed by the Makefile.

**Spec:** `docs/superpowers/specs/2026-06-01-column-attributes-design.md`

---

## Background: verified codebase facts

These are confirmed by reading the source. Trust them; re-verify only if an edit fails.

- **Core type** `ray_t` (`include/rayforce.h:113-162`): byte 19 is `uint8_t attrs`. Byte 17 `order` is the **allocator block-order — not free**. Byte 16 `mmod` is heap/mmap.
- **`attrs` bit map** (`src/mem/heap.h:48-67`): `0x02` GRAPH (-i64 atoms), `0x04` HNSW(-i64 atoms)/HAS_LINK(i32,i64 vecs), `0x08` HAS_INDEX (vecs), `0x10` SLICE (vecs), `0x20` NAME (**-sym atoms only**), `0x40` HAS_NULLS (vecs), `0x80` ARENA (all), `0x01-0x03` SYM width (sym vecs). **`0x20` is free for every vector type** (it is only meaningful on `-RAY_SYM` *atoms*), so it is safe to reuse as `RAY_ATTR_SORTED` on vectors.
- **Index block**: when `attrs & RAY_ATTR_HAS_INDEX (0x08)`, `ray_t.index` (bytes 0-7, aliased over `nullmap`) points to a `RAY_INDEX` child whose `data[]` holds a `ray_index_t` (`src/ops/idxop.h:66-125`). Fields: `uint8_t kind; uint8_t saved_attrs; int8_t parent_type; uint8_t reserved; int64_t built_for_len; uint8_t saved_nullmap[16]; union u {...}`. **`reserved` is the spare byte we repurpose as `markers`.**
- **Index kinds** (`src/ops/idxop.h:48-64`): `RAY_IDX_NONE=0, HASH=1, SORT=2, ZONE=3, BLOOM=4, CHUNK_ZONE=5`. We add `RAY_IDX_PART=6`.
- **Attach pattern** (`src/ops/idxop.c`): `prepare_attach(vp, "what")` validates + drops any existing index + COWs + rejects non-numeric (`numeric_elem_size==0`) → returns the (possibly new) parent or a `RAY_ERROR`. `ray_index_alloc(kind, parent_type, parent_len)` allocates the block. `attach_finalize(parent, idx)` snapshots the nullmap, sets `parent->index`, sets `HAS_INDEX`. See `ray_index_attach_sort` at `idxop.c:786` for the template.
- **Builtin fn shape**: `attach_via(v, fn)` at `idxop.c:1034` retains `v`, calls `fn(&w)`, releases on error. Unary fns `ray_t* f(ray_t* v)`. Binary fns `ray_t* f(ray_t* a, ray_t* b)`.
- **Errors**: `ray_error("code", "printf fmt", ...)` returns a `RAY_ERROR` object. Codes used: `"type"`, `"nyi"`, `"domain"`, `"oom"`, etc. `RAY_IS_ERR(p)` tests.
- **Vector helpers**: `ray_is_vec(v)`, `ray_data(v)` (→ element bytes), `ray_vec_is_null(v, i)`, `ray_vec_new(type, cap)` (then set `->len`), `numeric_elem_size(t)` (`idxop.c:39`), `ray_cow(v)`, `ray_retain`/`ray_release`.
- **Symbols**: a `'foo` literal is an atom of type `-RAY_SYM` whose `->i64` is the interned id. Intern with `int64_t ray_sym_intern_runtime(const char* s, size_t n)` (`src/table/sym.h:113`).
- **Registration** (`src/lang/eval.c:2817-2823`): `register_unary(".idx.zone", RAY_FN_NONE, ray_idx_zone_fn);` etc. `register_binary(name, attrs, fn)` exists (`eval.c:2435`).
- **asof executor**: `exec_window_join` (`src/ops/join.c:1556-1904`) sorts both inputs by `(eq-keys, time)` via bottom-up mergesort on index arrays `li_idx[]`/`ri_idx[]` (lines ~1682-1761), then two-pointer merges. Surface: `(asof-join [keys… time] left right)`; last key is the time column.
- **`.rfl` test format** (`test/main.c:190-210`): one self-contained expression per line. `LHS -- RHS` formats both and string-compares. `EXPR !- SUBSTR` expects a `RAY_ERROR` whose text contains SUBSTR. `;;` comment, blank ignored, bare `(set …)` lines persist state. Files auto-discovered under `test/rfl/`.
- **`.idx.info` builder** (`idxop.c:961-1026`): a `switch (kind)` emitting a dict via `dict_append_sym_i64(&keys,&vals,"field",val)`. Has a `case RAY_IDX_NONE: break;` and **no `default`**, so adding `RAY_IDX_PART` requires a new `case` or the compiler `-Werror=switch` will fail the build.

---

## File structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/mem/heap.h` | attrs flag defs | Add `RAY_ATTR_SORTED 0x20` + doc |
| `src/ops/idxop.h` | index kinds, payload, public API | Add `RAY_IDX_PART`, part payload, `markers` byte (rename `reserved`), marker-flag defines, inline attr accessors, fn decls |
| `src/ops/idxop.c` | attach builders + new attribute builtins | Add part builder, sorted/unique/grouped/parted setters, `.attr.get`/`.attr.drop`, ascending/distinct verify scans, `.idx.info` part case |
| `src/lang/eval.c` | builtin registration | Register `.attr.*` family next to `.idx.*` |
| `src/ops/join.c` | asof executor | Add attribute fast-path before the sort |
| `test/rfl/integration/attributes.rfl` | attribute behavior tests | Create |
| `test/rfl/integration/joins.rfl` | asof equivalence tests | Append |
| `docs/docs/queries/indexes.md` (+ new attributes section) | user docs | Document `.attr.*` and the asof fast-path |

---

## Task 1: Header scaffolding — flag, kinds, payload, accessors

This task adds **all** the header-level declarations the later tasks reference (so every later task compiles standalone) plus the `sorted` accessor. No behavior change.

**Files:**
- Modify: `src/mem/heap.h` (attrs flag block near line 48-67)
- Modify: `src/ops/idxop.h` (kind enum, payload struct, marker defines, accessors)

- [ ] **Step 1: Add the sorted flag**

In `src/mem/heap.h`, in the attrs-flag block, add (keep the existing comment style):

```c
/* RAY_ATTR_SORTED (vectors): the vector's elements are known to be in
 * non-descending order.  A pure marker — no backing structure, no
 * allocation.  Set only via (.attr.set 'sorted v) after an O(n) verify
 * scan, so it never lies.  0x20 is free for vectors (it means NAME only
 * on -RAY_SYM atoms).  Order-aware operators (asof-join) may trust it. */
#define RAY_ATTR_SORTED  0x20
```

- [ ] **Step 2: Add the part index kind**

In `src/ops/idxop.h`, add `RAY_IDX_PART = 6,` to `ray_idx_kind_t` (after `RAY_IDX_CHUNK_ZONE = 5`).

- [ ] **Step 3: Rename the spare byte and add the part payload + marker defines**

In `ray_index_t`, rename `uint8_t reserved;` (line ~72) to `uint8_t markers;`. Then `grep -rn "->reserved\|\.reserved" src/ops/idxop.c` and fix any reference. Inside `union u`, add:

```c
struct {                /* RAY_IDX_PART */
    ray_t*  keys;       /* distinct partition values, in ascending block order */
    ray_t*  starts;     /* RAY_I64, row offset where each part begins */
    ray_t*  lens;       /* RAY_I64, row count of each part */
    int64_t n_parts;
} part;
```

Above the `ray_index_t` struct, add:

```c
/* Marker bits stored in ray_index_t.markers (block-resident attributes
 * that have no dedicated attrs bit).  sorted lives in attrs (RAY_ATTR_SORTED),
 * not here. */
#define RAY_MARK_UNIQUE  0x01
```

- [ ] **Step 4: Add the sorted accessor**

In `src/ops/idxop.h`, after the `ray_index_kind` inline (around line 165), add:

```c
/* --- Semantic attribute markers (see mem/heap.h: RAY_ATTR_SORTED) --- */

/* True iff v is a vector flagged sorted (non-descending). */
static inline bool ray_attr_is_sorted(const ray_t* v) {
    return v && !RAY_IS_ERR((ray_t*)v) && ray_is_vec(v)
        && (v->attrs & RAY_ATTR_SORTED);
}
```

- [ ] **Step 5: Build to confirm headers compile**

Run: `make test 2>&1 | tail -5`
Expected: builds and the existing suite passes (no behavior change yet). Adding the `markers` field and `RAY_IDX_PART` enum value with no `default` in `.idx.info`'s switch will NOT break the build yet because the part case is added in Task 4 — but if `-Werror=switch` fires now on the unhandled `RAY_IDX_PART`, add the Task-4 Step-6 `case RAY_IDX_PART:` early.

- [ ] **Step 6: Commit**

```bash
git add src/mem/heap.h src/ops/idxop.h
git commit -m "feat(attr): header scaffolding — sorted flag, part kind, markers byte

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `(.attr.set 'sorted v)`, `(.attr.get v)`, `(.attr.drop v)` with verify

**Files:**
- Modify: `src/ops/idxop.c` (append attribute builtins after `ray_idx_info_fn`, ~line 1063)
- Modify: `src/ops/idxop.h` (declare the new fns, near line 214-220)
- Modify: `src/lang/eval.c` (register, after line 2823)
- Test: `test/rfl/integration/attributes.rfl` (create)

- [ ] **Step 1: Write failing tests**

Create `test/rfl/integration/attributes.rfl`:

```
;; ===== sorted marker: set / get / drop =====
;; set on an ascending vector succeeds and round-trips through get
(set v [1 2 2 5 9])(.attr.get (.attr.set 'sorted v)) -- [`sorted]
;; set on a non-ascending vector errors
(.attr.set 'sorted [3 1 2]) !- sorted
;; get on a plain vector is empty
(.attr.get [1 2 3]) -- (`$())
;; drop clears the marker
(.attr.get (.attr.drop (.attr.set 'sorted [1 2 3]))) -- (`$())
;; unknown attribute name errors
(.attr.set 'bogus [1 2 3]) !- attr
```

Note: `[1 2 3]` is an i64 vector. `` (`$()) `` is the empty symbol vector — confirm the exact empty-symbol-vector literal/format against `test/rfl/` before finalizing; if the formatter prints it differently, match the formatter's output (the harness string-compares formatted values). If unsure, replace the two empty-result lines with `(.idx.has? (.attr.drop (.attr.set 'sorted [1 2 3]))) -- 0b`.

- [ ] **Step 2: Run to verify they fail**

Run: `make test 2>&1 | grep -i "attributes\|fail" | head`
Expected: failures — `.attr.set`/`.attr.get`/`.attr.drop` are unbound (parse/eval error).

- [ ] **Step 3: Implement the ascending-scan verifier**

In `src/ops/idxop.c`, add near the other static helpers (after `numeric_key_word`, ~line 80). This compares consecutive elements with correct signed/float ordering; rejects columns with nulls or NaN in v1 (asof time columns have neither):

```c
/* Returns true iff numeric vector v is non-descending.  v1 scope: rejects
 * (returns false) if any null or NaN is present — callers turn false into a
 * verify error.  Caller has already ensured numeric_elem_size(v->type) > 0. */
static bool vec_is_ascending(const ray_t* v) {
    int64_t n = v->len;
    if (n < 2) return true;
    if (v->attrs & RAY_ATTR_HAS_NULLS) {
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null((ray_t*)v, i)) return false;
    }
    const uint8_t* b = (const uint8_t*)ray_data((ray_t*)v);
    switch (v->type) {
    case RAY_BOOL: case RAY_U8: {
        const uint8_t* p = b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I16: {
        const int16_t* p = (const int16_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I32: case RAY_DATE: {
        const int32_t* p = (const int32_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I64: case RAY_TIME: case RAY_TIMESTAMP: {
        const int64_t* p = (const int64_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_F32: {
        const float* p = (const float*)b;
        for (int64_t i = 0; i < n; i++) if (p[i] != p[i]) return false; /* NaN */
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_F64: {
        const double* p = (const double*)b;
        for (int64_t i = 0; i < n; i++) if (p[i] != p[i]) return false; /* NaN */
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    default: return false;
    }
}
```

- [ ] **Step 4: Implement the marker setter, get, drop**

Append after `ray_idx_info_fn` (~line 1063) in `src/ops/idxop.c`. (`set_sorted_marker` COWs because it mutates the header `attrs` byte; mirrors `prepare_attach`'s COW discipline without dropping any existing index — `sorted` may coexist with a backing index.)

```c
/* --------------------------------------------------------------------------
 * Semantic attributes — (.attr.* ) family.  See docs spec
 * 2026-06-01-column-attributes-design.md.
 * -------------------------------------------------------------------------- */

/* Set the sorted marker after verifying.  Takes a borrowed v, returns an
 * owning ref of the (possibly COW-copied) vector, or a RAY_ERROR. */
static ray_t* attr_set_sorted(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("type", "attr: null");
    if (!ray_is_vec(v))
        return ray_error("type", "sorted: attribute applies to vectors only");
    if (numeric_elem_size(v->type) == 0)
        return ray_error("nyi", "sorted: only numeric vectors supported in v1 (type %d)",
                         (int)v->type);
    if (!vec_is_ascending(v))
        return ray_error("domain", "sorted: column is not in non-descending order");
    ray_retain(v);
    ray_t* w = ray_cow(v);            /* ray_cow consumes the +1 we just took */
    if (!w || RAY_IS_ERR(w)) { return w ? w : ray_error("oom", NULL); }
    w->attrs |= RAY_ATTR_SORTED;
    return w;
}

/* (.attr.get v) -> symbol vector of attributes currently held (any order),
 * or the empty symbol vector.  Reads the sorted bit and the index block's
 * markers byte + backing-index kind. */
ray_t* ray_attr_get_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* syms = ray_vec_new(RAY_SYM, 4);
    if (!syms || RAY_IS_ERR(syms)) return syms ? syms : ray_error("oom", NULL);
    syms->len = 0;
    int64_t* out = (int64_t*)ray_data(syms);
    if (ray_attr_is_sorted(v))
        out[syms->len++] = ray_sym_intern_runtime("sorted", 6);
    if (ray_index_has(v)) {
        ray_index_t* ix = ray_index_payload(v->index);
        if (ix->markers & RAY_MARK_UNIQUE)
            out[syms->len++] = ray_sym_intern_runtime("unique", 6);
        if (ix->kind == RAY_IDX_HASH)
            out[syms->len++] = ray_sym_intern_runtime("grouped", 7);
        else if (ix->kind == RAY_IDX_PART)
            out[syms->len++] = ray_sym_intern_runtime("parted", 6);
    }
    return syms;
}

/* (.attr.drop v) -> v with all attributes and any backing index removed. */
ray_t* ray_attr_drop_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    if (w->attrs & RAY_ATTR_HAS_INDEX) {
        ray_t* r = ray_index_drop(&w);   /* COWs + detaches; also clears markers byte (it lives in the block) */
        if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    }
    if (w->attrs & RAY_ATTR_SORTED) {
        ray_t* c = ray_cow(w);           /* consumes our +1 */
        if (!c || RAY_IS_ERR(c)) return c ? c : ray_error("oom", NULL);
        c->attrs &= ~RAY_ATTR_SORTED;
        w = c;
    }
    return w;
}

/* (.attr.set 'name v) — dispatch on the symbol name. */
ray_t* ray_attr_set_fn(ray_t* name, ray_t* v) {
    if (RAY_IS_ERR(name)) return name;
    if (!name || name->type != -RAY_SYM)
        return ray_error("type", "attr.set: first arg must be a symbol");
    int64_t id = name->i64;
    if (id == ray_sym_intern_runtime("sorted", 6))   return attr_set_sorted(v);
    if (id == ray_sym_intern_runtime("unique", 6))    return attr_set_unique(v);
    if (id == ray_sym_intern_runtime("grouped", 7))   return attr_set_grouped(v);
    if (id == ray_sym_intern_runtime("parted", 6))    return attr_set_parted(v);
    return ray_error("domain", "attr.set: unknown attribute (want sorted/unique/grouped/parted)");
}
```

`attr_set_unique`, `attr_set_grouped`, `attr_set_parted` are added in Tasks 3-4. For this task, stub the three not-yet-implemented ones just above `ray_attr_set_fn` so it compiles:

```c
static ray_t* attr_set_unique (ray_t* v) { (void)v; return ray_error("nyi", "unique: not yet implemented"); }
static ray_t* attr_set_grouped(ray_t* v) { (void)v; return ray_error("nyi", "grouped: not yet implemented"); }
static ray_t* attr_set_parted (ray_t* v) { (void)v; return ray_error("nyi", "parted: not yet implemented"); }
```

`RAY_MARK_UNIQUE`, the `markers` field, and `RAY_IDX_PART` were all declared in Task 1, so `ray_attr_get_fn` compiles as written (the `unique`/`grouped`/`parted` branches simply never fire until Tasks 3-4 populate them).

- [ ] **Step 5: Declare the new fns**

In `src/ops/idxop.h`, near line 220, add:

```c
ray_t* ray_attr_set_fn (ray_t* name, ray_t* v); /* (.attr.set 'name v) */
ray_t* ray_attr_get_fn (ray_t* v);              /* (.attr.get v) -> sym vec */
ray_t* ray_attr_drop_fn(ray_t* v);              /* (.attr.drop v) -> v cleared */
```

- [ ] **Step 6: Register the builtins**

In `src/lang/eval.c`, after line 2823 (the `.idx.info` registration):

```c
register_binary(".attr.set",  RAY_FN_NONE, ray_attr_set_fn);
register_unary (".attr.get",  RAY_FN_NONE, ray_attr_get_fn);
register_unary (".attr.drop", RAY_FN_NONE, ray_attr_drop_fn);
```

- [ ] **Step 7: Run tests**

Run: `make test 2>&1 | grep -i "attributes\|FAIL\|PASS" | tail`
Expected: the sorted/get/drop/unknown-name lines pass. (`unique`/`grouped`/`parted` lines come in Tasks 3-4.)

- [ ] **Step 8: Commit**

```bash
git add src/ops/idxop.c src/ops/idxop.h src/lang/eval.c test/rfl/integration/attributes.rfl
git commit -m "feat(attr): .attr.set/get/drop with verified sorted marker

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: `unique` marker (block-resident) and `grouped` (hash index)

**Files:**
- Modify: `src/ops/idxop.c` (implement `attr_set_unique`, `attr_set_grouped`, distinct verify)
- Test: `test/rfl/integration/attributes.rfl` (append)

The `markers` byte and `RAY_MARK_UNIQUE` were added in Task 1. Confirm `ray_index_alloc` zero-inits the payload (so `markers` starts at 0); if it does not memset, set `markers = 0` explicitly when allocating a block in the builders below.

- [ ] **Step 1: Write failing tests** (append to `attributes.rfl`)

```
;; ===== unique marker =====
(.attr.get (.attr.set 'unique [1 2 3 4])) -- [`unique]
(.attr.set 'unique [1 2 2 4]) !- unique
;; sorted + unique coexist
(.attr.get (.attr.set 'unique (.attr.set 'sorted [1 2 3]))) -- [`sorted `unique]
;; ===== grouped (hash-backed) =====
(.idx.has? (.attr.set 'grouped [1 1 2 3 3])) -- 1b
(.attr.get (.attr.set 'grouped [1 1 2 3 3])) -- [`grouped]
```

Confirm the symbol-vector format (`[`sorted `unique]`) against the formatter; adjust ordering to match how `ray_attr_get_fn` appends (sorted, then unique, then grouped/parted) and how the formatter renders a 2-element sym vector.

- [ ] **Step 2: Run to verify failure**

Run: `make test 2>&1 | grep -i "attributes" `
Expected: the new lines fail (`nyi: unique/grouped`).

- [ ] **Step 3: Implement distinct verify + `attr_set_unique`**

Replace the `attr_set_unique` stub. Distinctness via a one-shot open-addressing set over `numeric_key_word` (already in `idxop.c`). `unique` requires the index block to hold the marker, so allocate a `RAY_IDX_NONE` block carrying only `markers`, unless a backing index already exists (then set the bit on it).

```c
/* True iff all non-null rows are distinct.  Open-addressing probe over a
 * power-of-two table sized 2x the row count.  v1: numeric vectors only. */
static bool vec_all_distinct(const ray_t* v) {
    int64_t n = v->len;
    if (n < 2) return true;
    uint64_t cap = next_pow2((uint64_t)n * 2 + 1);
    if (cap < 16) cap = 16;
    uint64_t mask = cap - 1;
    int64_t* slot = (int64_t*)calloc((size_t)cap, sizeof(int64_t)); /* 0 = empty (store i+1) */
    if (!slot) return false; /* caller will treat as verify failure → safe */
    const uint8_t* base = (const uint8_t*)ray_data((ray_t*)v);
    bool ok = true;
    for (int64_t i = 0; i < n && ok; i++) {
        if (ray_vec_is_null((ray_t*)v, i)) continue;
        uint64_t h = mix64(numeric_key_word(base, v->type, i)) & mask;
        for (;;) {
            int64_t cur = slot[h];
            if (cur == 0) { slot[h] = i + 1; break; }
            uint64_t hw = numeric_key_word(base, v->type, cur - 1);
            uint64_t hi = numeric_key_word(base, v->type, i);
            if (hw == hi) { ok = false; break; }   /* duplicate */
            h = (h + 1) & mask;
        }
    }
    free(slot);
    return ok;
}

static ray_t* attr_set_unique(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("type", "attr: null");
    if (!ray_is_vec(v))
        return ray_error("type", "unique: attribute applies to vectors only");
    if (numeric_elem_size(v->type) == 0)
        return ray_error("nyi", "unique: only numeric vectors supported in v1 (type %d)",
                         (int)v->type);
    if (!vec_all_distinct(v))
        return ray_error("domain", "unique: column contains duplicate values");

    ray_retain(v);
    ray_t* w = ray_cow(v);
    if (!w || RAY_IS_ERR(w)) return w ? w : ray_error("oom", NULL);

    if (w->attrs & RAY_ATTR_HAS_INDEX) {
        ray_index_payload(w->index)->markers |= RAY_MARK_UNIQUE;
        return w;
    }
    /* No backing index: attach a marker-only block (kind NONE). */
    ray_t* idx = ray_index_alloc(RAY_IDX_NONE, w->type, w->len);
    if (!idx || RAY_IS_ERR(idx)) { ray_release(w); return idx ? idx : ray_error("oom", NULL); }
    ray_index_payload(idx)->markers = RAY_MARK_UNIQUE;
    return attach_finalize(w, idx);
}
```

Note: `attach_finalize` returns `parent` (here `w`) which we own. Confirm `ray_index_alloc` zero-inits `markers`; if not, set it explicitly as shown.

Also update `ray_index_has`/`.idx.has?` expectations: a marker-only block makes `ray_index_has` return true (it checks `HAS_INDEX && index!=NULL`). The unique test above does not assert `.idx.has?`, so this is acceptable; `.idx.info` will report `kind:none` (see Step 6 of Task 4 — ensure the `RAY_IDX_NONE` case is handled, which it already is at `idxop.c:1016`).

- [ ] **Step 4: Implement `attr_set_grouped`**

Replace the stub. `grouped` is exactly a hash index; reuse the existing builder:

```c
static ray_t* attr_set_grouped(ray_t* v) {
    /* grouped == hash index over the column.  prepare_attach (inside
     * ray_index_attach_hash) validates, COWs, and drops any prior index;
     * markers (e.g. unique) on that prior index are intentionally cleared
     * because at most one backing index is kept. */
    return attach_via(v, ray_index_attach_hash);
}
```

- [ ] **Step 5: Run tests**

Run: `make test 2>&1 | grep -i "attributes\|FAIL" | tail`
Expected: unique + grouped lines pass; no regressions.

- [ ] **Step 6: Commit**

```bash
git add src/ops/idxop.c test/rfl/integration/attributes.rfl
git commit -m "feat(attr): unique marker (block-resident) and grouped (hash index)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: `parted` — new `RAY_IDX_PART` index kind

**Files:**
- Modify: `src/ops/idxop.c` (part builder, `attr_set_parted`, `.idx.info` case, release/retain of part vectors)
- Test: `test/rfl/integration/attributes.rfl` (append)

`RAY_IDX_PART` and the `u.part` payload struct were added to `src/ops/idxop.h` in Task 1.

- [ ] **Step 1: Write failing tests** (append to `attributes.rfl`)

```
;; ===== parted (contiguous value-blocks) =====
;; a column already laid out as ascending value-blocks accepts parted
(.attr.get (.attr.set 'parted [1 1 1 2 2 3])) -- [`parted]
;; .idx.info reports the part kind
(at (.idx.info (.attr.set 'parted [1 1 2 2])) 'kind) -- `part
;; a non-blocked column is rejected (verify-only: no reorder)
(.attr.set 'parted [1 2 1 2]) !- parted
;; descending blocks rejected (blocks must be ascending)
(.attr.set 'parted [3 3 1 1]) !- parted
```

Confirm `(at (.idx.info …) 'kind)` returns a symbol and that the part kind formats as `` `part `` (the info builder emits the kind string — match it; if the existing builder emits e.g. `` `sort `` for sort, use the analogous `` `part ``).

- [ ] **Step 2: Run to verify failure**

Run: `make test 2>&1 | grep -i "attributes"`
Expected: parted lines fail (`nyi: parted`).

- [ ] **Step 3: Implement the part builder + verify + `attr_set_parted`**

Append in `src/ops/idxop.c` (near the other attach builders). The verify is a single pass: each new value must be strictly greater than the previous block's value (ascending, contiguous blocks ⇒ globally non-descending with equal runs). Build keys/starts/lens in the same pass.

```c
/* Build a RAY_IDX_PART index, verifying the column is laid out as
 * contiguous, ascending value-blocks.  Returns the attached parent or a
 * RAY_ERROR.  v1: numeric vectors only (enforced by prepare_attach). */
ray_t* ray_index_attach_part(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "part");
    if (RAY_IS_ERR(v)) return v;
    int64_t n = v->len;

    /* Contiguous-ascending-blocks ⇒ non-descending overall AND each distinct
     * value appears in exactly one run.  Non-descending is necessary; a value
     * recurring after a different value would violate "one contiguous block",
     * but non-descending order already forbids that.  So verifying
     * non-descending is sufficient. */
    if (!vec_is_ascending(v))
        return ray_error("domain", "parted: column is not laid out as ascending value-blocks");

    /* Count blocks (runs of equal values). */
    const uint8_t* base = (const uint8_t*)ray_data(v);
    int64_t nparts = (n > 0) ? 1 : 0;
    for (int64_t i = 1; i < n; i++)
        if (numeric_key_word(base, v->type, i) != numeric_key_word(base, v->type, i-1))
            nparts++;

    ray_t* starts = ray_vec_new(RAY_I64, nparts > 0 ? nparts : 1);
    ray_t* lens   = ray_vec_new(RAY_I64, nparts > 0 ? nparts : 1);
    ray_t* keys   = ray_vec_new(v->type, nparts > 0 ? nparts : 1);
    if (RAY_IS_ERR(starts) || RAY_IS_ERR(lens) || RAY_IS_ERR(keys)) {
        if (!RAY_IS_ERR(starts)) ray_release(starts);
        if (!RAY_IS_ERR(lens))   ray_release(lens);
        if (!RAY_IS_ERR(keys))   ray_release(keys);
        return ray_error("oom", NULL);
    }
    starts->len = lens->len = keys->len = nparts;
    int64_t* st = (int64_t*)ray_data(starts);
    int64_t* ln = (int64_t*)ray_data(lens);
    int es = numeric_elem_size(v->type);
    uint8_t* kb = (uint8_t*)ray_data(keys);

    int64_t p = 0, run_start = 0;
    for (int64_t i = 1; i <= n; i++) {
        bool boundary = (i == n) ||
            (numeric_key_word(base, v->type, i) != numeric_key_word(base, v->type, i-1));
        if (boundary && n > 0) {
            st[p] = run_start;
            ln[p] = i - run_start;
            memcpy(kb + (size_t)p*es, base + (size_t)run_start*es, (size_t)es);
            p++;
            run_start = i;
        }
    }

    ray_t* idx = ray_index_alloc(RAY_IDX_PART, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(starts); ray_release(lens); ray_release(keys);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.part.keys    = keys;
    ix->u.part.starts  = starts;
    ix->u.part.lens    = lens;
    ix->u.part.n_parts = nparts;
    return attach_finalize(v, idx);
}

static ray_t* attr_set_parted(ray_t* v) {
    return attach_via(v, ray_index_attach_part);
}
```

- [ ] **Step 4: Wire release/retain of the part-index child vectors**

The index block owns `keys`/`starts`/`lens`. Find where the index payload's child `ray_t*`s are released/retained (`ray_index_release_payload`/`ray_index_retain_payload`, declared `idxop.h:200-210`; defined in `idxop.c`). Add a `case RAY_IDX_PART:` to each that releases/retains `u.part.keys`, `u.part.starts`, `u.part.lens` (mirror how `RAY_IDX_SORT` handles `u.sort.perm` and `RAY_IDX_BLOOM` handles `u.bloom.bits`). **Skipping this leaks (and ASan in `make test` will catch a leak/UAF).**

- [ ] **Step 5: Add the `.idx.info` part case**

In `ray_index_info` (`idxop.c`, the `switch (kind)` ~line 961-1018), add before `case RAY_IDX_NONE:`:

```c
case RAY_IDX_PART:
    r = dict_append_sym_i64(&keys, &vals, "n_parts", ix->u.part.n_parts);
    if (RAY_IS_ERR(r)) goto fail;
    break;
```

Also ensure the function emits the `kind` field as `` `part `` for `RAY_IDX_PART` — locate where it maps kind→symbol (the existing code emits `` `sort ``/`` `hash `` etc.) and add the `part` mapping. If it uses a static `const char* kind_name[]` array, append `"part"` at index 6.

- [ ] **Step 6: Run tests**

Run: `make test 2>&1 | grep -i "attributes\|FAIL\|leak" | tail`
Expected: parted lines pass; no ASan leak reports.

- [ ] **Step 7: Commit**

```bash
git add src/ops/idxop.c test/rfl/integration/attributes.rfl
git commit -m "feat(attr): parted attribute via new RAY_IDX_PART index kind

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Conservative invalidation

Fresh result vectors already default to `attrs == 0` and no index, so most operators drop attributes automatically. The risk is paths that **copy the header verbatim** or **return the input retained**. This task adds an explicit guard helper and applies it where attributes could leak through a transform, plus tests proving drop-on-transform and keep-on-copy.

**Files:**
- Modify: `src/ops/idxop.h` (add `ray_attr_clear` inline)
- Modify: targeted operator sites (identified below)
- Test: `test/rfl/integration/attributes.rfl` (append)

- [ ] **Step 1: Add the clear helper**

In `src/ops/idxop.h`:

```c
/* Strip all semantic attributes (sorted bit + any index/markers block) from
 * a vector the caller exclusively owns (rc==1, freshly produced).  Cheap:
 * for the common case there is nothing to clear. */
static inline void ray_attr_clear_owned(ray_t* v) {
    if (!v || RAY_IS_ERR(v) || !ray_is_vec(v)) return;
    if (v->attrs & RAY_ATTR_HAS_INDEX) { ray_t* w = v; ray_index_drop(&w); /* in place on rc==1 */ }
    v->attrs &= ~RAY_ATTR_SORTED;
}
```

Confirm `ray_index_drop` operates in place when `rc==1` (it COWs only if shared). If it can reallocate, do not use this inline on a borrowed pointer — instead clear at the allocation site (Step 3).

- [ ] **Step 2: Write the tests** (append to `attributes.rfl`)

```
;; ===== conservative propagation =====
;; filtering a sorted column drops the marker
(set s (.attr.set 'sorted [1 2 3 4 5]))(.attr.get (where (> s 2) s)) -- (`$())
;; arithmetic drops the marker
(.attr.get (+ (.attr.set 'sorted [1 2 3]) 1)) -- (`$())
;; append/join result is unattributed
(.attr.get (, (.attr.set 'sorted [1 2 3]) [4 5])) -- (`$())
;; a plain copy / aliasing via set keeps it (trivially-preserving)
(set s2 (.attr.set 'sorted [1 2 3]))(.attr.get s2) -- [`sorted]
```

Adjust the exact builtins (`where`, `,` for join/append, arithmetic) to the codebase's real verb names — confirm against `test/rfl/` and `src/lang/eval.c`. The intent: pick three transforms (filter, arithmetic, concat) and assert the result has no attributes; assert a stored/aliased value keeps it.

- [ ] **Step 3: Run to verify current behavior**

Run: `make test 2>&1 | grep -i "attributes"`
Expected: most likely **already pass** (fresh allocations have `attrs==0`). If any transform *leaks* an attribute (test fails), that operator copies the header — fix it: at the point it builds/returns the result vector, call `ray_attr_clear_owned(result)` (or ensure it allocates fresh rather than retaining the input). Record which operators needed the guard in the commit message.

- [ ] **Step 4: Audit slices and COW explicitly**

- `grep -rn "RAY_ATTR_SLICE" src/` — a slice shares the parent buffer but is a *reordering-agnostic window*; a slice of a sorted vector starting at 0 is still sorted, but a slice is conservatively **not** guaranteed sorted (offset/stride). Ensure slice creation does **not** copy `RAY_ATTR_SORTED` from parent to slice (slices set their own `attrs` with `SLICE`; verify `SORTED` is not OR'd in). Add a `.rfl` test if slices are user-reachable.
- `grep -rn "ray_cow" src/mem/cow.c` — COW copies the header including `attrs` and the index pointer. For a true copy (same contents) that is **correct** (the copy really is still sorted / still has a valid index). Confirm COW deep-copies or retains the index block consistently (it already must, since `.idx.*` relies on COW). No change expected; add a comment if you touch it.

- [ ] **Step 5: Run tests**

Run: `make test 2>&1 | grep -i "attributes\|FAIL" | tail`
Expected: all propagation lines pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(attr): conservative attribute invalidation across transforms

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: asof fast-path — un-partitioned both-sorted

**Files:**
- Modify: `src/ops/join.c` (`exec_window_join`, ~line 1556-1904)
- Test: `test/rfl/integration/joins.rfl` (append)

- [ ] **Step 1: Read the executor's sort step**

Read `src/ops/join.c:1556-1842`. Identify: where `li_idx[]`/`ri_idx[]` are built and sorted (the bottom-up mergesort, ~1682-1761), and the variables holding the left/right time columns and row counts. The fast-path replaces "sort the index array" with "identity index array" when the side is already ordered.

- [ ] **Step 2: Write the equivalence tests** (append to `joins.rfl`)

```
;; asof-join fast-path: pre-sorted single-key inputs match the unsorted result
(set L (table [Time Price] (list [10:00:01.000 10:00:03.000] [100.0 101.0])))
(set R (table [Time Bid] (list [10:00:00.000 10:00:02.000 10:00:04.000] [99.0 100.5 101.5])))
(asof-join [Time] L R) -- (asof-join [Time] (update Time (.attr.set 'sorted Time) from L) (update Time (.attr.set 'sorted Time) from R))
```

The RHS marks the Time columns sorted and must produce the identical table. Adjust the column-marking syntax to the real way to set an attribute on a table column (confirm the update/amend form in `test/rfl/`; if marking a column in place is awkward, instead add a C-level unit test in `test/test_lang.c` that builds two tables, marks the time columns via `attr_set_sorted`, and asserts `fmt_eq` of `asof-join` with and without the marker). The invariant to prove: **result is byte-identical with and without the attribute.**

- [ ] **Step 3: Add the fast-path branch**

In `exec_window_join`, before the left-side mergesort, detect when the single time key is already sorted and skip sorting that side — fill its index array with the identity permutation `0,1,…,n-1` instead. Guard conditions: exactly one join key (un-partitioned), and that column carries `RAY_ATTR_SORTED`. Pseudocode to insert (adapt variable names to the actual code):

```c
/* Fast-path: un-partitioned asof where a side's time column is already
 * sorted.  Skip that side's mergesort; the natural row order IS the sort. */
bool single_key = (n_keys == 1);              /* eq-keys == 0, only the time key */
ray_t* lt = /* left time column */;
ray_t* rt = /* right time column */;
bool l_presorted = single_key && ray_attr_is_sorted(lt);
bool r_presorted = single_key && ray_attr_is_sorted(rt);

if (l_presorted) { for (int64_t i = 0; i < ln; i++) li_idx[i] = i; }
else             { /* existing left mergesort */ }

if (r_presorted) { for (int64_t i = 0; i < rn; i++) ri_idx[i] = i; }
else             { /* existing right mergesort */ }
```

Include `"ops/idxop.h"` in `join.c` if not already (for `ray_attr_is_sorted`). Keep the existing two-pointer merge unchanged — it consumes `li_idx`/`ri_idx` regardless of how they were produced.

- [ ] **Step 4: Run tests**

Run: `make test 2>&1 | grep -i "joins\|FAIL" | tail`
Expected: the equivalence line passes; all existing asof/window-join tests still pass.

- [ ] **Step 5: Commit**

```bash
git add src/ops/join.c test/rfl/integration/joins.rfl
git commit -m "perf(join): asof fast-path skips sort for pre-sorted single-key inputs

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: asof fast-path — partitioned (parted + sorted-within-part)

**Files:**
- Modify: `src/ops/join.c` (`exec_window_join`)
- Test: `test/rfl/integration/joins.rfl` (append)

- [ ] **Step 1: Define the qualifying condition**

A side qualifies for skip-sort in the partitioned case `(asof-join [Sym Time] …)` when: the eq-key column (`Sym`) carries `RAY_IDX_PART` (so rows are already grouped into contiguous, ascending value-blocks) **and** the time column is sorted within each part. The simplest sufficient, cheaply-checkable condition: `Sym` is `parted` **and** `Time` carries `RAY_ATTR_SORTED` *globally* is NOT correct (global time order ≠ per-part order). So check per-part: walk the part index's `starts`/`lens` and confirm `Time` is non-descending inside each `[start, start+len)`. That check is O(n) — cheaper than the O(n log n) sort it avoids, and only runs when `Sym` is already parted.

- [ ] **Step 2: Write the equivalence test** (append to `joins.rfl`)

```
;; partitioned asof fast-path equivalence (parted Sym, sorted-within-part Time)
(set L (table [Sym Time Price] (list [a a b b] [10:00:01.000 10:00:03.000 10:00:01.000 10:00:02.000] [1.0 2.0 3.0 4.0])))
(set R (table [Sym Time Bid] (list [a a b b] [10:00:00.000 10:00:02.000 10:00:00.000 10:00:03.000] [9.0 9.5 8.0 8.5])))
(asof-join [Sym Time] L R) -- (asof-join [Sym Time] L R)
```

This self-equality only exercises the path if the build also marks the columns; as in Task 6, prefer a C unit test in `test/test_lang.c` that constructs L/R, applies `attr_set_parted` to `Sym` and `attr_set_sorted` per-part to `Time` on one copy, and asserts `fmt_eq(asof(plain), asof(attributed))`. The invariant: identical result with and without attributes.

- [ ] **Step 3: Add a helper: time-sorted-within-parts**

In `join.c` (or `idxop.c` exposed via `idxop.h`):

```c
/* True iff `time` is non-descending within every part described by the
 * RAY_IDX_PART index on `keycol`.  keycol must be parted. */
static bool time_sorted_within_parts(const ray_t* keycol, const ray_t* time) {
    if (ray_index_kind(keycol) != RAY_IDX_PART) return false;
    ray_index_t* ix = ray_index_payload(keycol->index);
    const int64_t* st = (const int64_t*)ray_data(ix->u.part.starts);
    const int64_t* ln = (const int64_t*)ray_data(ix->u.part.lens);
    const uint8_t* tb = (const uint8_t*)ray_data((ray_t*)time);
    for (int64_t p = 0; p < ix->u.part.n_parts; p++) {
        int64_t s = st[p], e = st[p] + ln[p];
        for (int64_t i = s + 1; i < e; i++)
            if (numeric_key_word(tb, time->type, i) < numeric_key_word(tb, time->type, i-1)) {
                /* numeric_key_word is fine for monotonic temporal types
                 * (TIME/TIMESTAMP/DATE/I64) which are the asof time types;
                 * if float time is ever allowed, use a typed compare here. */
                return false;
            }
    }
    return true;
}
```

`numeric_key_word` is order-preserving for the unsigned/temporal integer types used as asof time keys; if asof ever admits float/negative-int time columns, swap in the typed comparison from `vec_is_ascending`. Document this assumption inline.

- [ ] **Step 4: Wire into `exec_window_join`**

When `n_keys >= 2`, set `l_presorted = (the side's eq-key col is parted) && time_sorted_within_parts(eqkey, time)`. When true, fill that side's index array with the identity permutation (rows are already in `(part, time)` order). Reuse the same identity-fill / mergesort branch from Task 6, generalized:

```c
bool l_can_skip = (single_key && ray_attr_is_sorted(lt))
               || (n_keys >= 2 && ray_index_kind(l_eqkey) == RAY_IDX_PART
                                && time_sorted_within_parts(l_eqkey, lt));
/* …same for right… */
```

Confirm `l_eqkey` (the single partition column for the 2-key case). For >2 keys (multiple eq-keys), conservatively do **not** skip (fall back to sort) unless a multi-column part index exists — out of scope; `log`/comment this limitation.

- [ ] **Step 5: Run tests**

Run: `make test 2>&1 | grep -i "joins\|FAIL\|leak" | tail`
Expected: equivalence passes; existing partitioned asof tests unaffected.

- [ ] **Step 6: Commit**

```bash
git add src/ops/join.c src/ops/idxop.h test/rfl/integration/joins.rfl test/test_lang.c
git commit -m "perf(join): asof fast-path for parted+sorted partitioned inputs

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Documentation

Per project convention (docs site is MkDocs Material; new features need prose, not just reference rows), document the attribute family and the asof fast-path. **No vendor names** anywhere (no third-party product or language names).

**Files:**
- Modify: `docs/docs/queries/indexes.md` (add an Attributes section) — or create `docs/docs/queries/attributes.md` and add it to `mkdocs.yml` nav.

- [ ] **Step 1: Decide page placement**

Read `docs/docs/queries/indexes.md` and `mkdocs.yml`. If indexes.md is short, add a `## Column attributes` section there. If it's already large, create `docs/docs/queries/attributes.md` and add a nav entry under the Queries section in `mkdocs.yml`.

- [ ] **Step 2: Write the prose**

Cover: the four attributes and their meaning; `.attr.set`/`.attr.get`/`.attr.drop` with worked examples (mirror the `.idx.*` example style already in indexes.md); strict verify-on-set (errors on violation); that `sorted`+`unique` combine but only one backing index is kept; conservative propagation (attributes are dropped by transforms — re-assert after); and the asof-join fast-path (pre-sorted single-key inputs, or `parted`+sorted-within-part partitioned inputs, skip the per-join sort). Use only `.rfl` snippets that you have run.

- [ ] **Step 3: Verify the docs build (if tooling present)**

Run: `command -v mkdocs >/dev/null && mkdocs build -q 2>&1 | tail || echo "mkdocs not installed; skipping local build (CI builds on push)"`
Expected: clean build, or the skip message.

- [ ] **Step 4: Commit**

```bash
git add docs/
git commit -m "docs(queries): document column attributes and asof fast-path

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification

- [ ] **Run the whole suite:** `make test 2>&1 | tail -20` — all green, no ASan/UBSan reports.
- [ ] **Re-read the spec** (`docs/superpowers/specs/2026-06-01-column-attributes-design.md`) and confirm each section maps to a task: attribute model (T1-4), surface syntax (T2-4), set semantics/verify (T2-4), propagation (T5), asof payoff (T6-7), representation (T1,T3,T4). 
- [ ] **Confirm no vendor names** introduced: confirm no third-party product or language names were introduced in new code/docs.
```
