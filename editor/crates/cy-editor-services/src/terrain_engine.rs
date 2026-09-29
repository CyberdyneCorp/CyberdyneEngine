// SPDX-License-Identifier: MIT
//! The engine's answer to the terrain the editor authors.
//!
//! The editor is a client of the engine's terrain module: it records strokes as modifiers
//! ([`crate::terrain`]) and never computes a height. After every change to the edited terrain's
//! stack — a stroke, an undo, a redo, an enable, a reorder — [`TerrainEngine::maintain`] sends the
//! whole ordered stack as `terrain.evaluate` and keeps the reply: the heights, texels and holes the
//! engine evaluated, what its meshing and collision left open, and the regions whose navigation the
//! engine has flagged stale. Undo therefore needs no inverse on the wire: the document's history
//! restores the stack and the same stack evaluates to the same bytes.
//!
//! The payload formats are the engine's, stated once in `src/editor_backend/include/cy/editor/
//! terrain_service.h`.

use cy_editor_commands::Outcome;
use cy_editor_core::brush::Stroke;
use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_documents::Document;
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::runtime::RuntimeSession;
use crate::terrain::{TOOLS, TerrainStack};

/// The service operation and the format of its request and reply.
pub const TERRAIN_EVALUATE_OPERATION: &str = "terrain.evaluate";
/// The payload format both sides speak.
pub const TERRAIN_EVALUATE_FORMAT: u32 = 1;
/// Metres along each edge of the authored terrain. The painting surface's zero-to-one coordinates
/// span this.
pub const TERRAIN_EXTENT_METRES: f32 = 128.0;
/// Engine tiles along each edge: 2 x 64 quads, one metre between samples.
pub const TERRAIN_TILES: u32 = 2;
/// The unedited ground's height, in metres.
pub const TERRAIN_BASE_HEIGHT: f32 = 0.0;
const SERVICE_SCHEMA_VERSION: u32 = 1;

/// The `terrain.evaluate` request for one terrain root's current stack, or `None` when the node
/// is not a terrain.
///
/// Every modifier travels, disabled ones included, in stack order; a paint stroke names its layer
/// by order (the first layer is 1, 0 the unpainted base). A stroke whose payload no longer decodes
/// is refused rather than skipped, because skipping it would show ground the document does not
/// describe.
pub fn evaluation_request(document: &Document, terrain: NodeId) -> Option<Result<Vec<u8>>> {
    let stack = TerrainStack::read(document, terrain)?;
    Some(encode_request(terrain, &stack))
}

fn encode_request(terrain: NodeId, stack: &TerrainStack) -> Result<Vec<u8>> {
    let mut writer = Writer::new();
    writer.u32(TERRAIN_EVALUATE_FORMAT);
    writer.u128(terrain.as_u128());
    writer.u32(TERRAIN_TILES);
    writer.f32(TERRAIN_EXTENT_METRES);
    writer.f32(TERRAIN_BASE_HEIGHT);
    writer.u32(u32::try_from(stack.modifiers.len()).unwrap_or(u32::MAX));
    for modifier in &stack.modifiers {
        let op = TOOLS
            .iter()
            .position(|tool| *tool == modifier.kind)
            .ok_or_else(|| {
                Problem::new(
                    "send the terrain to the engine",
                    format!(
                        "modifier {} has unknown tool {:?}",
                        modifier.id, modifier.kind
                    ),
                )
            })?;
        let stroke = Stroke::decode(&modifier.stroke)?;
        let layer = modifier
            .layer
            .and_then(|layer| stack.layers.iter().position(|known| known.id == layer))
            .map_or(0, |order| u8::try_from(order + 1).unwrap_or(u8::MAX));
        writer.u128(modifier.id.as_u128());
        writer.u8(u8::try_from(op).unwrap_or(u8::MAX));
        writer.u8(u8::from(modifier.enabled));
        writer.u8(layer);
        writer.f32(stroke.brush.radius);
        writer.f32(stroke.brush.strength);
        writer.f32(stroke.brush.falloff);
        writer.u32(u32::try_from(stroke.samples.len()).unwrap_or(u32::MAX));
        for sample in &stroke.samples {
            writer.f32(sample.x);
            writer.f32(sample.y);
            writer.f32(sample.pressure);
        }
    }
    Ok(writer.finish())
}

