# Contributing to CyberdyneEngine

Two things are worth knowing before anything else.

**`just` is the entry point.** Run `just` with no arguments; it lists every recipe with a one-line
description, and those descriptions are the documentation of the workflow. If you are about to type
a build, test, format or generate command by hand, there is a recipe for it — or there should be,
and adding one is part of the change.

**The specifications come first.** `openspec/specs/` is the authority on what the engine does. Code
that disagrees with a specification is a defect in one of the two, and which one is a decision made
in a change proposal rather than in a pull request.

---

## Your first build

```
just                              # what can I do?
just env-doctor                   # is this machine ready?
just build-engine --profile dev   # configure and build
```

Or, without `just`, through CMake directly — building through the underlying build system is
supported and produces the same result:

```
cmake --preset dev
cmake --build --preset dev
```

You need CMake 3.28+, Ninja, and Clang 17+, GCC 13+ or MSVC 19.38+. On Windows, run from a
Developer Command Prompt so that the compiler is on the path; recipes use `bash`, which Git for
Windows provides.

## Profiles

Four names, and they mean the same thing in every toolchain the engine uses. Select one with
`--profile <name>` on any build, run or test recipe, or set `CY_PROFILE` for a whole shell.

| Profile | CMake configuration | Assertions | Editor | What it is for |
|---|---|---|---|---|
| `debug` | `Debug` | on | on | Debugging the engine itself |
| `dev` | `Development` | on | on | Day-to-day work; the default |
| `profile` | `Profile` | off | off | Measurements that reflect shipping performance |
| `release` | `Shipping` | off | off | Release builds |

The mapping lives in `cmake/profiles.cmake` and in the profile table at the top of the `justfile`,
and the two are cross-checked at configure time: a build fails rather than quietly using a
configuration you did not ask for. The Cargo and Slang columns are written down there too, unused
until those toolchains arrive at M5 and M3.

## Local configuration

Never edit a shared workflow file to make something work on your machine. Use `.just.local`,
`CMakeUserPresets.json`, or the environment — `CY_PROFILE`, `CY_BUILD_DIR`, `CY_JOBS`. All are
ignored by git, and recipes report when an override is in effect, so a divergence from the default
is visible when someone is diagnosing your problem.

## Making a change

Anything larger than a typo goes through the OpenSpec flow, and the artefacts are reviewed before
the code is written:

1. **Propose.** A change under `openspec/changes/<id>/` with `proposal.md` (why), `design.md` (the
   decisions the specifications left open, and why each is settled the way it is), spec deltas, and
   `tasks.md` (the ordered plan).
2. **Implement** against those artefacts, one task at a time.
3. **Validate.** `just quality-specs` runs `openspec validate --specs --strict`.
4. **Archive** the change, so the living specifications in `openspec/specs/` stay current.

`docs/roadmap/implementing.md` describes how a milestone becomes changes. `just roadmap-status` says
what is implemented; `just roadmap-milestone <id>` runs a milestone's exit criteria.

## Rules the code is held to

**C++20, no exceptions, no RTTI.** The engine compiles with `-fno-exceptions` and `-fno-rtti`.
Never `throw`, never `try`/`catch`, never `dynamic_cast` or `typeid`. An operation that can fail for
a reason the caller must handle returns `cy::Expected<T, Error>`; an invariant no correct caller can
violate is a `CY_ASSERT`, which fires in `Debug` and `Development` and is compiled out of `Profile`
and `Shipping`.

**Naming.** Namespace `cy`. Macros `CY_*`. Symbols exported through the flat C ABI are `cy_*`.

**Layering.** A lower layer never depends on a higher one:

```
core(0) < ecs(1) < servers(2) < backends + platform(3) < scene(4) < runtime(5) < abi(6) < editor + tools(7)
```

This is enforced, not advised. Every engine target is declared through `cy_add_module()`, which
records its layer and fails at configure time on an upward link; `just quality-layers` catches the
`#include` that CMake never saw, including the rule that no SDL header appears above `platform/`. A
bare `add_library` or `add_executable` in the engine tree is a lint failure, because it is a target
that opted out of the check.

**Platform code lives under `platform/<name>/`**, never behind an `#ifdef` on the host in a shared
file. Adding a platform must require no change in `src/core/`, `src/ecs/`, `src/servers/` or
`src/scene/`; if it does, the abstraction is wrong and that is the bug to fix.

**Every diagnostic field carries a privacy classification.** It is a required argument of the field
macro, not a decoration, and there is no overload that omits it.

**A bug fix carries a regression test.** The test goes in the same pull request as the fix.

## Before you push

```
just quality-format-check
just quality-lint
just quality-layers
just test-unit
```

Continuous integration runs these same recipes and nothing else, so a check that fails there
reproduces locally by construction. Where CI needs more — artefact upload, caching, sharding — it
wraps the recipes rather than replacing them.

## Documentation

Prose references recipes; it does not restate their command lines. A document that describes a
sequence of raw commands is a missing recipe, and the fix is to add the recipe and name it. A
directory's `README.md` states what belongs there and which capability governs it, so that where a
file goes is answered by reading one page rather than by pattern-matching against what already
exists.

## Recipes that are not implemented yet

The engine is early. A recipe that has no behaviour yet prints what it is and the task that fills
it, and exits non-zero:

```
$ just test-unit
just test-unit: not implemented (task 4.1.3)
```

That is deliberate. The recipe surface is the map of the workflow, and a name that exists with an
honest failure is more useful than a name that has to be guessed at later — and than one that
silently succeeds.
