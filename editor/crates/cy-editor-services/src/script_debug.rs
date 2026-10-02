// SPDX-License-Identifier: MIT
//! The Play debugger and hot reload, on the wire: the requests the editor sends to the engine's
//! `script.debug.*` and `script.reload` operations and the replies it reads back. Issues #84 and #29.
//!
//! The engine owns the debugger. Breakpoints stop the engine's compiled program at a node (it runs
//! a copy with a probe at every node boundary; see `src/graph/include/cy/graph/script_debug.h`),
//! and when a graph stops the WHOLE simulation tick stops with it. The editor only asks: set a
//! breakpoint, pause, continue, step into or over, read the variables and pins of one instance, and
//! read the trace of the nodes that ran, which the panel draws as execution highlighting. The
//! payloads are specified in `src/editor_backend/include/cy/editor/script_service.h`; the engine's
//! suite writes the replies decoded here into `src/editor_backend/tests/data/`.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

use crate::script_graph::{
    CompileDiagnostic, WIRE_FORMAT, expect_format, finished, read_diagnostics,
};

/// Read the debug state, inspecting one instance and its watched pins.
pub const DEBUG_GET: &str = "script.debug.get";
/// Add or remove a breakpoint.
pub const DEBUG_BREAKPOINT: &str = "script.debug.breakpoint";
/// Pause, continue or step.
pub const DEBUG_CONTROL: &str = "script.debug.control";
/// Recompile a running graph and swap it in at the next tick.
pub const RELOAD: &str = "script.reload";

/// What a debugger control asks of Play.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DebugAction {
    /// Break at the next node any graph runs.
    Pause,
    /// Run on to the next breakpoint.
    Continue,
    /// Run the paused instance to its very next node, data nodes included.
    StepInto,
    /// Run the paused instance to its next node on an execution chain.
    StepOver,
}

impl DebugAction {
    /// The number the engine reads.
    #[must_use]
    pub const fn code(self) -> u8 {
        match self {
            Self::Pause => 0,
            Self::Continue => 1,
            Self::StepInto => 2,
            Self::StepOver => 3,
        }
    }

    /// The action a step `mode` names: `into` or `over`.
    ///
    /// # Errors
    ///
    /// Any other word.
    pub fn step(mode: &str) -> Result<Self> {
        match mode {
            "into" => Ok(Self::StepInto),
            "" | "over" => Ok(Self::StepOver),
            other => Err(Problem::new(
                format!("step {other:?}"),
                "a step goes `into` the next node or `over` to the next execution node",
            )),
        }
    }
}

/// A breakpoint as the editor keeps it: a graph (its file's stem), a node, and the entity it stops
/// for by engine identity, zero for every instance.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct Breakpoint {
    /// The graph's name: its `.cyscript` file's stem.
    pub graph: String,
    /// The node's key.
    pub node: u64,
    /// The entity's engine identity, or zero for every instance.
    pub entity: u64,
}

/// Which instance the debug state inspects, and which of its pins to read.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct WatchList {
    /// The entity's engine identity; zero inspects the paused instance.
    pub entity: u64,
    /// The graph, or empty for the entity's first.
    pub graph: String,
    /// `(node, pin)` pairs, read in this order.
    pub pins: Vec<(u64, String)>,
}

/// The `script.debug.get` request for `watch`.
#[must_use]
pub fn debug_get_payload(watch: &WatchList) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.u64(watch.entity);
    out.text(&watch.graph);
    out.u32(u32::try_from(watch.pins.len()).unwrap_or(u32::MAX));
    for (node, pin) in &watch.pins {
        out.u64(*node);
        out.text(pin);
    }
    out.finish()
}

/// The `script.debug.breakpoint` request.
#[must_use]
pub fn breakpoint_payload(breakpoint: &Breakpoint, enabled: bool) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.text(&breakpoint.graph);
    out.u64(breakpoint.node);
    out.u64(breakpoint.entity);
    out.u8(u8::from(enabled));
    out.finish()
}