/// One region of the terrain whose navigation the engine flagged stale, in metres.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct StaleRegion {
    /// Minimum corner along x.
    pub min_x: f32,
    /// Minimum corner along z.
    pub min_z: f32,
    /// Maximum corner along x.
    pub max_x: f32,
    /// Maximum corner along z.
    pub max_z: f32,
}

/// What the engine evaluated for the edited terrain.
#[derive(Clone, PartialEq, Debug)]
pub struct TerrainEvaluation {
    /// Increments with every evaluation the engine session made.
    pub generation: u64,
    /// Samples along each edge.
    pub edge: u32,
    /// Metres along each edge.
    pub extent: f32,
    /// The range stored heights are quantised over.
    pub height_min: f32,
    /// See `height_min`.
    pub height_max: f32,
    /// Triangles the engine's meshing emitted for rendering.
    pub rendered_triangles: u32,
    /// Quads rendering left open because they are holes.
    pub rendered_hole_quads: u32,
    /// Collision samples the engine marked as holes.
    pub collision_holes: u32,
    /// Regions whose navigation is stale until it is rebaked.
    pub stale: Vec<StaleRegion>,
    /// `edge * edge` stored heights, rows along x.
    pub heights: Vec<u16>,
    /// `(edge - 1)^2` texels: four layers then four weights.
    pub texels: Vec<[u8; 8]>,
    /// `(edge - 1)^2` flags: one where the surface is cut away.
    pub holes: Vec<u8>,
}

impl TerrainEvaluation {
    /// Decode the engine's reply, refusing a truncated or inconsistent one.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let format = reader.u32()?;
        if format != TERRAIN_EVALUATE_FORMAT {
            return Err(Problem::new(
                "read the engine's terrain",
                format!("reply format {format} is unsupported"),
            ));
        }
        let generation = reader.u64()?;
        let edge = reader.u32()?;
        if !(2..=1025).contains(&edge) {
            return Err(Problem::new(
                "read the engine's terrain",
                format!("an edge of {edge} samples is outside what the engine evaluates"),
            ));
        }
        let extent = reader.f32()?;
        let height_min = reader.f32()?;
        let height_max = reader.f32()?;
        let rendered_triangles = reader.u32()?;
        let rendered_hole_quads = reader.u32()?;
        let collision_holes = reader.u32()?;
        let stale_count = reader.u32()? as usize;
        if stale_count > 4096 {
            return Err(Problem::new(
                "read the engine's terrain",
                "the stale navigation list is unbounded",
            ));
        }
        let mut stale = Vec::with_capacity(stale_count);
        for _ in 0..stale_count {
            stale.push(StaleRegion {
                min_x: reader.f32()?,
                min_z: reader.f32()?,
                max_x: reader.f32()?,
                max_z: reader.f32()?,
            });
        }
        let samples = (edge as usize) * (edge as usize);
        let quads = ((edge - 1) as usize) * ((edge - 1) as usize);
        let mut heights = Vec::with_capacity(samples);
        for _ in 0..samples {
            heights.push(u16::from(reader.u8()?) | (u16::from(reader.u8()?) << 8));
        }
        let mut texels = Vec::with_capacity(quads);
        for _ in 0..quads {
            let mut texel = [0_u8; 8];
            for byte in &mut texel {
                *byte = reader.u8()?;
            }
            texels.push(texel);
        }
        let mut holes = Vec::with_capacity(quads);
        for _ in 0..quads {
            holes.push(reader.u8()?);
        }
        if !reader.is_empty() {
            return Err(Problem::new(
                "read the engine's terrain",
                format!("{} trailing bytes remain", reader.remaining()),
            ));
        }
        Ok(Self {
            generation,
            edge,
            extent,
            height_min,
            height_max,
            rendered_triangles,
            rendered_hole_quads,
            collision_holes,
            stale,
            heights,
            texels,
            holes,
        })
    }

    /// The height at a sample, in metres.
    #[must_use]
    pub fn height(&self, x: u32, z: u32) -> f32 {
        let stored = self.heights[(z as usize) * (self.edge as usize) + x as usize];
        self.height_min + (f32::from(stored) / 65535.0) * (self.height_max - self.height_min)
    }

    /// FNV-1a over the stored heights: equal digests are byte-identical heights for any agent
    /// that compares two evaluations without receiving the lattice.
    #[must_use]
    pub fn heights_digest(&self) -> u64 {
        fnv(self.heights.iter().flat_map(|height| height.to_le_bytes()))
    }

    /// FNV-1a over the texels and holes.
    #[must_use]
    pub fn weights_digest(&self) -> u64 {
        fnv(self
            .texels
            .iter()
            .flat_map(|texel| texel.iter().copied())
            .chain(self.holes.iter().copied()))
    }

    /// Quads cut away.
    #[must_use]
    pub fn hole_count(&self) -> usize {
        self.holes.iter().filter(|hole| **hole != 0).count()
    }
}

