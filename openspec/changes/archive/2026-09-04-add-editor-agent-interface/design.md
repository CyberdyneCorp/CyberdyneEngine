# Design: the editor agent interface

## The decision that makes this small

An agent interface is normally a large piece of work because it is a second API: someone enumerates
the operations worth exposing, writes a schema for each, and maintains the mapping. It grows,
diverges from the interface it mirrors, and eventually becomes a second way to mutate a project that
undo only partly understands.

None of that is necessary here, because `editor-rust-application` already requires:

> Every user-invokable action SHALL be registered in a command registry… An action reachable only
> through a specific widget SHALL be a defect… Commands that modify project state SHALL execute
> through the transaction system.

If that requirement holds, an agent interface is a **projection**. The tools are the registry. The
mutations are transactions. The refusals are the availability predicates. There is nothing to
enumerate and nothing to keep in sync, and the work is mostly in resisting the temptation to add
exceptions.

That requirement even named this use case before it existed — the original text says commands serve
"automation, tests, and future assistive tooling". This change replaces the placeholder with the
concrete thing.

**The one real addition** is command metadata. A registry that only has to render a menu can get by
with a label and an icon. A caller that has never seen the menu needs typed parameters, a description
of what the operation *does*, and a declared effect class. That is why the metadata requirement binds
from the first command rather than at M8 — retrofitting it across an established registry is an
entry-by-entry migration, and this project has already paid that price once with reflection identity.

## Why the observation half matters more than the mutation half

An agent that can place a light but cannot see that the scene is now blown out is doing data entry
with extra steps. The interesting capability is the loop: change something, look, decide.

The editor is unusually well positioned for this. It already renders the viewport engine-side, it
already picks engine-side so what is picked is what was drawn, and `editor-viewport-and-gizmos`
already requires captures to declare whether overlays were included. So "the agent sees what the
human sees" is not a feature to build — it is a refusal to build a second path, which is the same
argument that keeps the editor from having a second renderer.

Two things follow that are worth stating explicitly:

- **Debug views are part of observation.** "Why is this dark" is answerable from a depth or normal
  buffer and very hard to answer from a colour image. Where the renderer already produces one, the
  agent can ask for it.
- **The image must say what it contains.** An agent evaluating lighting against an image with
  selection outlines burned in will draw wrong conclusions and be confident about them.

## Attribution, and what it is not

Every transaction records its actor. For an agent, that includes the session and the intent it was
working toward.

**This is not a security control**, and the specification says so in as many words, because
attribution that is mistaken for authorisation is worse than none — it invites someone to trust a
field that any client sets. The access control is the scope declaration and the effect class; the
attribution answers a different question, which is "who changed this and why", and which no history
in any engine currently answers.

The reason to record intent rather than only identity: a diff showing forty transforms changed is
unreadable. A diff showing forty transforms changed *toward "align the streetlights to the road
spline"* is reviewable in seconds. Intent is the compression.

## Scope, effect class, and why confirmation is rare

The obvious safety design is to confirm everything. It is also the wrong one: prompting for ordinary
work trains a human to approve without reading, which makes the one prompt that mattered
indistinguishable from the ninety that did not.

So the design puts the weight elsewhere:

| Mechanism | Covers |
|---|---|
| **Effect class** on every command | The agent knows the consequence before invoking |
| **Scope** on every connection | The agent cannot reach what it was not given |
| **Transactions** | Every reversible mistake is one undo away |
| **Confirmation** | Only what undo cannot reach |

Undo is the confirmation for everything reversible. Confirmation is reserved for what leaves the
transaction system — deleting from disk, overwriting source, publishing, calling an external service.

## Reproducibility, for the same reason as everywhere else

An agent session is recorded as the commands it invoked. That is deliberately the same shape as
`replay-and-rollback`'s one command log and `gameplay-framework`'s one command stream, and for the
same reason: a session that replays can be reviewed, bisected, turned into a regression test, and
attached to a defect report. An agent that did something strange is otherwise unfalsifiable.

## Where it sits on the roadmap

**Seed at M5**, when the editor and its command registry first exist. **Working at M8**, when there
is a project substantial enough that driving it is worth anything. **Complete at M11.**

The command-metadata requirement in `editor-rust-application` binds from M5's first command, and is
the only part of this change that cannot be deferred.

## Alternatives considered

**A dedicated agent API, hand-written.** Rejected — it is the second action surface `editor-ui-ux`
already forbids, and every consequence of that prohibition applies: divergent vocabulary, operations
undo does not fully cover, and a maintenance burden proportional to the editor's size.

**Driving the editor through synthetic input events.** Rejected as brittle and unattributable: it
produces gizmo drags rather than intents, cannot express "set this value exactly", and records
nothing useful in history. Synthetic input has a legitimate use in `input-and-actions` for testing
the input stack; it is the wrong layer for an agent.

**Exposing the C ABI directly to agents.** Rejected. The ABI is a language boundary, not an action
surface; it has no availability predicates, no transactions, and no undo. An agent on the ABI could
corrupt a document in ways the editor cannot represent, let alone reverse.

**Making attribution a permission system.** Rejected, as above. Two mechanisms that look alike and
answer different questions is how a security control gets built on a field that clients set.
