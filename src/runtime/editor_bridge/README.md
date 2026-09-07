# `src/runtime/editor_bridge` — the hosted runtime's side of the editor's control socket

What a `cyberdyne-editor --host <socket>` talks to. **M7 tasks 5b.3 and 5b.4.**

## Why it exists now

`editor/crates/cy-editor-protocol/` has carried this message set since M5, and until M7 the only
thing that ever answered it was `cy-runtime-stub` — a Rust binary in the editor's own workspace
whose module comment says what it is:

> What it is not: an engine. It holds no world, and it echoes what it is asked to apply. The real
> hosted runtime is a C++ binary … speaking this same message set, over this same committed
> encoding.

This is that C++ side. It is what lets `Message::GizmoIntent` reach `cy::render::build_gizmo_layout`
and `Message::GizmoGeometry` carry the answer back — the whole of task 5b.3, whose parts all existed
at M6 and none of which were joined: the protocol carried both messages, the editor's
`cy_editor_viewport::layout` hit-tested a published layout, and nothing in `src/` produced one, so a
click had nothing to land on.

## The framing

A four-byte little-endian length, a four-byte FNV-1a checksum, then the payload —
`cy_editor_protocol::frame`, restated. The checksum is not for corruption, of which a Unix domain
socket has none. It is for the case the peer **crashed mid-write**, which `editor-rust-application`
requires the editor to survive and which M5's artefact tests by killing a process: a length read out
of a half-written frame must be refused rather than believed.

## It never blocks, in either direction

`poll()` returns what has arrived and nothing else. A runtime that blocked reading the editor would
stop rendering because the editor was busy, which is the inversion the whole out-of-process design
exists to prevent — the same rule, in the other direction, that `cy_editor_protocol::Session` states
as "there is no blocking send".

A partial frame is *kept* rather than discarded: the reader accumulates and answers "nothing yet"
until a whole frame is there. Discarding would turn a message the kernel happened to split into a
protocol error.

## The defect this module shipped and then found

`EditorRequest::payload` was a span into the read buffer, and `poll` compacts that buffer once it
has taken a frame out of it — so a caller reading the payload after `poll` returned was reading the
**next** message. With two messages in one write, the second one's tag was decoded as the first
one's transaction and the runtime reported

> a transaction ends before its actor

which reads as a protocol version mismatch and is not one. The payload is now copied into a buffer
of its own before the compaction, and
`unit.editor_bridge/a_message_the_caller_reads_after_poll_is_still_its_own` is the regression.

## What is deliberately not here

**The image.** Frames travel over the viewport transport (`src/backends/viewport/`), on a different
socket, because they are pixels and this is control. The two are joined only by the frame identifier
they both carry.

**A world.** This module decodes and encodes; what a pick or a gizmo intent *means* is the host's,
because the host is what holds a scene. That is why `poll` hands back a request rather than calling
a handler: a bridge that owned the answers would be a bridge that owned the world.

**Transactions.** `Apply` is decoded and can be answered, but nothing here interprets the bytes:
that encoding is `cy_editor_core::codec`'s and belongs to whatever applies it. Live editing is M8's,
and this module is the wire it will arrive on.