fn fnv(bytes: impl Iterator<Item = u8>) -> u64 {
    bytes.fold(0xCBF2_9CE4_8422_2325_u64, |hash, byte| {
        (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01B3)
    })
}

/// The editor's side of `terrain.evaluate`: one request in flight, the last stack sent, and the
/// last answer.
#[derive(Default)]
pub struct TerrainEngine {
    request: Option<RequestId>,
    sent: Option<Vec<u8>>,
    terrain: Option<NodeId>,
    evaluation: Option<TerrainEvaluation>,
    problem: Option<String>,
}

impl TerrainEngine {
    /// An engine view that has evaluated nothing.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Send `wanted` — the edited terrain's current request — when it differs from what the engine
    /// last received and nothing is in flight. One request at a time: a stack that changes while
    /// the engine evaluates is sent when the answer arrives, so a burst of edits costs one
    /// evaluation per round trip rather than one per edit.
    pub fn maintain(
        &mut self,
        runtime: &RuntimeSession,
        wanted: Option<(NodeId, Result<Vec<u8>>)>,
    ) -> Option<Problem> {
        if !runtime.is_connected() {
            self.request = None;
            self.sent = None;
            return None;
        }
        let (terrain, payload) = wanted?;
        let payload = match payload {
            Ok(payload) => payload,
            Err(problem) => {
                self.problem = Some(problem.to_string());
                return None;
            }
        };
        if self.request.is_some() || self.sent.as_deref() == Some(payload.as_slice()) {
            return None;
        }
        match runtime.service_request(
            SERVICE_SCHEMA_VERSION,
            TERRAIN_EVALUATE_OPERATION,
            payload.clone(),
        ) {
            Ok(request) => {
                self.request = Some(request);
                self.sent = Some(payload);
                if self.terrain != Some(terrain) {
                    self.evaluation = None;
                }
                self.terrain = Some(terrain);
                None
            }
            Err(problem) => Some(problem),
        }
    }

    /// Take the engine's answer to the request in flight. Anything else is not this service's.
    pub fn accept(&mut self, message: &Message) -> Option<Problem> {
        let Message::ServiceEvent {
            request,
            kind,
            schema_version,
            payload,
        } = message
        else {
            return None;
        };
        if self.request != Some(*request) {
            return None;
        }
        let result = match kind {
            ServiceEventKind::Accepted | ServiceEventKind::Progress => return None,
            ServiceEventKind::Completed if *schema_version == SERVICE_SCHEMA_VERSION => {
                TerrainEvaluation::decode(payload)
            }
            ServiceEventKind::Completed => Err(Problem::new(
                "read the engine's terrain",
                format!("schema {schema_version} is unsupported"),
            )),
            ServiceEventKind::Failed => Err(failure(payload)),
            ServiceEventKind::Cancelled => Err(Problem::new(
                "evaluate the terrain",
                "the engine cancelled the evaluation",
            )),
        };
        self.request = None;
        match result {
            Ok(evaluation) => {
                self.evaluation = Some(evaluation);
                self.problem = None;
                None
            }
            Err(problem) => {
                // A refused stack is not resent until it changes; the refusal says why.
                self.problem = Some(problem.to_string());
                Some(problem)
            }
        }
    }

