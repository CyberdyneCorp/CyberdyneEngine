# tools/quality/ — the four gates `testing-and-quality` asked for and nothing implemented

M11.d task 7.1. `testing-and-quality`'s "Static analysis and formatting" requirement lists six
enforcement points; this repository had three of them. Its "Documentation as a gate" requirement
names a fourth check, and that one is **the "documentation gate" the M11 exit criteria name by that
phrase**. All four are here.

| Gate | Recipe | What it reads | State on the day it landed |
|---|---|---|---|
| `swift-format` | `just quality-swift-format` | 34 Swift files in `bindings/swift/` and `samples/04-character/game/` | **unchecked here** — no Swift toolchain on this host; proved on CI's Swift legs |
| Licence headers | `just quality-licence` | 2 584 source files under ten roots | **2 of 2 584 carry an SPDX line.** A 2 582-file declared backlog |
| Spelling | `just quality-spelling` | comments and documentation over 17 roots | **clean**, with 25 declared words and 8 excluded lines |
| Undocumented symbols | `just quality-docs` | 2 472 public symbols: C++ headers, the C ABI, the Swift overlay | **1 932 documented (78%)**; a 540-symbol declared backlog |

`just quality-gates-selftest` proves all four. Fourteen cases, thirteen of which run on a machine
with no Swift toolchain.

## Why two of them carry a backlog, and why that is not an allowlist

The licence gate's honest first reading was *2 584 of 2 584 files have no licence header*. Landing
it as a red gate would mean a gate switched off in its first week; prepending a line to 2 584 files
inside a phase where eleven agents share one working tree would mean a merge dropping the line from
an unknown subset — a licence defect introduced by a licence gate. The doc gate's first reading was
540 undocumented public symbols, and the requirement's own verb is **newly**: *CI SHALL fail when a
**newly** exported symbol is undocumented.*

So both carry a baseline, in the shape `src/abi/abi_baseline.json` already uses here, with **two**
rules rather than one:

* a file or symbol **not** in the baseline and **not** compliant fails the gate, naming it;
* a baseline entry that has since been **fixed, renamed or deleted also fails**, telling the author
  to remove the line.

The second rule is the whole difference. An allowlist that is never re-checked grows. A baseline
that goes red when it is stale can only shrink, and the count is printed on every run — a debt with
a number on it. `just quality-gates-selftest` has a case for each direction
(`licence-stale-baseline`, `doc-stale-baseline`), because a rule nobody has watched fire is a rule
nobody knows is there.

### The baseline is a snapshot, and M11.d's close must re-take it

The two baselines were generated part-way through M11.d, in a tree eleven agents were writing to at
once. **Every source file added after that moment fails the licence gate**, correctly — the gate is
doing exactly what it exists to do — but a rung cannot close on a gate that is red because its own
peers were still working. The close therefore re-runs

    just quality-licence --update
    just quality-docs --update

once, after the last agent's work has landed, and commits the two baselines with the counts in the
message. Anything added *after* that must carry a header and a doc comment, which is the point.

## What each gate decides, in one line each

* **`swift_format_gate.py`** — resolves `CY_SWIFT_FORMAT`, then `swift-format`, then `swift format`
  (a Swift 6 subcommand, so this adds no dependency the Swift bindings did not already have), and
  lints against the committed `.swift-format`. **Exit 2 means it did not run**, which the selftest
  tells apart from exit 1, because "no toolchain" is not "formatted correctly".
* **`licence_gate.py`** — one line, `SPDX-License-Identifier: MIT`, within a file's first five.
  SPDX rather than a copyright paragraph because a licence scanner run over a shipped source drop
  gets the same answer this gate does.
* **`spell_gate.py`** — codespell, which is a list of *known misspellings* rather than a dictionary.
  A dictionary speller over a game engine reports `navmesh` and `swizzle`; codespell reports only
  words that are known misspellings of real ones.
  Measured before any configuration: 889 hits, all but three of them this project's own vocabulary.
  `spelling-ignore.txt` suppresses a **word everywhere, with its reason on the line**;
  `spelling-exclude.txt` suppresses **one literal line**, which is where a misspelling that must stay
  goes — the Arabic letter in the text shaper's joining table, and the settings test that feeds a
  deliberately misspelled option and requires it to be refused. Putting the latter in the former
  would blind the gate to the commonest typo in English across the whole tree.
* **`doc_gate.py`** — namespace-scope declarations in `src/*/include/cy/**`, every `cy_` entry in the
  C ABI header, and every `public` declaration in the Swift overlay. Class *members* are deliberately
  not checked: a rule demanding a comment on every accessor is satisfied by noise.

## The guard against the defect this project has shipped nine times

`tools/roadmap/falsify.py` enumerates nine checks in this repository that could not go red. Two of
them passed by **comparing nothing to nothing** — a parser that stopped recognising its input and
reported a clean result. Every gate here refuses that explicitly:

* the licence gate fails when its roots yield **no files at all**;
* the spelling gate fails when its configuration is **missing**, and when its roots do not exist;
* the doc gate fails when a public header declaring `namespace cy` parses to **zero symbols**;
* the swift-format gate fails when `.swift-format` is **absent**, because a formatter with no
  committed configuration enforces whatever its default happens to be on the machine it ran on.

Each of those four has its own selftest case (`licence-reads-nothing`,
`spelling-no-configuration`, `doc-parser-blind`, `swift-no-configuration`), and a fifth case —
`spelling-ignore-list-is-load-bearing` — removes one word from the ignore list and requires the gate
to go red, which is how we know the list is being read rather than silently dropped.

## What this host could not prove

`swift-format-break` — break a Swift file's formatting and require the gate to refuse — needs a
Swift toolchain. M11.d was worked on Linux with no Apple toolchain, which is the same fact that moved
the Metal backend out of this rung. The case is reported **NOT EVALUATED**, which is not a pass, and
`just quality-gates-selftest --strict` — what CI runs on a leg that installs Swift 6 — refuses to
exit 0 with any case unrun.
