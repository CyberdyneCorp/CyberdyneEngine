// SPDX-License-Identifier: MIT
//! The editor's side of `illumination.report` (issue #35): one request in flight, the next one
//! queued behind it, and the engine's last report kept, decoded.
//!
//! Every number in an [`IlluminationReport`] was decoded from the engine's reply — the editor
//! computes no illumination — so the panel and `gi.status` show the GI's own state: the settings
//! it runs at, the tier its rays took, its convergence and cost per stage, each emitter's class
//! and each object's, and for a clicked point `IlluminationSystem::explain`. The wire is
//! `src/editor_backend/include/cy/editor/illumination_service.h`'s.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::runtime::RuntimeSession;

const SCHEMA: u32 = 1;
const FORMAT: u32 = 1;
/// The operation's name, as the engine routes it.
pub const REPORT: &str = "illumination.report";

/// A pixel of a published frame to explain.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Pick {
    /// The viewport the frame came from.
    pub viewport: u32,
    /// The published frame.
    pub frame: u64,
    /// Pixel column and row in the frame.
    pub x: f32,
    /// Pixel row.
    pub y: f32,
}

/// The request payload: format, then the pick when there is one.
#[must_use]
pub fn report_payload(pick: Option<Pick>) -> Vec<u8> {
    let mut writer = Writer::new();
    writer.u32(FORMAT);
    match pick {
        Some(pick) => {
            writer.u8(1);
            writer.u32(pick.viewport);
            writer.u64(pick.frame);
            writer.f32(pick.x);
            writer.f32(pick.y);
        }
        None => writer.u8(0),
    }
    writer.finish()
}

/// One stage's wall time in the frame the GI last ran.
#[derive(Clone, PartialEq, Debug)]
pub struct Stage {
    /// The engine's name for it.
    pub name: String,
    /// Milliseconds.
    pub ms: f64,
}

/// One emitter, as the engine classified it.
#[derive(Clone, PartialEq, Debug)]
pub struct Emitter {
    /// The emitting object's engine identity.
    pub node: u64,
    /// The material it emits through.
    pub material: String,
    /// `CacheOnly`, `PromotedLight`, `Demoted` or `Disabled`.
    pub class: String,
    /// The rule that decided it, or `Automatic`.
    pub rule: String,
    /// Cards, square metres and radiant power.
    pub cards: u32,
    /// Square metres.
    pub area: f32,
    /// Radiant power, pi * luminance * area.
    pub power: f32,
}

/// One object's class.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ObjectClass {
    /// Its engine identity.
    pub node: u64,
    /// The engine's class name.
    pub class: String,
    /// An author set it.
    pub overridden: bool,
}

/// One source's part in an explained point.
#[derive(Clone, PartialEq, Debug)]
pub struct Source {
    /// The engine's name for the source.
    pub name: String,
    /// What it answered.
    pub radiance: [f32; 3],
    /// How much it was trusted.
    pub confidence: f32,
    /// Its share of the weight, 0 to 1.
    pub weight_share: f32,
    /// Excluded by the double-counting rule.
    pub excluded: bool,
}

/// What lit a clicked point.
#[derive(Clone, PartialEq, Debug)]
pub struct Explained {
    /// Where the click met the world, and the surface's normal there.
    pub point: [f32; 3],
    /// The surface's normal.
    pub normal: [f32; 3],
    /// Why it is the colour it is: `Lit`, `Occluded`, `Unconverged`, `MissingProbes`,
    /// `LowConfidence` or `Invalidated`.
    pub cause: String,
    /// The indirect diffuse the resolve gives it.
    pub diffuse: [f32; 3],
    /// Its confidence.
    pub confidence: f32,
    /// The source that carried most of it.
    pub dominant: String,
    /// Probes contributing, probes in reach, and the leak suspects among them.
    pub probes_contributing: u32,
    /// Probes within reach.
    pub probes_in_reach: u32,
    /// Contributing probes the field says cannot see the point.
    pub leak_suspects: u32,
    /// Their share of the probe weight.
    pub leak_weight: f32,
    /// The tier a ray from there takes.
    pub world_tier: String,
    /// The region's convergence, 0 to 1.
    pub convergence: f32,
    /// Every source's part.
    pub sources: Vec<Source>,
}

