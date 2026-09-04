# `src/core/config/` — the project graph and layered typed configuration

Layer 0, target `cy::core-config`, headers `<cy/core/config/...>`, namespace `cy::config`.
Section 4 of M1, governed by `project-and-plugins` and by `engine-architecture`'s "Deterministic
startup and shutdown".

| Header | What it is |
|---|---|
| `project.h` | the project graph, read-only: modules, plugins, content roots, build targets, settings |
| `settings.h` | layered typed configuration — schema, layers, provenance, shipping policy |
| `module_registry.h` | the four registration levels, and the order modules come up and go down in |
| `config.h` | the umbrella |

## The enforcement is not here, and cannot be

`project-and-plugins` requires that a cycle, an undeclared dependency and an upward layer dependency
**fail the build**. A check that ran inside a compiled program would run only after the build it was
supposed to fail had succeeded. So the enforcement is `tools/project/` and `cmake/project.cmake`, at
configure time, and what lives here is the *result*: `cmake/project.cmake` validates the manifest and
renders `<build>/generated/project/include/cy_project.h`, and `project.cpp` gives that file's
X-macro tables names and types.

There is therefore exactly one manifest parser in the tree, and it is in the language that has one.
A second in C++ would be a second thing to keep in agreement with the first, and the engine would pay
a JSON parser in its startup path to get it. `tools/project/README.md` has the schema and the
fixtures; `tools/project/selftest.py` runs as the CTest test `integration.project_graph`.

When the tree carries no project manifest, `cy_project.h` is rendered from `modules/*/module.json`
instead and `project().manifest_present` is false. That is still a declared graph and not one
inferred from the filesystem. Building with
`-DCY_PROJECT_MANIFEST=tools/project/fixtures/valid/project.json` takes the other path today.

## Layered typed configuration

Six layers, lowest first: `EngineDefault`, `Project`, `Platform`, `BuildConfiguration`, `User`,
`CommandLine`. Each overrides the previous.

```cpp
cy::config::ConfigStore store;
store.declare(schema);                      // typed, with a default, a range or an enumerator set
store.load_project_settings();              // the manifest's own values, and its platform override
store.apply_command_line(argc, argv);       // --renderer.profile=mobile
const auto resolved = store.resolve("renderer.profile").value();
// resolved.value, and resolved.layer — "where did this value come from"
```

Four decisions, each a place the obvious shortcut is wrong:

* **A setting must be declared before it can be set.** Writing an undeclared key is an error naming
  it, not a new entry. A configuration system that accepts anything cannot validate anything, cannot
  present anything in an editor, and cannot tell a typo from a feature.
* **The layer is part of the answer.** `resolve()` returns the value *and* the layer that supplied
  it; every layer's value is kept and resolution picks. A store that overwrote as it loaded could not
  answer "where did this value come from", which is a scenario the specification names.
* **The shipping policy is data.** Which layers a shipping build honours is a declared table, so a
  developer-local override cannot alter shipping behaviour and the rule is inspectable rather than
  scattered through the readers. `User` is excluded by default; a setting opts back in one at a time
  through `SettingSchema::user_override_in_shipping`. A refused layer is reported —
  `ResolvedSetting::suppressed_by_shipping_policy` — rather than silently skipped.
* **No allocation and no standard-library container.** A `ConfigStore` is a fixed array plus a text
  pool, so it is usable in the `Core` startup stage — which is precisely when the configuration that
  chooses an allocator is read.

Only **dotted** keys are settings on the command line, which is what separates
`--renderer.profile=mobile` from `--headless`. An *unknown* dotted key is an error: that is a typo
the user wants to hear about, not a switch for something else.

## Deterministic startup and shutdown over the module levels

`engine-architecture` fixes eleven startup stages, four of which are "modules at level `<L>`" —
`Core`, `Servers`, `Scene`, `Editor`. `ModuleRegistry` owns those four.

```cpp
cy::config::ModuleRegistry registry;
registry.add_project_modules();                       // descriptors, from the graph
registry.bind("my-module", &on_register, &on_unregister, &state);
runtime_config.modules = &registry;                   // the runtime drives the four stages
```

* **The order is recorded, not emergent.** `add()` inserts into an array sorted by (level, name), so
  the order does not depend on the order modules were added in, on a pointer value, or on a hash. The
  registry journals what it actually started and stopped, and `journal_is_reversed()` is the
  shutdown claim in one call, computed in every configuration rather than behind an assertion —
  `CY_ASSERT` is compiled out of `Profile` and `Shipping`, and an ordering claim that only held in a
  developer's build would be a claim about a developer's build.
* **Registration is an explicit call, never a static initialiser.** Static initialisation order is
  link order, and link order is exactly the non-determinism this requirement exists to remove. The
  reflection registry made the same decision for the same reason.
* **Levels come up in order and go down in reverse**, and the registry refuses anything else:
  `start()` requires every lower level to be running, `stop()` requires every higher level to have
  stopped.
* **A module that fails to register leaves its level as it found it** — the modules that call already
  registered are unregistered in reverse, and `Runtime::startup()` unwinds the stages below.

`src/runtime/probe/startup_order_probe.cpp --modules` prints the two journals, and
`src/runtime/tests/test_module_order.cpp` compares them across a hundred separate processes. Separate
processes, not a loop: a fresh address space each time is what would expose an order that depended on
an allocation address or a static initialiser, where a hundred iterations of one loop would reproduce
such an order faithfully and report it as deterministic.

## What is thinner than the capability

* **Plugins are declared, not loaded.** `project.h` carries a plugin's identifier, version, engine
  API range and enabled flag, and the graph refuses one whose range excludes this engine. The
  lifecycle `project-and-plugins` specifies — load, initialise, register, start, stop, unregister,
  shutdown, unload — needs a dynamic loader and the C ABI, which is M4. `ModuleRegistry`'s
  register/unregister pair is the shape those phases will take, one level at a time.
* **There is no lockfile.** Plugin resolution from version constraints into a committed lockfile is
  a requirement with no task in this milestone; the version-range grammar `graph.py` already parses
  is what resolution would be written against.
* **Extension points, trust tiers and hot reload** are declared vocabulary in the manifest and
  nothing more. They belong to the milestones that have something to extend.
* **Type ownership and unload safety** need the type registry to record an owning module.
  `cy::core-reflect`'s `TypeInfo` has a `module` field; nothing fills it yet.