/// The `script.debug.control` request.
#[must_use]
pub fn control_payload(action: DebugAction) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.u8(action.code());
    out.finish()
}

/// The `script.reload` request: the graph's project-relative reference and its saved text.
#[must_use]
pub fn reload_payload(reference: &str, source: &str) -> Vec<u8> {
    let mut out = Writer::new();
    out.u32(WIRE_FORMAT);
    out.text(reference);
    out.text(source);
    out.finish()
}

/// A value read out of a running instance, typed by the engine's `ValueKind`.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct DebugValue {
    /// `cy::graph::script::ValueKind`: 1 bool, 2 int, 3 float, and the rest.
    pub kind: u8,
    /// The value as a float.
    pub x: f32,
    /// The value as a whole number.
    pub integer: i64,
}

impl DebugValue {
    const BOOL: u8 = 1;
    const INT: u8 = 2;
    const TICK: u8 = 13;

    fn read(reader: &mut Reader<'_>) -> Result<Self> {
        Ok(Self {
            kind: reader.u8()?,
            x: reader.f32()?,
            integer: reader.i64()?,
        })
    }

    /// The value as a person reads it, at its kind.
    #[must_use]
    pub fn display(&self) -> String {
        match self.kind {
            Self::BOOL => (self.integer != 0).to_string(),
            Self::INT | Self::TICK => self.integer.to_string(),
            _ => crate::script_graph::format_float(self.x),
        }
    }
}

/// One node an instance ran.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct TraceEntry {
    /// Monotonic across the session.
    pub sequence: u64,
    /// The tick it ran in.
    pub tick: u64,
    /// The entity's engine identity.
    pub entity: u64,
    /// The graph.
    pub graph: String,
    /// The node.
    pub node: u64,
}

/// One graph variable of the inspected instance.
#[derive(Clone, PartialEq, Debug)]
pub struct DebugVariable {
    /// The declaring node: its identity across reloads.
    pub id: u64,
    /// Its name.
    pub name: String,
    /// Its value.
    pub value: DebugValue,
}

/// One watched pin of the inspected instance.
#[derive(Clone, PartialEq, Debug)]
pub struct WatchValue {
    /// The node.
    pub node: u64,
    /// The pin.
    pub pin: String,
    /// Whether the program has a register for that pin.
    pub found: bool,
    /// What it holds.
    pub value: DebugValue,
}

/// What the last applied reload did.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct ReloadReport {
    /// The graph, or empty when nothing has been reloaded.
    pub graph: String,
    /// Its program generation now.
    pub generation: u32,
    /// Instances moved to the new program.
    pub instances: u32,
    /// Variables carried over, by declaring node.
    pub kept: u32,
    /// Variables that started at their default.
    pub added: u32,
    /// Variables the new program no longer declares.
    pub dropped: u32,
    /// Waits in progress that go on in the new program.
    pub waits_kept: u32,
    /// Waits the new program could not continue.
    pub waits_dropped: u32,
}

/// The Play debugger's state, as the engine reports it.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct DebugState {
    /// Whether Play is running graphs.
    pub playing: bool,
    /// Whether they run their debug-instrumented programs.
    pub debugging: bool,
    /// Whether a graph is holding the simulation at a node.
    pub paused: bool,
    /// Why: `breakpoint`, `step` or `pause`.
    pub reason: &'static str,
    /// The paused entity's engine identity.
    pub paused_entity: u64,
    /// The paused graph.
    pub paused_graph: String,
    /// The node it is paused before.
    pub paused_node: u64,
    /// The tick the break happened in.
    pub paused_tick: u64,
    /// The graph system's tick.
    pub tick: u64,
    /// The breakpoints the engine holds.
    pub breakpoints: Vec<Breakpoint>,
    /// The nodes run, oldest first, at most the engine's capacity.
    pub trace: Vec<TraceEntry>,
    /// The inspected instance's entity, zero for none.
    pub inspected_entity: u64,
    /// Its graph.
    pub inspected_graph: String,
    /// Its variables.
    pub variables: Vec<DebugVariable>,
    /// Its watched pins, in the requested order.
    pub watches: Vec<WatchValue>,
    /// The last reload applied.
    pub last_reload: ReloadReport,
}