/// The engine's GI report.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct IlluminationReport {
    /// The engine's names for the settings it runs at.
    pub mode: String,
    /// The global quality.
    pub quality: String,
    /// The global tier preference.
    pub tier: String,
    /// Hardware ray tracing allowed.
    pub hardware_ray_tracing: bool,
    /// The allocation, in milliseconds.
    pub budget_ms: f32,
    /// The tier that answered world rays.
    pub world_tier: String,
    /// The least converged region, 0 to 1.
    pub convergence: f32,
    /// Surface cards, probes, volumes and lights.
    pub cards: u32,
    /// Live radiance-cache probes.
    pub probes: u32,
    /// GI volumes placed.
    pub volumes: u32,
    /// Lights the world gave the GI.
    pub lights: u32,
    /// Rebuilds of the GI since the world opened, and in-place applications.
    pub rebuilds: u64,
    /// Settings, volume, class and rule changes applied in place.
    pub applications: u64,
    /// The frame the GI last ran, its allocation and its measured cost.
    pub frame: u64,
    /// Milliseconds.
    pub allocation_ms: f32,
    /// Milliseconds.
    pub measured_ms: f32,
    /// Wall time per stage.
    pub stages: Vec<Stage>,
    /// Probes and surface pages updated, rays by tier, and the caches' hit rates.
    pub probes_updated: u32,
    /// Surface pages shaded.
    pub pages_updated: u32,
    /// Screen-tier rays.
    pub screen_rays: u32,
    /// Software-tier rays.
    pub software_rays: u32,
    /// Hardware-tier rays.
    pub hardware_rays: u32,
    /// 0 to 1.
    pub surface_hit_rate: f32,
    /// 0 to 1.
    pub radiance_hit_rate: f32,
    /// Emitters classified and where their power went.
    pub emitter_count: u32,
    /// Every watt, by column.
    pub total_power: f32,
    /// Left to the cache.
    pub cached_power: f32,
    /// Promoted to lights.
    pub promoted_power: f32,
    /// Over the light budget.
    pub demoted_power: f32,
    /// Switched off.
    pub disabled_power: f32,
    /// Every emitter.
    pub emitters: Vec<Emitter>,
    /// Every object.
    pub objects: Vec<ObjectClass>,
    /// The explained point, when one was asked for and met the world.
    pub explained: Option<Explained>,
}

fn vec3(reader: &mut Reader<'_>) -> Result<[f32; 3]> {
    Ok([reader.f32()?, reader.f32()?, reader.f32()?])
}

impl IlluminationReport {
    /// Decode a COMPLETED `illumination.report` payload.
    ///
    /// # Errors
    ///
    /// A payload of another format, or one that ends early.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        if reader.u32()? != FORMAT {
            return Err(Problem::new(
                "read the GI report",
                "the report is of another format",
            ));
        }
        let mut report = Self {
            mode: reader.text()?,
            quality: reader.text()?,
            tier: reader.text()?,
            hardware_ray_tracing: reader.u8()? != 0,
            budget_ms: reader.f32()?,
            world_tier: reader.text()?,
            convergence: reader.f32()?,
            cards: reader.u32()?,
            probes: reader.u32()?,
            volumes: reader.u32()?,
            lights: reader.u32()?,
            rebuilds: reader.u64()?,
            applications: reader.u64()?,
            frame: reader.u64()?,
            allocation_ms: reader.f32()?,
            measured_ms: reader.f32()?,
            ..Self::default()
        };
        for _ in 0..reader.u32()? {
            report.stages.push(Stage {
                name: reader.text()?,
                ms: reader.f64()?,
            });
        }
        report.probes_updated = reader.u32()?;
        report.pages_updated = reader.u32()?;
        report.screen_rays = reader.u32()?;
        report.software_rays = reader.u32()?;
        report.hardware_rays = reader.u32()?;
        report.surface_hit_rate = reader.f32()?;
        report.radiance_hit_rate = reader.f32()?;
        report.emitter_count = reader.u32()?;
        report.total_power = reader.f32()?;
        report.cached_power = reader.f32()?;
        report.promoted_power = reader.f32()?;
        report.demoted_power = reader.f32()?;
        report.disabled_power = reader.f32()?;
        for _ in 0..reader.u32()? {
            report.emitters.push(Emitter {
                node: reader.u64()?,
                material: reader.text()?,
                class: reader.text()?,
                rule: reader.text()?,
                cards: reader.u32()?,
                area: reader.f32()?,
                power: reader.f32()?,
            });
        }
        for _ in 0..reader.u32()? {
            report.objects.push(ObjectClass {
                node: reader.u64()?,
                class: reader.text()?,
                overridden: reader.u8()? != 0,
            });
        }
        if reader.u8()? != 0 {
            let mut explained = Explained {
                point: vec3(&mut reader)?,
                normal: vec3(&mut reader)?,
                cause: reader.text()?,
                diffuse: vec3(&mut reader)?,
                confidence: reader.f32()?,
                dominant: reader.text()?,
                probes_contributing: reader.u32()?,
                probes_in_reach: reader.u32()?,
                leak_suspects: reader.u32()?,
                leak_weight: reader.f32()?,
                world_tier: reader.text()?,
                convergence: reader.f32()?,
                sources: Vec::new(),
            };
            for _ in 0..reader.u32()? {
                explained.sources.push(Source {
                    name: reader.text()?,
                    radiance: vec3(&mut reader)?,
                    confidence: reader.f32()?,
                    weight_share: reader.f32()?,
                    excluded: reader.u8()? != 0,
                });
            }
            report.explained = Some(explained);
        }
        if !reader.is_empty() {
            return Err(Problem::new(
                "read the GI report",
                "bytes follow the report",
            ));
        }
        Ok(report)
    }
}