    /// The engine's last evaluation of the edited terrain.
    #[must_use]
    pub fn evaluation(&self) -> Option<&TerrainEvaluation> {
        self.evaluation.as_ref()
    }

    /// Navigation was rebaked, and the engine consumed the regions its terrain flagged stale with
    /// it: the navmesh's one stale flag is cleared here too, so `terrain.status` and the Navigation
    /// panel agree without waiting for the next evaluation.
    pub fn navigation_rebaked(&mut self) {
        if let Some(evaluation) = self.evaluation.as_mut() {
            evaluation.stale.clear();
        }
    }

    /// Regions whose navigation the engine last flagged stale; empty before any evaluation.
    #[must_use]
    pub fn stale_navigation(&self) -> &[StaleRegion] {
        self.evaluation
            .as_ref()
            .map_or(&[], |evaluation| evaluation.stale.as_slice())
    }

    /// The terrain root the evaluation belongs to.
    #[must_use]
    pub const fn terrain(&self) -> Option<NodeId> {
        self.terrain
    }

    /// Whether an evaluation is in flight.
    #[must_use]
    pub const fn pending(&self) -> bool {
        self.request.is_some()
    }

    /// The last refusal, if the engine refused the current stack.
    #[must_use]
    pub fn problem(&self) -> Option<&str> {
        self.problem.as_deref()
    }

    /// Whether the evaluation shown is of the stack the document now holds.
    #[must_use]
    pub fn is_current(&self, wanted: Option<&[u8]>) -> bool {
        self.request.is_none() && self.evaluation.is_some() && self.sent.as_deref() == wanted
    }

    /// The `terrain.status` answer.
    #[must_use]
    pub fn status(&self) -> Outcome {
        let mut outcome = match &self.evaluation {
            Some(evaluation) => Outcome::new(format!(
                "Engine terrain generation {}: {} triangles, {} holes, navigation stale in {} regions",
                evaluation.generation,
                evaluation.rendered_triangles,
                evaluation.hole_count(),
                evaluation.stale.len()
            ))
            .with("evaluated", Value::Bool(true))
            .with("generation", Value::Text(evaluation.generation.to_string()))
            .with("edge", Value::Int(i64::from(evaluation.edge)))
            .with("extent", Value::Float(evaluation.extent))
            .with(
                "heights_digest",
                Value::Text(format!("{:016x}", evaluation.heights_digest())),
            )
            .with(
                "weights_digest",
                Value::Text(format!("{:016x}", evaluation.weights_digest())),
            )
            .with(
                "rendered_triangles",
                Value::Int(i64::from(evaluation.rendered_triangles)),
            )
            .with(
                "rendered_hole_quads",
                Value::Int(i64::from(evaluation.rendered_hole_quads)),
            )
            .with(
                "collision_holes",
                Value::Int(i64::from(evaluation.collision_holes)),
            )
            .with(
                "navigation_stale",
                Value::Bool(!evaluation.stale.is_empty()),
            )
            .with(
                "navigation_stale_regions",
                Value::Text(
                    evaluation
                        .stale
                        .iter()
                        .map(|region| {
                            format!(
                                "{} {} {} {}",
                                region.min_x, region.min_z, region.max_x, region.max_z
                            )
                        })
                        .collect::<Vec<_>>()
                        .join("; "),
                ),
            ),
            None => Outcome::new("No terrain has been evaluated by the engine")
                .with("evaluated", Value::Bool(false)),
        };
        outcome = outcome.with("pending", Value::Bool(self.pending()));
        if let Some(terrain) = self.terrain {
            outcome = outcome.with("terrain", Value::Text(terrain.to_string()));
        }
        if let Some(problem) = &self.problem {
            outcome = outcome.with("problem", Value::Text(problem.clone()));
        }
        outcome
    }
}

