## ADDED Requirements

### Requirement: The shared graph IR is proven against every consumer before consumers are built on it
The graph intermediate representation SHALL be validated against the stated semantic needs of every
capability that lowers through it — `visual-scripting`, `gameplay-abilities-and-effects`,
`ai-system`, `animation-and-skinning`, `sequencing-and-cinematics`, `vfx-system` and
`camera-system` — before any of them is implemented against it.

The validation SHALL be recorded: for each consumer, the semantics its specification requires, and
whether the IR expresses each one as written, expresses it with a specified extension, or cannot
express it.

A consumer that the IR cannot serve SHALL be recorded as a named exception with its own evaluator
behind the same authoring surface, and the exception SHALL narrow the "no graph is interpreted at
runtime" guarantee **by name** rather than silently.

#### Scenario: A consumer's semantics are not expressible
- **WHEN** a consumer requires a semantic the IR cannot express
- **THEN** the IR SHALL be extended by a specified change, or the consumer SHALL be recorded as a
  named exception, and every consumer already lowering through the IR SHALL be re-tested

#### Scenario: The premise is refuted
- **WHEN** two or more consumers require exceptions
- **THEN** the shared-IR premise SHALL be treated as refuted and the plan re-stated, rather than the
  remaining consumers being built on it

### Requirement: No graph is interpreted at runtime
An authored graph SHALL be compiled to a program before it is evaluated, and evaluation SHALL be
batched over instances rather than dispatched per entity per frame.

This SHALL be checked by a test that fails when a graph consumer performs a per-entity virtual call
in its evaluation path, rather than by review.

#### Scenario: A per-entity virtual tick is introduced
- **WHEN** a graph consumer evaluates instances through a per-entity virtual call
- **THEN** the check SHALL fail and name the consumer
