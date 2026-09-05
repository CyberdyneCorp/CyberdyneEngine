# `tools/abi/` — the ABI description and its compatibility gate

`native-abi`: *"The build SHALL generate a machine-readable description of the ABI (function names,
signatures, struct layouts, enum values) and CI SHALL diff it against the committed baseline. Any
change other than an append SHALL fail CI unless accompanied by an explicit, reviewed approval entry
recording the rationale and the version bump."*

| | |
|---|---|
| `abi_describe.py` | Parses `src/abi/include/cy/abi/cy_abi.h` and emits the description as JSON |
| `abi_gate.py` | Diffs the description against `src/abi/abi_baseline.json`, and `--update`s it |
| `selftest.py` | Breaks the ABI on purpose and checks that the gate notices |

Run through `just quality-abi`, and by CI through the CTest entries `integration.abi_baseline` and
`integration.abi_gate`. None of them needs a configured build.

## Why the description is derived rather than measured

The obvious way to learn a struct's layout is to compile a program that prints `offsetof`. That was
rejected twice over:

* a description produced by compiling describes **one toolchain on one machine**, and the baseline is
  committed and diffed across the whole platform matrix — a Windows runner and a Linux runner would
  legitimately disagree and the gate would have nothing stable to compare against;
* the point of a C ABI is that its layout is **fixed by the declaration**. Every member is a
  fixed-width integer, a float, a pointer, or an array of those. Deriving the layout states that
  rule; sampling it only observes one instance of it.

`src/abi/tests/test_layout.cpp` asserts the compiler's `sizeof` and `offsetof` against exactly the
numbers this generator computes, so the model is checked rather than assumed.

## Why the parser refuses what it does not understand

A parser that skipped an unfamiliar declaration would silently drop an ABI entry from the
description — and the gate would then happily approve removing it from the header. So an
unrecognised declaration is an error naming the text, and `cy_abi.h` stays inside the subset of C
that this reads.

## What is not compared

Parameter **names** and every comment. Renaming a parameter is a documentation change; a gate that
failed on one would be switched off within a month.

## The selftest has no fixture directory

Every case derives its input from the live header, applies one edit in memory, and runs the gate
against the real committed baseline. A copied fixture would be a snapshot of the ABI on the day it
was written — and worse, if the parser ever stopped recognising the interface table, a hand-written
"reordered" fixture and a hand-written "correct" one would both describe nothing, and comparing
nothing to nothing succeeds. The first case is the control: the unedited header matches the
baseline. Every other case is only meaningful because that one passes.
