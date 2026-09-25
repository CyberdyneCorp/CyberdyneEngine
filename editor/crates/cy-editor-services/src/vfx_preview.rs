// SPDX-License-Identifier: MIT
//! Bounded engine VFX preview snapshots and controls.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

/// Live count for one compiled emitter.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EmitterCount {
    /// Engine-authored emitter name.
    pub name: String,
    /// Particles alive in the isolated preview world.
    pub live: u32,
}

/// One engine snapshot. The viewport renderer is connected separately.
#[derive(Clone, Debug, PartialEq)]
pub struct VfxPreviewSnapshot {
    /// Cook identity; parameter updates preserve it.
    pub cook_key: u64,
    /// Whether frame requests advance simulation.
    pub playing: bool,
    /// Current effect time in seconds.
    pub time_seconds: f32,
    /// Simulation speed multiplier.
    pub time_scale: f32,
    /// Current total live population.
    pub live_particles: u32,
    /// Particles spawned in the most recent step.
    pub spawned: u32,
    /// Particles killed in the most recent step.
    pub killed: u32,
    /// GPU-preferred emitters running on the CPU in this preview.
    pub cpu_fallbacks: u32,
    /// Pool bytes in use.
    pub pool_used_bytes: u64,
    /// Pool byte budget.
    pub pool_total_bytes: u64,
    /// Events dropped by bounded channels in the most recent step.
    pub events_dropped: u32,
    /// Events truncated by chain depth in the most recent step.
    pub events_truncated: u32,
    /// Per-emitter population in document order.
    pub emitters: Vec<EmitterCount>,
}

impl VfxPreviewSnapshot {
    /// Decode the complete engine-owned schema and reject partial results.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut input = Reader::new(bytes);
        if input.u32()? != 1 {
            return Err(invalid("unsupported VFX preview schema"));
        }
        let cook_key = input.u64()?;
        let playing = match input.u8()? {
            0 => false,
            1 => true,
            _ => return Err(invalid("invalid VFX preview play state")),
        };
        let time_seconds = input.f32()?;
        let time_scale = input.f32()?;
        let live_particles = input.u32()?;
        let spawned = input.u32()?;
        let killed = input.u32()?;
        let cpu_fallbacks = input.u32()?;
        let pool_used_bytes = input.u64()?;
        let pool_total_bytes = input.u64()?;
        let events_dropped = input.u32()?;
        let events_truncated = input.u32()?;
        let emitter_count = input.u32()?;
        if cook_key == 0
            || !time_seconds.is_finite()
            || time_seconds < 0.0
            || !time_scale.is_finite()
            || !(0.1..=4.0).contains(&time_scale)
            || pool_used_bytes > pool_total_bytes
            || emitter_count == 0
            || emitter_count > 256
        {
            return Err(invalid("invalid VFX preview values"));
        }
        let mut emitters = Vec::new();
        for _ in 0..emitter_count {
            let name = input.text()?;
            let live = input.u32()?;
            if name.is_empty()
                || emitters
                    .iter()
                    .any(|prior: &EmitterCount| prior.name == name)
            {
                return Err(invalid("invalid VFX preview emitter"));
            }
            emitters.push(EmitterCount { name, live });
        }
        if !input.is_empty() {
            return Err(invalid("trailing VFX preview data"));
        }
        Ok(Self {
            cook_key,
            playing,
            time_seconds,
            time_scale,
            live_particles,
            spawned,
            killed,
            cpu_fallbacks,
            pool_used_bytes,
            pool_total_bytes,
            events_dropped,
            events_truncated,
            emitters,
        })
    }
}

/// Editor actions accepted by the engine preview service.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum VfxPreviewAction {
    /// Start advancing on frame requests.
    Play,
    /// Hold the current state.
    Pause,
    /// Return to time zero.
    Restart,
    /// Replay deterministically to a bounded time.
    Scrub(f32),
    /// Change simulation speed.
    TimeScale(f32),
}

impl VfxPreviewAction {
    /// Encode one engine control request.
    pub fn encode(&self) -> Result<Vec<u8>> {
        let mut out = Writer::new();
        match *self {
            Self::Play => out.u8(0),
            Self::Pause => out.u8(1),
            Self::Restart => out.u8(2),
            Self::Scrub(seconds) => {
                if !seconds.is_finite() || !(0.0..=30.0).contains(&seconds) {
                    return Err(invalid("scrub time must be between 0 and 30 seconds"));
                }
                out.u8(3);
                out.f32(seconds);
            }
            Self::TimeScale(scale) => {
                if !scale.is_finite() || !(0.1..=4.0).contains(&scale) {
                    return Err(invalid("time scale must be between 0.1 and 4"));
                }
                out.u8(4);
                out.f32(scale);
            }
        }
        Ok(out.finish())
    }
}

/// Encode a live parameter update without recompiling the graph.
pub fn parameter_update(name: &str, values: &[f32]) -> Result<Vec<u8>> {
    if name.is_empty()
        || name.len() > 128
        || !name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
        || values.is_empty()
        || values.len() > 4
        || values.iter().any(|value| !value.is_finite())
    {
        return Err(invalid("invalid VFX preview parameter"));
    }
    let mut out = Writer::new();
    out.text(name);
    out.u8(u8::try_from(values.len()).expect("four lanes at most"));
    for value in values {
        out.f32(*value);
    }
    Ok(out.finish())
}

fn invalid(reason: &str) -> Problem {
    Problem::new("use VFX preview", reason)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preview_snapshot_keeps_engine_counts_and_refuses_trailing_data() {
        let mut writer = Writer::new();
        writer.u32(1);
        writer.u64(42);
        writer.u8(1);
        writer.f32(0.5);
        writer.f32(2.0);
        for value in [12, 4, 0, 1] {
            writer.u32(value);
        }
        writer.u64(512);
        writer.u64(4096);
        writer.u32(2);
        writer.u32(0);
        writer.u32(2);
        writer.text("CpuEmitter");
        writer.u32(6);
        writer.text("GpuEmitter");
        writer.u32(6);
        let bytes = writer.finish();
        let snapshot = VfxPreviewSnapshot::decode(&bytes).unwrap();
        assert_eq!(snapshot.cook_key, 42);
        assert_eq!(snapshot.emitters[1].live, 6);
        assert_eq!(snapshot.cpu_fallbacks, 1);
        let mut trailing = bytes;
        trailing.push(0);
        assert!(VfxPreviewSnapshot::decode(&trailing).is_err());
    }

    #[test]
    fn controls_and_parameters_refuse_values_the_engine_cannot_use() {
        assert!(VfxPreviewAction::Scrub(31.0).encode().is_err());
        assert!(VfxPreviewAction::TimeScale(f32::NAN).encode().is_err());
        assert!(parameter_update("speed", &[4.0]).is_ok());
        assert!(parameter_update("speed", &[f32::INFINITY]).is_err());
    }
}
