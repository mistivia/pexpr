# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`pexpr` is a small C11 library implementing **P Expression**, an S-expression-like
serialization format with five value types: integers, reals, strings, symbols, and lists
(e.g. `[1 2 [4 5 "str"] a-symbol]`). It provides an in-memory value type (`struct pnode`), a
serializer, a one-shot parser, and a streaming (incremental-feed) parser.

- `doc/FORMAT.md` — normative wire format spec, including design notes explaining
  *why* certain implementation choices were made (read this before changing parser
  or serializer behavior).
- `doc/API.md` — full API walkthrough with examples.
- `README.md` — quick overview and build/test commands.

## Build & test commands

```sh
make            # build build/debug/libpexpr.a
make test       # build and run the unit tests (tests/, no external framework)
make asan       # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan       # ThreadSanitizer
make msan       # MemorySanitizer (uninitialized reads; requires clang)
make coverage   # gcov/lcov line coverage -> build/coverage/html/
make check      # runs test + asan + tsan + msan + coverage (everything CI runs)
make clean      # rm -rf build
```

There is no test filtering flag — `tests/test_main.c` just calls
`run_pnode_tests()`, `run_serialize_tests()`, `run_parser_tests()`,
`run_stream_tests()` in sequence, each defined in the correspondingly named
`tests/test_*.c` file. To run a subset while iterating, comment out the calls
you don't need in `test_main.c`, or temporarily restrict which `TEST_SRCS` are
linked in the `Makefile`.

Always run `make asan` (and ideally `make check`) after touching `src/parser.c`
or anything involving `pnode` ownership — the parser's stackless-coroutine setup
and manual list-append/error-path cleanup are exactly the kind of code where
leaks and use-after-free regress silently under a plain debug build. `make msan`
matters here too: the coroutine resumes by jumping into the middle of the parse
procedures, so a suspension-crossing value kept in a plain local (instead of a
frame slot) reads as uninitialized on resume.

Link consumers against `build/debug/libpexpr.a` with `-Iinclude`.

## Architecture

### Value model: `struct pnode` is a plain value type, never heap-allocated itself

This is the one invariant that shapes the whole API and is easy to
violate by habit (e.g. reaching for a `pnode_free`/pointer-returning
pattern). Every constructor (`pnode_make_integ`, `pnode_make_real`,
`pnode_make_str`, `pnode_make_cstr`, `pnode_make_nsymbol`,
`pnode_make_symbol`, `pnode_make_list`, `pnode_copy`, `pexpr_parse`,
`p_parser_get_result`) returns `struct pnode` **by value**. The
library only ever heap-allocates what a node owns *internally* — a
string's or symbol's byte buffer, or a list's child array (`list`,
sized `list_cap`, first `list_len` valid) — never the `struct pnode`
wrapper. `PTYPE_SYMBOL` (a bareword matching `<initial> <subsequent>*`,
or one of the peculiar identifiers `+`/`-`, e.g. `nil`, `set!`,
`list->vector` — see `doc/FORMAT.md`) reuses the exact same
`str`/`str_len` storage and construction/drop/copy code paths as
`PTYPE_STR`; only `type`, case folding (symbols are case-insensitive:
`pnode_make_symbol()`/`pnode_make_nsymbol()` and the parser lowercase
uppercase ASCII letters before storing), and how the serializer emits
it differ. Consequently:

- Failure can't be signaled with `NULL`; it's signaled in-band via `pnode_ok()`
  (false for a failed `pnode_make_str`/`pnode_make_cstr`/`pnode_make_nsymbol`/
  `pnode_make_symbol`/`pnode_copy`/parse).
- `pnode_drop()` frees what a node owns and recurses into list children, but
  never frees `node` itself — callers own that storage (stack, array element).
  It's NULL-safe and idempotent.
