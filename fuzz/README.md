# Fuzzing

Coverage-guided fuzz targets for the highest-value untrusted-input surfaces.
Linux + clang only (the system toolchain on macOS does not bundle the fuzzing
runtime).

## Targets

| Target | Entry point | Surface |
|---|---|---|
| `fuzz_parse`    | `ray_parse`            | Rayfall source parser / lexer |
| `fuzz_numparse` | `ray_parse_i64/f64/u64_hex` | numeric-literal scanners (pure, no runtime) |
| `fuzz_de`       | `ray_de` / `ray_de_raw` | binary wire-frame / journal-entry decoder |

The journal log is a sequence of IPC frames verbatim, so the `de` corpus also
hardens journal replay's decode step.

## Running

```sh
make fuzz-parse                 # 60s (default) run of one target
make fuzz-parse FUZZ_RUNTIME=0  # run until a crash / Ctrl-C
make fuzz-smoke                 # short pass over all targets (CI gate)
```

Grown corpora live in `fuzz/corpus/<target>/` (gitignored); committed starter
inputs live in `fuzz/seeds/<target>/`.  Regenerate the working corpora from the
test suite before a session:

```sh
scripts/fuzz-seed-parse.sh      # parse/eval inputs from test/rfl
scripts/fuzz-seed-frames.sh     # binary frames captured from a journaling server
```

### Local note: AddressSanitizer + high-entropy ASLR

On some newer kernels the sanitizer runtime intermittently faults at process
exit under the default ASLR entropy — unrelated to any target code (it also
happens on an empty input).  If you see nondeterministic `SEGV`s at teardown,
run under reduced ASLR:

```sh
setarch -R make fuzz-parse
```

CI runners are unaffected.  If clang auto-selects a gcc toolchain directory
without `libstdc++` (a partially-installed newer gcc shadowing the real one),
pass the real one:

```sh
make fuzz-smoke FUZZ_LDEXTRA=-L/usr/lib/gcc/x86_64-linux-gnu/<ver>
```

## Crash triage workflow

```
crash-XXXX appears (CI artifact or local run)
  1. confirm:   ./build_fuzz/fuzz_<t> crash-XXXX
  2. minimize:  ./build_fuzz/fuzz_<t> -minimize_crash=1 -runs=20000 crash-XXXX
  3. fix on the dev branch (normal PR flow)
  4. regression test:
       language-expressible bug  -> test/rfl/regress/<name>.rfl   (expr !- code)
       binary-input bug          -> keep the minimized input in fuzz/seeds/<t>/
  5. retain the minimized input under fuzz/seeds/<t>/ so the corpus never
     forgets the case
```

Error assertions in `.rfl` regression tests match the 7-byte error **code**
(e.g. `!- parse`, `!- domain`), not the detailed message — the message is not
part of the value rendered for comparison.
