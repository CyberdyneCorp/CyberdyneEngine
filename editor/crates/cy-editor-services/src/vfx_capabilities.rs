// SPDX-License-Identifier: MIT
//! Versioned renderer and simulation-target options published by the VFX runtime.

use cy_editor_core::codec::Reader;
use cy_editor_core::problem::{Problem, Result};

/// One engine renderer kind and whether this build can composite it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct RendererCapability {
    /// Engine `RendererKind` numeric identity.
    pub kind: u8,
    /// Engine-owned name.
    pub name: String,
    /// Whether a complete draw path exists.
    pub available: bool,
    /// Named reason when unavailable.
    pub reason: String,
}

/// One simulation path, including the current runtime-device decision.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct TargetCapability {
    /// Engine `SimulationPath` numeric identity.
    pub path: u8,
    /// Engine-owned name.
    pub name: String,
    /// Whether this build can compile the target.
    pub compile_available: bool,
    /// Whether the attached device can execute it now.
    pub runtime_available: bool,
    /// Engine fallback reason code.
    pub reason: String,
    /// Engine explanation for the decision.
    pub explanation: String,
}

/// A single backend snapshot of renderer and target options.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct VfxAuthoringCapabilities {
    /// Renderer kinds in engine registry order.
    pub renderers: Vec<RendererCapability>,
    /// Simulation paths in engine order.
    pub targets: Vec<TargetCapability>,
}

impl VfxAuthoringCapabilities {
    /// Decode the engine-owned schema, refusing partial and duplicate records.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut input = Reader::new(bytes);
        if input.u32()? != 1 {
            return Err(invalid("unsupported VFX capability schema"));
        }
        let renderer_count = input.u32()?;
        if renderer_count == 0 || renderer_count > 32 {
            return Err(invalid("invalid VFX renderer count"));
        }
        let mut renderers = Vec::new();
        for _ in 0..renderer_count {
            let renderer = RendererCapability {
                kind: input.u8()?,
                name: input.text()?,
                available: read_bool(&mut input)?,
                reason: input.text()?,
            };
            if renderer.name.is_empty()
                || renderers
                    .iter()
                    .any(|prior: &RendererCapability| prior.kind == renderer.kind)
                || (!renderer.available && renderer.reason.is_empty())
            {
                return Err(invalid("invalid VFX renderer entry"));
            }
            renderers.push(renderer);
        }
        let target_count = input.u32()?;
        if target_count != 2 {
            return Err(invalid("expected both VFX simulation paths"));
        }
        let mut targets = Vec::new();
        for _ in 0..target_count {
            let target = TargetCapability {
                path: input.u8()?,
                name: input.text()?,
                compile_available: read_bool(&mut input)?,
                runtime_available: read_bool(&mut input)?,
                reason: input.text()?,
                explanation: input.text()?,
            };
            if target.path > 1
                || target.name.is_empty()
                || targets
                    .iter()
                    .any(|prior: &TargetCapability| prior.path == target.path)
            {
                return Err(invalid("invalid VFX simulation target"));
            }
            targets.push(target);
        }
        if input.remaining() != 0 {
            return Err(invalid("trailing VFX capability data"));
        }
        Ok(Self { renderers, targets })
    }
}

fn read_bool(input: &mut Reader<'_>) -> Result<bool> {
    match input.u8()? {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(invalid("invalid VFX availability flag")),
    }
}

fn invalid(reason: &str) -> Problem {
    Problem::new("load VFX authoring capabilities", reason)
}

#[cfg(test)]
mod tests {
    use cy_editor_core::codec::Writer;

    use super::*;

    #[test]
    fn capability_schema_keeps_unavailable_reason_and_target_status() {
        let mut writer = Writer::new();
        writer.u32(1);
        writer.u32(1);
        writer.u8(5);
        writer.text("Decal");
        writer.u8(0);
        writer.text("No projection pass");
        writer.u32(2);
        for (path, name, available) in [(0, "GpuPreferred", false), (1, "CpuRequired", true)] {
            writer.u8(path);
            writer.text(name);
            writer.u8(1);
            writer.u8(u8::from(available));
            writer.text(if available {
                "EffectRequiresCpu"
            } else {
                "NoDeviceInThisWorld"
            });
            writer.text("reason");
        }
        let decoded = VfxAuthoringCapabilities::decode(&writer.finish()).unwrap();
        assert_eq!(decoded.renderers[0].reason, "No projection pass");
        assert!(!decoded.targets[0].runtime_available);
        assert!(decoded.targets[1].runtime_available);
    }

    #[test]
    fn capability_schema_refuses_unexplained_unavailable_renderers() {
        let mut writer = Writer::new();
        writer.u32(1);
        writer.u32(1);
        writer.u8(5);
        writer.text("Decal");
        writer.u8(0);
        writer.text("");
        writer.u32(2);
        for path in 0..2 {
            writer.u8(path);
            writer.text("target");
            writer.u8(1);
            writer.u8(0);
            writer.text("NoDeviceInThisWorld");
            writer.text("reason");
        }
        assert!(VfxAuthoringCapabilities::decode(&writer.finish()).is_err());
    }
}