/// The engine's failure diagnostic: a count, then a code and a message per entry.
fn failure(payload: &[u8]) -> Problem {
    let mut reader = Reader::new(payload);
    let decoded = (|| -> Result<(String, String)> {
        let _count = reader.u32()?;
        Ok((reader.text()?, reader.text()?))
    })();
    match decoded {
        Ok((code, message)) => Problem::new("evaluate the terrain", format!("{code}: {message}")),
        Err(_) => Problem::new(
            "evaluate the terrain",
            "the engine returned an unreadable failure",
        ),
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// An engine reply with `stale` regions, for the suites that drive the terrain's engine side.
    pub(crate) fn reply(edge: u32, stale: &[[f32; 4]], height: u16, holes: &[usize]) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u32(TERRAIN_EVALUATE_FORMAT);
        writer.u64(3);
        writer.u32(edge);
        writer.f32(128.0);
        writer.f32(-512.0);
        writer.f32(1536.0);
        writer.u32(100);
        writer.u32(u32::try_from(holes.len()).unwrap());
        writer.u32(7);
        writer.u32(u32::try_from(stale.len()).unwrap());
        for region in stale {
            for value in region {
                writer.f32(*value);
            }
        }
        for _ in 0..edge * edge {
            writer.u8((height & 0xFF) as u8);
            writer.u8((height >> 8) as u8);
        }
        let quads = ((edge - 1) * (edge - 1)) as usize;
        for _ in 0..quads {
            for byte in [0_u8, 0, 0, 0, 255, 0, 0, 0] {
                writer.u8(byte);
            }
        }
        for quad in 0..quads {
            writer.u8(u8::from(holes.contains(&quad)));
        }
        writer.finish()
    }

    #[test]
    fn an_engine_reply_decodes_to_its_lattice_holes_and_stale_regions() {
        let evaluation =
            TerrainEvaluation::decode(&reply(3, &[[1.0, 2.0, 3.0, 4.0]], 16384, &[1])).unwrap();
        assert_eq!(evaluation.generation, 3);
        assert_eq!(evaluation.edge, 3);
        assert_eq!(evaluation.heights.len(), 9);
        assert_eq!(evaluation.texels.len(), 4);
        assert_eq!(evaluation.hole_count(), 1);
        assert_eq!(evaluation.collision_holes, 7);
        assert_eq!(
            evaluation.stale,
            [StaleRegion {
                min_x: 1.0,
                min_z: 2.0,
                max_x: 3.0,
                max_z: 4.0
            }]
        );
        assert!((evaluation.height(0, 0) - (-512.0 + 2048.0 * 16384.0 / 65535.0)).abs() < 1e-3);
    }

    #[test]
    fn a_navigation_rebake_clears_the_stale_regions_status_reports() {
        let mut engine = TerrainEngine::new();
        engine.navigation_rebaked();
        assert!(
            engine.stale_navigation().is_empty(),
            "nothing evaluated yet"
        );
        engine.evaluation =
            Some(TerrainEvaluation::decode(&reply(3, &[[1.0, 2.0, 3.0, 4.0]], 0, &[])).unwrap());
        assert_eq!(engine.stale_navigation().len(), 1);
        assert_eq!(
            engine.status().values["navigation_stale"],
            Value::Bool(true)
        );
        engine.navigation_rebaked();
        assert!(engine.stale_navigation().is_empty());
        assert_eq!(
            engine.status().values["navigation_stale"],
            Value::Bool(false)
        );
        assert!(
            engine.evaluation().is_some(),
            "only the stale flag is consumed"
        );
    }

    #[test]
    fn a_truncated_or_padded_reply_is_refused() {
        let bytes = reply(3, &[], 0, &[]);
        assert!(TerrainEvaluation::decode(&bytes[..bytes.len() - 1]).is_err());
        let mut padded = bytes;
        padded.push(0);
        assert!(TerrainEvaluation::decode(&padded).is_err());
    }

    #[test]
    fn digests_distinguish_heights_and_weights_separately() {
        let base = TerrainEvaluation::decode(&reply(3, &[], 100, &[])).unwrap();
        let raised = TerrainEvaluation::decode(&reply(3, &[], 101, &[])).unwrap();
        let holed = TerrainEvaluation::decode(&reply(3, &[], 100, &[2])).unwrap();
        assert_ne!(base.heights_digest(), raised.heights_digest());
        assert_eq!(base.weights_digest(), raised.weights_digest());
        assert_eq!(base.heights_digest(), holed.heights_digest());
        assert_ne!(base.weights_digest(), holed.weights_digest());
    }

    #[test]
    fn an_engine_failure_names_its_code() {
        let mut writer = Writer::new();
        writer.u32(1);
        writer.text("terrain.evaluate");
        writer.text("a dab lies outside the region");
        let problem = failure(&writer.finish());
        assert!(problem.to_string().contains("terrain.evaluate: a dab lies"));
    }

    #[test]
    fn each_changed_stack_is_sent_once_and_never_while_one_is_in_flight() {
        use cy_editor_protocol::{Session, read_frame};

        let (editor_reader, _runtime_writer) = std::io::pipe().unwrap();
        let (mut runtime_reader, editor_writer) = std::io::pipe().unwrap();
        let runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
        let terrain = NodeId::from_u128(0x29);
        let mut engine = TerrainEngine::new();
        let sent = |reader: &mut std::io::PipeReader| match Message::decode(
            &read_frame(reader).unwrap().unwrap(),
        )
        .unwrap()
        {
            Message::ServiceRequest {
                request,
                operation,
                payload,
                ..
            } => {
                assert_eq!(operation, TERRAIN_EVALUATE_OPERATION);
                (request, payload)
            }
            other => panic!("expected a terrain request, got {other:?}"),
        };

        assert!(
            engine
                .maintain(&runtime, Some((terrain, Ok(vec![1]))))
                .is_none()
        );
        let (first, payload) = sent(&mut runtime_reader);
        assert_eq!(payload, [1]);
        assert!(engine.pending());
        // A newer stack waits for the answer rather than queueing a second evaluation.
        assert!(
            engine
                .maintain(&runtime, Some((terrain, Ok(vec![2]))))
                .is_none()
        );
        assert!(
            engine
                .accept(&Message::ServiceEvent {
                    request: first,
                    kind: ServiceEventKind::Completed,
                    schema_version: SERVICE_SCHEMA_VERSION,
                    payload: tests::reply(3, &[], 7, &[]),
                })
                .is_none()
        );
        assert!(!engine.pending());
        assert!(engine.is_current(Some(&[1])));
        assert!(!engine.is_current(Some(&[2])));
        // The same stack again is not resent; the changed one now is.
        assert!(
            engine
                .maintain(&runtime, Some((terrain, Ok(vec![1]))))
                .is_none()
        );
        assert!(!engine.pending());
        assert!(
            engine
                .maintain(&runtime, Some((terrain, Ok(vec![2]))))
                .is_none()
        );
        let (second, payload) = sent(&mut runtime_reader);
        assert_eq!(payload, [2]);

        // A refusal is kept for the panel and the stale evaluation stays visible.
        let mut failure = Writer::new();
        failure.u32(1);
        failure.text("terrain.evaluate");
        failure.text("a dab lies outside the region");
        assert!(
            engine
                .accept(&Message::ServiceEvent {
                    request: second,
                    kind: ServiceEventKind::Failed,
                    schema_version: SERVICE_SCHEMA_VERSION,
                    payload: failure.finish(),
                })
                .is_some()
        );
        assert!(engine.problem().unwrap().contains("outside the region"));
        assert!(engine.evaluation().is_some());
        assert_eq!(
            engine.status().values["evaluated"],
            Value::Bool(true),
            "the last good evaluation is still reported"
        );
    }
}
