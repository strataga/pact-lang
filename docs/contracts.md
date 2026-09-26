# Pact interchange contracts

This document defines the first versioned machine formats emitted and accepted
by the current prototype. A format version is independent of the Pact language
version and compiler version.

## JSON AST

The root object contains `capabilities` and `body` arrays. Producers should
include `"version": 1`. For compatibility, a missing version is interpreted as
version 1. Any explicit version other than integer `1` is rejected with
`PACT-J011`; the compiler does not guess how to interpret an unknown format.

The current AST is intentionally small. Statements are `{"let": NAME,
"value": EXPR}` and `{"return": EXPR}`. Each expression is a strict tagged
object with exactly one form:

* `{"int": INTEGER}` is a signed 64-bit integer.
* `{"var": NAME}` refers to a binding.
* `{"bool": BOOLEAN}` is a boolean literal (`true` or `false`).
* `{"tool": "math.double", "args": [EXPR]}` calls the one registered tool.
* `{"bin": {"op": OP, "left": EXPR, "right": EXPR}}` is arithmetic; `OP` is
  `+`, `-`, or `*`.
* `{"cmp": {"op": OP, "left": EXPR, "right": EXPR}}` compares two `i64`
  expressions. `OP` is `==` or `<`, and the result is boolean. The nested
  object must contain exactly `op`, `left`, and `right`.
* `{"if": {"condition": EXPR, "then": EXPR, "else": EXPR}}` is a conditional
  expression. Its condition must be boolean and both branches must be `i64`;
  the result is `i64`. The nested object must contain exactly `condition`,
  `then`, and `else`.

Booleans cannot be used as arithmetic or comparison operands, returned, or
passed to a tool; these comparison operators accept only `i64`. Conditional
branches must both be `i64`. Tool calls anywhere inside a conditional branch,
including a nested conditional branch, are rejected because the current
lowering uses LLVM `select`, which evaluates both branch values before selection.
For example, the text expression `if count < 7 { 42 } else { 0 }` has this JSON form:

```json
{
  "if": {
    "condition": {
      "cmp": {
        "op": "<",
        "left": {"var": "count"},
        "right": {"int": 7}
      }
    },
    "then": {"int": 42},
    "else": {"int": 0}
  }
}
```

See `examples/double.json` for a complete program using the other forms.

## Diagnostics

Every diagnostic response is one JSON object with:

* `diagnostic_version`: integer `1` identifying this envelope format.
* `ok`: whether the requested check or compile stage succeeded.
* `diagnostics`: an array of objects with stable `code` and human-readable
  `message` fields.

Consumers should branch on diagnostic codes, not message text. The current
prototype does not yet include source spans or a versioned request/response
service; those are future interface work. Existing `PACT-*` codes remain stable
as the compiler grows.

The Linux `run` launcher reports sandbox setup, timeout, output-limit, CPU-limit,
cancellation, runtime-filter, cleanup, and host-tool-broker failures with
`PACT-S001` through `PACT-S009`. It does not execute a program directly when a
sandbox control is unavailable. The initial `math.double` implementation is
host-side and accepts only a fixed 16-byte versioned request for that operation,
with one signed 64-bit integer argument. The per-run Unix socket is exposed to
the generated process inside its namespace; no other host tool or credential is
available through it.

## Compatibility policy

An incompatible change to either machine format requires a new integer version.
Readers reject unsupported explicit versions with a stable code. The text
syntax is not covered by the JSON AST version. Compiler, runtime, and language
format versions should be recorded separately in any future benchmark output.