impl DebugState {
    /// Decode a `script.debug.get`, `.breakpoint` or `.control` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, or bytes left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        const ACTION: &str = "read the engine's graph debugger";
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, ACTION)?;
        let mut state = Self {
            playing: reader.u8()? != 0,
            debugging: reader.u8()? != 0,
            paused: reader.u8()? != 0,
            reason: match reader.u8()? {
                0 => "breakpoint",
                1 => "step",
                _ => "pause",
            },
            paused_entity: reader.u64()?,
            paused_graph: reader.text()?,
            paused_node: reader.u64()?,
            paused_tick: reader.u64()?,
            tick: reader.u64()?,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            state.breakpoints.push(Breakpoint {
                graph: reader.text()?,
                node: reader.u64()?,
                entity: reader.u64()?,
            });
        }
        for _ in 0..reader.u32()? {
            state.trace.push(TraceEntry {
                sequence: reader.u64()?,
                tick: reader.u64()?,
                entity: reader.u64()?,
                graph: reader.text()?,
                node: reader.u64()?,
            });
        }
        state.inspected_entity = reader.u64()?;
        state.inspected_graph = reader.text()?;
        for _ in 0..reader.u32()? {
            state.variables.push(DebugVariable {
                id: reader.u64()?,
                name: reader.text()?,
                value: DebugValue::read(&mut reader)?,
            });
        }
        for _ in 0..reader.u32()? {
            state.watches.push(WatchValue {
                node: reader.u64()?,
                pin: reader.text()?,
                found: reader.u8()? != 0,
                value: DebugValue::read(&mut reader)?,
            });
        }
        state.last_reload = ReloadReport {
            graph: reader.text()?,
            generation: reader.u32()?,
            instances: reader.u32()?,
            kept: reader.u32()?,
            added: reader.u32()?,
            dropped: reader.u32()?,
            waits_kept: reader.u32()?,
            waits_dropped: reader.u32()?,
        };
        finished(&reader, ACTION)?;
        Ok(state)
    }

    /// The last `count` distinct nodes of `graph` the trace says ran, newest first, each with how
    /// recently: 1 for the newest, falling towards 0. What the canvas highlights.
    #[must_use]
    pub fn recent_nodes(&self, graph: &str, count: usize) -> Vec<(u64, f32)> {
        let mut recent: Vec<u64> = Vec::new();
        for entry in self.trace.iter().rev() {
            if entry.graph == graph && !recent.contains(&entry.node) {
                recent.push(entry.node);
                if recent.len() == count {
                    break;
                }
            }
        }
        let span = recent.len().max(1) as f32;
        recent
            .into_iter()
            .enumerate()
            .map(|(age, node)| (node, 1.0 - age as f32 / span))
            .collect()
    }

    /// Whether the paused node is `node` of `graph`.
    #[must_use]
    pub fn paused_at(&self, graph: &str, node: u64) -> bool {
        self.paused && self.paused_graph == graph && self.paused_node == node
    }
}

/// The engine's answer to a `script.reload`.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct ReloadReply {
    /// Whether the new program was staged for the next tick.
    pub accepted: bool,
    /// The generation it will have; zero when refused.
    pub generation: u32,
    /// Why it was refused, node by node, or what the compiler warned about.
    pub diagnostics: Vec<CompileDiagnostic>,
}

impl ReloadReply {
    /// Decode a `script.reload` reply.
    ///
    /// # Errors
    ///
    /// Another format, a truncated reply, or bytes left over.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        const ACTION: &str = "read the engine's reload";
        let mut reader = Reader::new(payload);
        expect_format(&mut reader, ACTION)?;
        let reply = Self {
            accepted: reader.u8()? != 0,
            generation: reader.u32()?,
            diagnostics: read_diagnostics(&mut reader)?,
        };
        finished(&reader, ACTION)?;
        Ok(reply)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::script_graph::tests::engine_fixture;