- `pnode_list_append()` **moves** its `child` argument into the list's backing
  array on success; the caller must not touch `child` again afterward. On
  failure `child` is untouched (still the caller's to drop), since it was
  passed by value.
- List capacity grows by doubling (`list_cap`), starting at 0 → 1 → 2 → 4 ...
  (see `pnode_list_append` in `src/pnode.c`).
- The parser's `invalid_result()` and `pnode_copy()`'s failure path both use
  the same convention for "no value": `PTYPE_LIST` with `list == NULL` but
  `list_len == (size_t)-1` — distinguishable from a genuine empty list
  (`list_len == 0`) and checkable via `pnode_ok()`.

### Streaming parser: recursive-descent grammar written as synchronous code via a stackless coroutine

`src/parser.c` implements the streaming parser (`p_parser_init/feed/
get_result/errmsg/reset/destroy`) as a recursive-descent grammar that would
normally require a hand-rolled state machine to pause/resume across
`p_parser_feed()` calls. Instead the grammar is written as *coroutine
procedures* (`P_PEEK` / `P_VALUE` / `P_LIST` / `P_STRING` / `P_TOKEN`) driven
by a small stackless (Duff's-device) coroutine library, `src/stackless.{c,h}`
(adapted from [this gist](https://gist.github.com/mistivia/be432897e9be2782f83315c0871320e5)).
`parser_co()` is one C function whose body is a `CO_BEGIN`/`CO_END` switch:
`CO_YIELD` suspends it (returning `CO_PAUSED`) when the byte source runs dry,
and `CO_CALL`/`CO_RET` provide recursion by pushing/popping frames on an
*explicit heap-allocated* call stack (`struct co`, walked via `co_push`/
`co_top`/`co_pop`), so the native C stack never actually recurses.

Working in the coroutine body has sharp rules, because a resume jumps the
`switch` straight to the label of the pending `CO_YIELD`/`CO_CALL`, skipping
everything textually above it in that procedure:

- **No `switch` of your own** — it would capture the machinery's `case`
  labels. `CO_YIELD` (a `return`) and `CO_CALL`/`CO_RET` (a `goto` back to
  the single `co_dispatch` label) are all safe inside nested loops, so
  procedures use ordinary `for`/`while` loops containing calls and yields
  (e.g. `P_LIST`'s child loop). `break`/`continue` are fine too, but bind
  only to a loop you wrote — exit a *procedure* with `CO_RET`, never a bare
  `break`.
- **State that must survive a suspension lives in a frame slot or the impl,
  never a plain local.** Each procedure's locals go in its `*_args` frame
  struct (reached through the per-procedure `VAR()` shorthand for `CO_VAR`);
  the caller fills the leading fields and the procedure initializes the rest.
  The byte source is its own no-argument procedure, `P_PEEK` (invoked with
  `CO_CALL0`, which pushes a frameless stack entry): it skips comments and
  yields when the chunk runs dry, working entirely against `p->` fields
  (`peek_c`/`peek_have`) so a yield mid-peek loses nothing. Procedures return
  via the shared `p->rc` channel plus their frame's `out` pointer.

Abandoning a parse mid-stream (`p_parser_destroy()`/`p_parser_reset()` while
suspended) makes `co_drop()` free the frame stack without running any cleanup,
so `struct p_parser_impl` tracks in-flight allocations explicitly for
reclamation first:

- `open_lists` — a stack of pointers to every `PTYPE_LIST` node currently
  under construction (pushed when a `P_LIST` frame starts, popped before it
  `CO_RET`s via *any* exit path — success or error). These point into the
  coroutine frames' `vars` buffers.
- `active_buf` — the single `struct pbuf` (see `src/pbuf.h`/`pbuf.c`, an
  internal growable byte buffer) currently accumulating a string or number
  token, if any. Only one can be active since token collection never
  recurses.

`reclaim_in_flight()` drops those (freeing the `pnode` trees and token buffer
they own), and must run *before* `co_drop()` frees the frame `vars` buffers
that hold them. `p_parser_feed(self, 0, NULL)` (`len == 0`) signals EOF —
needed because a bare top-level scalar like `42` has no closing delimiter to
signal completion on its own. `pexpr_parse()` is a thin wrapper that runs this
loop for the whole-buffer-up-front case.

Nesting depth is capped at `PEXPR_MAX_DEPTH` (256, in `src/parser.c`) to bound
the coroutine's explicit call stack against adversarial input — exceeding it
is a normal syntax error, not a crash.

### Module layout

```
include/pexpr.h   public API — the source of truth for documented behavior
src/pnode.c        struct pnode: construction, drop, copy, list append
src/serialize.c     pnode -> P Expression text
src/parser.c        one-shot + streaming parser (recursive descent over a coroutine)
src/pbuf.c/.h       internal growable byte buffer, used only by the parser
src/stackless.c/.h  vendored stackless (Duff's-device) coroutine primitives
tests/              one test_*.c per module, no external framework (see tests/test.h)
```

## Format quirks worth knowing before touching parser/serializer

See `doc/FORMAT.md` for the full spec; a few asymmetries that are easy to
get wrong when changing either side:

- Reals always *encode* in scientific notation (shortest round-tripping
  mantissa), but *decoding* accepts a much looser grammar and classifies a
  token as real vs. integer purely by whether it contains `.`/`e`/`E`.
- String decoding is lenient (any raw byte except `\` or `"` passes through
  verbatim, including control chars and multi-byte UTF-8); encoding is strict
  (escapes everything outside printable ASCII `0x20`-`0x7e` as `\xhh`).
  Round-tripping through encode→decode is exact; decode→encode is not
  required to be.
- `+` and `-` start both numbers and the "peculiar identifier" symbols
  `+`/`-` - `P_TOKEN` (in its `TOK_NUMSYM` mode) in `src/parser.c` collects
  the whole token first (superset charset) and classifies it afterward,
  rather than trying to disambiguate character-by-character, since the
  streaming source may only have one byte available at a time.
