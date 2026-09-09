# samples/05-editor-session — the M5 artefact

A scripted editing session: open a project, import a glTF asset, author a scene against a hosted
runtime, **have that runtime killed underneath the editor**, and finish the session with the
document intact — then recover the unsaved work in a new editor process and save it.

```
just run-editor-session                 # the whole session
just run-editor-session --only survive  # only the act that kills the runtime
```

`session.py` is the driver, `acts/*.cyscript` are the scripts the editor runs, and `project/` is the
project the session opens. Nothing here is a mock: the importer is `tools/import/`'s `cy_import_cli`,
the editor is `cyberdyne-editor`, and the runtime is a separate process on the other side of a Unix
domain socket.

## What the session proves, act by act

| act | what runs | what it decides |
|-----|-----------|-----------------|
| 0 | `cyberdyne-editor --list-commands` | which of the artefact's steps the command surface can do, and which it cannot — see "What is thinner than the task list" |
| 1 | `tools/project/project.py validate` | the project manifest is a real, valid project graph |
| 2 | `cy_import_cli`, twice | the glTF cooks to six sub-assets, both sidecars are written, the identity record carries a **non-zero cooked hash**, and the second import is a cache **hit** |
| 3 | the editor, with a runtime attached | a session authors the scene over the live bridge and reports `engine: hosted` |
| 4 | the editor, whose runtime is **SIGKILLed mid-session** | the editor outlives it, every command in the act still runs, the loss is surfaced with a remedy, and the document ends intact and unsaved |
| 5 | a new editor over the same journal | the unsaved work is offered back with a count, and saving discards the journal |

Act 4 is the milestone's argument for the editor being a client rather than part of the engine, and
it is the reason the artefact is shaped this way. A test that dropped a socket would prove the
protocol handles an end of stream; only killing a **process** proves the editor's fate is not tied
to the runtime's.

## How the kill is timed, which is the only subtle part

A scripted session is over in about three milliseconds, so "kill the runtime while the editor is
mid-session" cannot be timed by sleeping. The first attempt at this raced: the runtime died after
the editor had already exited, and the run reported the claim proven having proven nothing.

The editor reads `--script` with `read_to_string`, and it attaches its runtime **before** it reads
the script. So the act is delivered through a FIFO, and the ordering is decided rather than raced:

1. the editor connects — *observed*, by watching the runtime's open file descriptors, not assumed;
2. the runtime is killed with `SIGKILL`, while the editor sits blocked on the FIFO, connected;
3. the act is written to the FIFO and the writer closes, so every command in it runs afterwards;
4. the editor finishes the act, surfaces the loss, and exits zero.

The alternative was a `--pause-here` flag on the editor, which is a test hook in shipping code. A
FIFO is a property of the operating system and the editor has no idea it is being held.

## The driver's own negative cases

`python3 session.py --selftest` requires each of the two assertions that carry act 4 and act 3 to
**fail** when it should: a runtime that is never killed must not satisfy act 4, and a session whose
runtime never answered must not satisfy act 3. An act that passed either way would be a green tick
over nothing, which is the failure this repository has paid for before.

## What is thinner than the task list, precisely

Task 6.1 asks the session to "manipulate it with gizmos" and "enter play mode". **Neither is on the
editor's command surface**, and the artefact says so on every run rather than faking it:

```
==> act 0       the editor's command surface, 6 command(s)
    viewport.transform   NOT ON THE COMMAND SURFACE — a gizmo drag, as a transform transaction
    runtime.play         NOT ON THE COMMAND SURFACE — entering play mode
    asset.import         NOT ON THE COMMAND SURFACE — importing from inside the editor
```

**`asset.import` closed at M8.a**, and act 0 now prints it as present. Its `OPTIONAL_STEPS` entry
carries no invocation, and the reason is about this session rather than about the command: the
editor here runs with the REPOSITORY as its working directory, so it has no project open and
`assets/lamppost.gltf` is not a path it can resolve — act 2 already cooks that file by running
`cy_import_cli` against the sample's own project. The command itself is driven through the registry,
with the entity it creates asserted, by
`editor/crates/cy-editor-services/tests/importing_from_inside_the_editor.rs`.

`cy_editor_services::builtin::register` registers six commands — create and delete an entity,
select, undo, redo, save — and none of them transforms anything, starts a simulation, or imports.
The machinery behind all three exists and is tested: `cy_editor_viewport::gizmo` does the
manipulation maths, `cy_editor_viewport::play` holds the play state and the persistence statement,
and `tools/import/` is the pipeline this session drives from the command line instead. What is
missing is the registration that puts them on the one action surface, which is where
`editor-rust-application` requires every action to be. `OPTIONAL_STEPS` in `session.py` names the
three identifiers; the day they are registered, the session runs them without this file changing.

Two smaller gaps, both recorded where they bite:

* **A script cannot name an entity a previous line created.** `scene.create-entity` returns the
  identity on `Outcome::values["entity"]`, and `cy_editor_app::run_script` binds nothing, so
  `parent=`, `edit.select` and `scene.delete-entity` are unscriptable. `acts/01-place.cyscript`
  therefore creates three roots rather than a hierarchy, and says so.
* **`file.save` writes no backing asset.** Its own source says the serialisation is a later task;
  what it does implement is the sequence that matters — the journal is discarded only after the
  write reports success — and act 5 is what checks that.

## Why the artefact is a driver rather than a `cy_sample_*` binary

Every earlier milestone's artefact is one executable, because every earlier milestone's subject was
the engine. M5's subject is a boundary between processes written in two languages, and the claim is
a statement about processes. A C++ program that started an editor and a runtime and killed one of
them would link no engine code and prove nothing about the engine; what it would add is a build
dependency that says nothing. `bindings/swift/tools/` and `samples/04-character/tools/` orchestrate
multi-toolchain artefacts the same way, in the same language.

The consequence is that `just run-sample editor-session` does **not** work — `run-sample` finds
`samples/*/cy_sample_<name>` in the build tree and there is no such binary here. `just
run-editor-session` is the recipe, and `just/run.just` says why beside it.

## Where it runs

Linux and macOS. The live bridge is a Unix domain socket, and `cy-runtime-stub` says so itself on a
platform that has none; `session.py` exits 2 there rather than reporting a session it did not run.
The CTest entries in `tests/editor/` are registered only on such a host and only when `cargo` and
`just` are both present, for the same reason `samples/04-character` is not built without a Swift
toolchain: a check that skips is a check whose green means nothing.