/// The `illumination.*` request state [`crate::backend::BackendServices`] holds.
#[derive(Debug)]
pub struct IlluminationRequests {
    in_flight: Option<RequestId>,
    queued: Option<Vec<u8>>,
    report: Versioned<Option<IlluminationReport>>,
    problem: Option<String>,
}

impl Default for IlluminationRequests {
    fn default() -> Self {
        Self {
            in_flight: None,
            queued: None,
            report: Versioned::new(None),
            problem: None,
        }
    }
}

impl IlluminationRequests {
    /// Ask for a report, now when nothing is in flight, or after the one that is (the latest ask
    /// replaces an earlier queued one: a report is a reading, and only the newest matters).
    ///
    /// # Errors
    ///
    /// No runtime is attached.
    pub fn request(
        &mut self,
        runtime: &RuntimeSession,
        payload: Vec<u8>,
    ) -> Result<Option<RequestId>> {
        if !runtime.is_connected() {
            return Err(Problem::new(
                "ask the engine for its GI report",
                "no runtime is attached, and the editor computes no illumination itself",
            )
            .with_remedy("start the editor with a hosted runtime (`just run-editor-live`)"));
        }
        if self.in_flight.is_some() {
            self.queued = Some(payload);
            return Ok(None);
        }
        let request = runtime.service_request(SCHEMA, REPORT, payload)?;
        self.in_flight = Some(request);
        Ok(Some(request))
    }

    /// Once a frame: send what was queued, and forget a runtime that went away.
    pub fn maintain(&mut self, runtime: &RuntimeSession) -> Option<Problem> {
        if !runtime.is_connected() {
            if self.in_flight.take().is_some() {
                self.problem =
                    Some("the runtime disconnected before answering the GI report".into());
            }
            self.queued = None;
            return None;
        }
        if self.in_flight.is_none()
            && let Some(payload) = self.queued.take()
        {
            return self.request(runtime, payload).err();
        }
        None
    }

    /// Take the engine's answer to the request in flight. `None` for a message that is not one.
    pub fn accept(&mut self, message: &Message) -> Option<Option<Problem>> {
        let Message::ServiceEvent {
            request,
            kind,
            schema_version,
            payload,
        } = message
        else {
            return None;
        };
        if self.in_flight != Some(*request) {
            return None;
        }
        if matches!(
            kind,
            ServiceEventKind::Accepted | ServiceEventKind::Progress
        ) {
            return Some(None);
        }
        self.in_flight = None;
        let settled = match (kind, *schema_version) {
            (ServiceEventKind::Completed, SCHEMA) => {
                IlluminationReport::decode(payload).map(|report| {
                    self.report.set(Some(report));
                    self.problem = None;
                })
            }
            (ServiceEventKind::Completed, _) => Err(Problem::new(
                "read the GI report",
                "the engine answered in a schema this editor does not know",
            )),
            _ => Err(Problem::new(
                "ask the engine for its GI report",
                failure_detail(payload),
            )),
        };
        Some(settled.err().inspect(|problem| {
            self.problem = Some(problem.to_string());
        }))
    }

