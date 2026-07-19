# The P Expression Format

P Expression (pexpr) is a small S-expression-like serialization format with
four value types: integers, reals, strings, and lists.

```
[1 2 [4 5 "str"]]
```

This document is the normative description of the wire format. Where the
original design left something unspecified, that is called out explicitly
along with the choice this implementation makes.

## Values

- **Integer** — decimal, base 10, optional leading `-`. No scientific
  notation, no leading `+` in encoder output (the parser accepts a leading
  `+` when reading, see "Parsing" below).
- **Real** — decimal, always encoded in scientific notation, e.g. `1.5e+02`,
  `-2.5e-03`, `0e+00`. NaN and infinity have no representation in this
  format; encoding one is an error.
- **String** — `"..."`, see below.
- **List** — `[...]`, elements separated by a single space, e.g.
  `[1 2 [4 5 "str"]]`. No trailing separator, no comma.

## Strings

Strings are wrapped in `"`. `\` starts an escape sequence:

| Escape | Byte     |
|--------|----------|
| `\0`   | `0x00`   |
| `\\`   | `0x5c`   |
| `\a`   | `0x07`   |
| `\b`   | `0x08`   |
| `\t`   | `0x09`   |
| `\n`   | `0x0a`   |
| `\v`   | `0x0b`   |
| `\f`   | `0x0c`   |
| `\r`   | `0x0d`   |
| `\"`   | `0x22`   |
| `\xhh` | byte `0xhh` |

**Encoding** a string escapes every byte outside the printable-ASCII range
`0x20`–`0x7e` as `\xhh` with lowercase hex digits (one escape per byte —
non-ASCII text such as UTF-8 is escaped byte-by-byte, not per code point).
Everything else is either one of the named escapes above or is written
literally.

**Decoding** is lenient: any byte that is not `\` or the closing `"` is
copied through verbatim, so parsed strings may contain raw newlines,
control characters, and multi-byte UTF-8 sequences without needing
escapes. `\x` hex digits are accepted in either case (`\x4a` and `\x4A`
both decode to `J`), even though the encoder only ever emits lowercase. An
unrecognized escape (e.g. `\q`) is a syntax error.

## Numbers

**Encoding**: integers are plain decimal; reals always use scientific
notation, formatted with the shortest mantissa precision that still
round-trips exactly back to the original `double` through `strtod()`.

**Decoding** is lenient about the source grammar: a token is read greedily
as `[+-]?[0-9]*(\.[0-9]*)?([eE][+-]?[0-9]*)?` starting from a byte that is
a digit, `+`, or `-`, and is classified as a **real** if it contains `.`,
`e`, or `E`, and as an **integer** otherwise. Whatever the token turns out
to be, it must still parse cleanly as a whole under `strtod`/`strtoll`
(trailing junk like `1.2.3` or `1e` with no exponent digits is a syntax
error).

## Lists

`[` and `]` delimit a list. Elements are read until `]`; between elements,
any run of space, `\t`, `\n`, or `\r` acts as a separator (the encoder only
ever emits single ASCII spaces; the decoder accepts the wider whitespace
set and multiple/mixed occurrences of it, including none between a value
and a following `]`).

## Design notes / deviations from the original spec

The original design (see `DESIGN` in the repository history) left a few
details implicit; this is what the implementation does and why:

- **`struct pnode`'s `list` field** is `struct pnode *list`, matching the
  sketch literally: a contiguous array of child *values* (not pointers),
  paired with an explicit `size_t list_len` rather than a NULL terminator
  (a NULL terminator doesn't make sense for an array of values — there's
  no pointer to be NULL). The sketch's separate `void **mem` bookkeeping
  field was dropped: since `pnode_free()` already switches on `type`, it
  can free what a node owns directly (the string buffer, or the `list`
  array itself) without a generic side table, and recurses into each
  element of `list` for children. See the doc comment on `struct pnode`
  in `include/pexpr.h`.
- **`struct pnode` is a value type throughout the construction API.**
  `pnode_new_integ()`/`pnode_new_real()`/`pnode_new_str()`/
  `pnode_new_cstr()`/`pnode_new_list()`/`pnode_copy()` return `struct
  pnode` directly rather than a heap-allocated pointer, and
  `pnode_list_append()`'s `child` parameter is a value too (moved into
  the list's array on success). This library never allocates the
  `struct pnode` wrapper itself - only what a `PTYPE_STR`/`PTYPE_LIST`
  value owns internally (a string's bytes, a list's child array) is
  heap memory, and only those two constructors can fail. Since a
  failure can no longer be signaled with a `NULL` return, it's signaled
  in-band instead and checked with `pnode_ok()` (see `include/pexpr.h`);
  `pnode_free()` correspondingly frees what a node owns but never the
  node itself. The one place this library still hands back a
  heap-allocated `struct pnode *` is at the parser's outer boundary
  (`pexpr_parse()`, `p_parser_get_result()`), where there's no
  caller-provided storage to write into - free those with
  `pnode_delete()`, not `pnode_free()`.
- **End of input for the streaming parser.** A bare top-level scalar like
  `42` has no closing delimiter, so the parser cannot know it's complete
  until told there's nothing more coming. `p_parser_feed(self, 0, NULL)`
  (`len == 0`) signals end of input. This is the one addition on top of
  the literal `p_parser_feed` signature in the design notes, and one-shot
  `pexpr_parse()` uses it internally so callers of that function don't
  need to know about it.
- **Nesting depth** is capped (`PEXPR_MAX_DEPTH` in `src/parser.c`,
  currently 256) to keep adversarial input from exhausting the parser
  coroutine's stack; exceeding it is a normal syntax error, not a crash.
