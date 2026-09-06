# Insert M5.5 — Operable: an editor you can see, and one an agent can drive

## Why

M5 delivered the editor as a client: documents, transactions, commands, picking, the live bridge,
and a scripted session that survives having its runtime killed. All of that is real and all of it is
tested. **None of it can be looked at.**

`cy-editor-interface` says so in its own header — docking, workspaces, the command palette,
keyboard-first operation and the generated inspector exist *"as models a test can drive, with no
window, no graphics device and no interface toolkit."* That is a defensible reading of
`editor-rust-application`, which makes the toolkit an implementation detail. It is not a defensible
reading of the roadmap, which claimed `editor-ui-ux` at **Working** in the same milestone.

**This is a defect in the ladder, not in the work.** `delivery-roadmap` requires that a milestone
end in an artefact exercising the capabilities it claims *"end to end, through the same entry points
a user would use."* A Python script driving command objects does not exercise docking or
keyboard-first operation through the entry points a user would use. The row was wrong when it was
written; M5's implementation satisfied the letter of it exactly.

The second reason is that the project is five milestones deep with **one screenshot**. Every serious
defect found so far — traces leaking build paths, a renderer whose sample drew black while exiting
zero, a descriptor bug that survived because every device suite rendered one frame — was an
integration failure that only appeared when something actually ran end to end. A subsystem verified
in isolation and never assembled is a subsystem whose integration is untested by construction, and
the editor is now the largest such subsystem in the tree.

The third reason is the one that changes what the engine is for. **An agent that can drive the
editor can author.** M5 built the command registry with typed parameters, machine-readable
descriptions and declared effect classes — the projection surface `editor-agent-interface` needs —
and M4 proved that a Swift module can be reloaded with live state intact. Put those together with a
viewport an agent can look at, and an agent can create a scene, place and manipulate objects, write
a gameplay script, reload it, and *see whether it worked*. That loop is worth far more at M5.5 than
at M8, because it is how the remaining six milestones get exercised.

## What Changes

- **The editor opens.** A window, a toolkit chosen and recorded, docked panels, the hierarchy, the
  inspector generated from reflection, the content browser, and the viewport showing the engine's
  own rendered image — not a second renderer, per `editor-viewport-and-gizmos`.
- **A person manipulates.** Select by clicking, and translate, rotate and scale with gizmos, at the
  latency M5's live-bridge spike measured. One transaction per manipulation, undo that restores
  exact values.
- **The agent interface reaches Working, not Seed.** Tools projected from the command registry,
  resources for the scene and selection, **viewport observation returning the engine-rendered
  image**, manipulation through the same path a gizmo drag uses, transactions with attribution, and
  scope with effect classes.
- **Agents author.** Concretely, and this is the capability the milestone is for: an agent creates
  entities and composes a scene; writes a Swift gameplay script into the project; triggers the build
  and reload M4 proved; enters play mode; and **looks at the result** to decide what to do next.
- **`editor-ui-ux` is corrected to Seed at M5** and Working at M5.5, because that is what the
  implementation is and the record should say so rather than the plan being right retroactively.

**Closing artefact**: two, because the milestone has two audiences.
`samples/05b-editor-window` — the editor opens on a project, a person selects an object, drags a
gizmo, undoes, and saves. `samples/05b-agent-authoring` — an agent connects over MCP, composes a
scene from an empty project, writes and reloads a gameplay script, and captures the viewport to
confirm what it built.

## Capabilities

### Advanced Capabilities

`editor-ui-ux` and `editor-agent-interface` to **Working**; `editor-visual-language` to **Working**;
`editor-viewport-and-gizmos` and `live-editing` remain Working with their interactive paths
exercised for the first time.

### Modified Capabilities

- `delivery-roadmap` — insert **M5.5 · Operable** between M5 and M6, and correct M5's row: it claimed
  `editor-ui-ux` at Working while closing on a scripted artefact that cannot exercise docking,
  workspaces, the palette or keyboard-first operation. The insertion is recorded rather than
  renumbering M6 through M11, so every existing reference stays valid.
- `editor-agent-interface` — reaches Working at M5.5 rather than M8. The reasoning changes with it:
  the interface was placed at M8 on the assumption that driving an editor is only worth anything once
  there is a substantial project to drive. That is wrong in a way worth writing down — the agent
  loop is most valuable *while the engine is being built*, because it is how the remaining milestones
  get exercised end to end by something other than a test.

## Impact

- **New code**: the editor's window and toolkit layer, the viewport's device path, the MCP transport,
  and two samples.
- **New dependency**: an interface toolkit, chosen in this change and recorded with its reasoning —
  the first time `editor-rust-application`'s "the toolkit is an implementation detail" is exercised
  rather than deferred. Plus an MCP implementation behind an engine-owned interface.
- **Roadmap**: M6 through M11 keep their numbers and their content. The ladder gains one entry.
- **Risk**: the toolkit choice is the one decision here that is expensive to reverse, and it is the
  one the specification deliberately left open. It gets a spike, and the spike's criterion is whether
  the viewport can present an engine-rendered image without a copy through the CPU.