    /// The identity the engine's suite gives its one unit.
    const UNIT: u64 = 0x0123_4567_89AB_CDEF;

    #[test]
    fn the_engines_paused_state_decodes_with_its_variable_and_watches() {
        let state = DebugState::decode(&engine_fixture("script_debug_paused_v1.wire")).unwrap();
        assert!(state.playing && state.debugging && state.paused);
        assert_eq!(state.reason, "breakpoint");
        assert_eq!(state.paused_entity, UNIT);
        assert_eq!(state.paused_graph, "unit_counter");
        assert_eq!(state.paused_node, 11);
        assert!(state.paused_at("unit_counter", 11));
        assert!(!state.paused_at("unit_counter", 4));
        assert_eq!(
            state.breakpoints,
            vec![Breakpoint {
                graph: "unit_counter".into(),
                node: 11,
                entity: UNIT
            }]
        );
        assert_eq!(state.inspected_entity, UNIT);
        assert_eq!(state.variables.len(), 1);
        assert_eq!(state.variables[0].name, "orders");
        assert_eq!(state.variables[0].id, 7);
        assert_eq!(state.variables[0].value.display(), "0");
        assert_eq!(state.watches.len(), 2);
        assert_eq!(
            (state.watches[0].node, state.watches[0].pin.as_str()),
            (10, "value")
        );
        assert!(state.watches[0].found);
        assert_eq!(state.watches[0].value.display(), "1");
        // The trace ends at the node it is paused before, and that node is the newest highlight.
        assert_eq!(state.trace.last().map(|entry| entry.node), Some(11));
        let recent = state.recent_nodes("unit_counter", 4);
        assert_eq!(recent.first(), Some(&(11, 1.0)));
        assert!(recent.windows(2).all(|pair| pair[0].1 > pair[1].1));
    }

    #[test]
    fn the_engines_reload_replies_decode() {
        let accepted = ReloadReply::decode(&engine_fixture("script_reload_v1.wire")).unwrap();
        assert!(accepted.accepted);
        assert_eq!(accepted.generation, 2);
        assert!(accepted.diagnostics.is_empty());
        let refused =
            ReloadReply::decode(&engine_fixture("script_reload_refused_v1.wire")).unwrap();
        assert!(!refused.accepted);
        assert_eq!(refused.diagnostics.len(), 1);
        assert_eq!(refused.diagnostics[0].code, "script.reload.type");
        assert_eq!(refused.diagnostics[0].node, 7);
        assert_eq!(refused.diagnostics[0].detail, "int -> bool");
    }

    #[test]
    fn requests_are_the_bytes_the_engine_reads() {
        let mut expected = Writer::new();
        expected.u32(1);
        expected.u64(UNIT);
        expected.text("unit_counter");
        expected.u32(1);
        expected.u64(10);
        expected.text("value");
        assert_eq!(
            debug_get_payload(&WatchList {
                entity: UNIT,
                graph: "unit_counter".into(),
                pins: vec![(10, "value".into())],
            }),
            expected.finish()
        );
        assert_eq!(control_payload(DebugAction::StepOver), vec![1, 0, 0, 0, 3]);
        assert_eq!(DebugAction::step("into").unwrap(), DebugAction::StepInto);
        assert!(DebugAction::step("out").is_err());
        let breakpoint = breakpoint_payload(
            &Breakpoint {
                graph: "g".into(),
                node: 4,
                entity: 0,
            },
            false,
        );
        assert_eq!(breakpoint.last(), Some(&0));
        assert_eq!(breakpoint.len(), 4 + 4 + 1 + 8 + 8 + 1);
    }

    #[test]
    fn a_value_is_shown_at_its_kind() {
        let at = |kind, x, integer| DebugValue { kind, x, integer }.display();
        assert_eq!(at(1, 0.0, 1), "true");
        assert_eq!(at(2, 0.0, -3), "-3");
        assert_eq!(at(3, 2.5, 0), "2.5");
    }
}