    /// The engine's last report.
    #[must_use]
    pub fn report(&self) -> Option<&IlluminationReport> {
        self.report.get().as_ref()
    }

    /// Bumped by every report, so a panel can tell a new one arrived.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.report.revision()
    }

    /// A request is in flight or queued.
    #[must_use]
    pub const fn pending(&self) -> bool {
        self.in_flight.is_some() || self.queued.is_some()
    }

    /// Why the last request failed.
    #[must_use]
    pub fn problem(&self) -> Option<&str> {
        self.problem.as_deref()
    }
}

/// `gi.status`: the engine's last report as a command outcome, for a person and for an agent.
#[must_use]
pub fn outcome(requests: &IlluminationRequests) -> cy_editor_commands::Outcome {
    use cy_editor_core::value::Value;
    let whole = |value: u32| Value::Int(i64::from(value));
    let mut outcome = cy_editor_commands::Outcome::new(if requests.report().is_some() {
        "The engine's GI report"
    } else {
        "The engine has not reported its GI"
    })
    .with("pending", Value::Bool(requests.pending()));
    if let Some(problem) = requests.problem() {
        outcome = outcome.with("problem", Value::Text(problem.to_owned()));
    }
    let Some(report) = requests.report() else {
        return outcome;
    };
    outcome = outcome
        .with("mode", Value::Text(report.mode.clone()))
        .with("quality", Value::Text(report.quality.clone()))
        .with("tier", Value::Text(report.tier.clone()))
        .with(
            "hardware_ray_tracing",
            Value::Bool(report.hardware_ray_tracing),
        )
        .with("budget_ms", Value::Float(report.budget_ms))
        .with("world_tier", Value::Text(report.world_tier.clone()))
        .with("convergence", Value::Float(report.convergence))
        .with("cards", whole(report.cards))
        .with("probes", whole(report.probes))
        .with("volumes", whole(report.volumes))
        .with("measured_ms", Value::Float(report.measured_ms))
        .with("rays.software", whole(report.software_rays))
        .with("rays.hardware", whole(report.hardware_rays))
        .with("rays.screen", whole(report.screen_rays))
        .with("emitters", whole(report.emitter_count))
        .with("power.promoted", Value::Float(report.promoted_power))
        .with("power.cached", Value::Float(report.cached_power))
        .with("power.disabled", Value::Float(report.disabled_power));
    for stage in &report.stages {
        #[allow(clippy::cast_possible_truncation, reason = "a stage's milliseconds")]
        let ms = stage.ms as f32;
        outcome = outcome.with(format!("stage.{}", stage.name), Value::Float(ms));
    }
    for emitter in &report.emitters {
        outcome = outcome.with(
            format!("emitter.{:016x}.{}", emitter.node, emitter.material),
            Value::Text(format!(
                "{} (rule {}): {} cards, {:.3} m^2, power {:.3}",
                emitter.class, emitter.rule, emitter.cards, emitter.area, emitter.power
            )),
        );
    }
    for object in &report.objects {
        outcome = outcome.with(
            format!("object.{:016x}", object.node),
            Value::Text(format!(
                "{}{}",
                object.class,
                if object.overridden { " (set)" } else { "" }
            )),
        );
    }
    if let Some(explained) = &report.explained {
        outcome = outcome
            .with("point", Value::Vec3(explained.point))
            .with("point.cause", Value::Text(explained.cause.clone()))
            .with("point.diffuse", Value::Vec3(explained.diffuse))
            .with("point.confidence", Value::Float(explained.confidence))
            .with("point.dominant", Value::Text(explained.dominant.clone()))
            .with("point.probes", whole(explained.probes_contributing))
            .with("point.leak_suspects", whole(explained.leak_suspects))
            .with("point.convergence", Value::Float(explained.convergence));
    }
    outcome
}

