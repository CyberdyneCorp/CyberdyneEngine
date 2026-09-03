# Documentation

| | |
|---|---|
| [**Roadmap**](ROADMAP.md) | The order in which the specifications become code — twelve milestones, four eras, no dates |
| [Capability matrix](roadmap/capability-matrix.md) | Every capability against every milestone, with the maturity tier it reaches |
| [Dependencies](roadmap/dependencies.md) | The graphs the ordering follows, and the three cycles with their breaks |
| [Implementing the roadmap](roadmap/implementing.md) | How a milestone becomes OpenSpec changes, the rules every implementation change carries, and what is in flight |
| [Risks and deferrals](roadmap/risks.md) | The register, the spike at the head of each milestone, and where deferred scope re-enters |
| [`roadmap/status.yaml`](roadmap/status.yaml) | The authoritative per-capability status record |

**Design**

| | |
|---|---|
| [Editor visual language](design/editor-visual-language.md) | How the editor looks, what its colours mean, and what it calls things — with the reference imagery |

The engine itself is specified in [`openspec/specs/`](../openspec/specs/README.md) — 74 capabilities
stating what is being built and why. Those specifications are the contract; this directory explains
the order of construction and nothing more.

**Where things live**

| Question | Answer |
|---|---|
| What is being built, and why? | [`openspec/specs/`](../openspec/specs/README.md) |
| In what order, and what closes each step? | [`docs/ROADMAP.md`](ROADMAP.md) and [`delivery-roadmap`](../openspec/specs/delivery-roadmap/spec.md) |
| What is implemented today? | [`docs/roadmap/status.yaml`](roadmap/status.yaml), reported by `just roadmap::status` |
| What is being changed right now? | [`openspec/changes/`](../openspec/changes/) |
| How do I run anything? | `just` — every developer task is a recipe |
