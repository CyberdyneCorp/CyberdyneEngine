# Add the editor agent interface: agents as first-class editor clients

## Why

An agent that can read a scene, place and manipulate objects, and *see the result in the viewport*
is the difference between a tool that answers questions about a project and one that can do work in
it. This is worth specifying now rather than later, because it is not an API to be bolted on — it is
a projection of a surface the editor already has, and it stays cheap only if it is designed as one.

`editor-rust-application` already requires that **every user-invokable action is a registered
command**, that menus, shortcuts, the palette, automation, tests and *"future assistive tooling"*
all invoke commands, and that an action reachable only through a specific widget is a defect. That
requirement was written before this proposal and it is what makes it tractable: if the command
registry is genuinely the single action surface, then an agent interface is a **projection of that
registry**, not a second implementation of it — and it cannot drift, because there is nothing to
drift from.

The alternative is what usually happens: an agent API grows beside the editor, covering the twenty
operations someone thought of, diverging in vocabulary and semantics, and quietly becoming a second
way to mutate a project that undo does not fully understand. Every one of those consequences is
already forbidden by an existing requirement. Writing this down now is mostly a matter of *not*
introducing exceptions.

The second reason is that the interesting half is not the mutation, it is the **observation**. An
agent that can place a light but cannot see that the scene is now blown out is doing data entry. The
editor already renders the viewport engine-side, already picks engine-side so that what is picked is
what was drawn, and already knows how to produce a capture with overlays excluded. An agent should
see the same image a human sees, through the same path, for the same reason the editor has no second
renderer.

## What Changes

- **New `editor-agent-interface` capability.** Agents are editor clients, not a privileged peer, and
  the protocol they speak is **MCP**.
- **Tools are generated from the command registry.** Every registered command is invocable, with its
  description, parameters and availability predicate derived from command metadata and reflection —
  so a command added for a menu is an agent tool the same day, and one that is renamed cannot leave
  a stale tool behind.
- **Resources are the read surface**: the scene graph, an entity's inspector properties, the current
  selection, the asset browser, diagnostics, and the build and play state.
- **The agent sees the viewport.** A rendered image from the engine's own renderer — the same path
  the human's viewport uses — with overlays and gizmos declared rather than assumed, and captures
  honouring the existing rule that an image representing the shipping frame excludes them.
- **Manipulation goes through the manipulation path.** An agent's move, rotate or scale is the same
  operation a gizmo drag performs: one transaction, the same snapping and constraint handling, the
  same numeric stability, operating on state captured at the start rather than accumulated.
- **Every agent mutation is a transaction**, so undo, autosave, crash recovery and semantic merge
  treat it exactly as they treat a human edit — and **every transaction records who made it**, which
  agent, in which session, and toward what stated intent.
- **Scope and trust.** A connection declares what it may touch; destructive and irreversible
  operations are gated; the human can revoke mid-session, and the editor stays usable throughout —
  a human action always wins a conflict.
- **An agent session is reproducible**, because it is a command log, which is the same reason replay
  and rollback are one mechanism in `gameplay-framework`.

## Capabilities

### New Capabilities

- `editor-agent-interface` — the projection of the command registry, the resource read surface,
  viewport observation, manipulation semantics, transactions and attribution, scope and trust,
  reproducibility, budget, generated discovery, and forbidden patterns.

### Modified Capabilities

- `editor-rust-application` — "Commands are the single action surface" replaces the placeholder
  *"future assistive tooling"* with the concrete requirement, and requires command metadata rich
  enough for **machine invocation** — typed parameters, a description written for a caller that
  cannot see the widget, and a declared effect class — because a registry that only has to satisfy a
  menu will not satisfy a caller that has never seen the menu.
- `editor-documents-and-transactions` — transactions carry **provenance**: the actor that produced
  them, human or agent, and for an agent the session and intent. Undo, the journal and semantic merge
  are unchanged in mechanism; they simply gain an attributable author.

## Impact

- **Roadmap**: `editor-agent-interface` reaches **Seed at M5** with the editor, **Working at M8**
  when there is a project worth driving, and **Complete at M11**. The command-metadata requirement
  binds from the **first command written**, for the usual reason — a registry filled in without it
  is a registry that has to be revisited entry by entry.
- **Dependencies**: an MCP implementation, integrated behind an engine-owned interface like every
  other protocol, so the wire format stays replaceable.
- **Risk**: the honest one is scope, not architecture. "Everything the editor can do" is a large
  surface, and the projection makes it large *automatically* — which is the point, and also means an
  agent can invoke a command nobody considered from an agent's perspective. The mitigations are the
  effect class on every command, the scope declaration on every connection, and the fact that every
  mutation is an ordinary undoable transaction.