/// The schema-1 failure payload's code and detail, as a sentence.
fn failure_detail(payload: &[u8]) -> String {
    let mut reader = Reader::new(payload);
    match (reader.u32(), reader.text(), reader.text()) {
        (Ok(1), Ok(code), Ok(detail)) => format!("{code}: {detail}"),
        _ => "the engine refused the request".into(),
    }
}

#[cfg(test)]
#[allow(
    clippy::float_cmp,
    reason = "the values compared are the exact literals the engine's fixture was written with"
)]
mod tests {
    use super::*;

    /// The engine's own encoding of a report: `src/editor_backend/tests/test_illumination_preview.cpp`
    /// writes it with `encode_illumination_report` and holds it to this committed file.
    fn engine_report() -> Vec<u8> {
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../src/editor_backend/tests/data/illumination_report_v1.wire");
        std::fs::read(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
    }

    #[test]
    fn the_engines_report_decodes_field_for_field() {
        let report = IlluminationReport::decode(&engine_report()).unwrap();
        assert_eq!(report.mode, "Dynamic");
        assert_eq!(report.quality, "High");
        assert_eq!(report.tier, "Automatic");
        assert!(report.hardware_ray_tracing);
        assert_eq!(report.budget_ms, 2.5);
        assert_eq!(report.world_tier, "HardwareTrace");
        assert_eq!(report.convergence, 0.75);
        assert_eq!(
            (report.cards, report.probes, report.volumes, report.lights),
            (120, 64, 1, 2)
        );
        assert_eq!(
            (report.rebuilds, report.applications, report.frame),
            (1, 3, 40)
        );
        assert_eq!(report.measured_ms, 1.75);
        assert_eq!(report.stages.len(), 6);
        assert_eq!(report.stages[2].name, "ProbeUpdate");
        assert!((report.stages[2].ms - 1.5).abs() < f64::EPSILON);
        assert_eq!((report.software_rays, report.hardware_rays), (900, 1200));
        assert_eq!(
            (report.surface_hit_rate, report.radiance_hit_rate),
            (0.5, 0.25)
        );
        assert_eq!(report.promoted_power, 41.0);
        assert_eq!(report.emitters.len(), 1);
        assert_eq!(report.emitters[0].node, 0x12);
        assert_eq!(report.emitters[0].material, "materials/lamp-glass");
        assert_eq!(report.emitters[0].class, "PromotedLight");
        assert_eq!(report.emitters[0].rule, "Automatic");
        assert_eq!(
            (report.emitters[0].cards, report.emitters[0].area),
            (16, 0.25)
        );
        assert_eq!(report.objects[0].node, 0x13);
        assert_eq!(report.objects[0].class, "FastDynamic");
        assert!(!report.objects[0].overridden);
        let explained = report.explained.unwrap();
        assert_eq!(explained.point, [0.75, 0.0, 0.0]);
        assert_eq!(explained.cause, "Lit");
        assert_eq!(explained.diffuse, [0.5, 0.25, 0.125]);
        assert_eq!(explained.dominant, "RadianceCache");
        assert_eq!(
            (explained.probes_contributing, explained.probes_in_reach),
            (6, 8)
        );
        assert_eq!(explained.convergence, 0.9375);
        assert_eq!(explained.sources.len(), 1);
        assert_eq!(explained.sources[0].weight_share, 1.0);
    }

    #[test]
    fn a_truncated_or_padded_report_is_refused() {
        let bytes = engine_report();
        assert!(IlluminationReport::decode(&bytes[..bytes.len() - 1]).is_err());
        let mut padded = bytes;
        padded.push(0);
        assert!(IlluminationReport::decode(&padded).is_err());
    }

    #[test]
    fn a_report_request_carries_the_pick_when_there_is_one() {
        assert_eq!(report_payload(None), vec![1, 0, 0, 0, 0]);
        let picked = report_payload(Some(Pick {
            viewport: 2,
            frame: 11,
            x: 160.0,
            y: 120.0,
        }));
        assert_eq!(picked.len(), 4 + 1 + 4 + 8 + 4 + 4);
        assert_eq!(picked[4], 1);
        assert_eq!(&picked[9..17], &11_u64.to_le_bytes());
    }
}
