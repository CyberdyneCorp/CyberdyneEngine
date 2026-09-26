// SPDX-License-Identifier: MIT
//! Typed results from the engine-owned VFX compiler service.

use cy_editor_core::codec::Reader;
use cy_editor_core::problem::{Problem, Result};

/// One particle array derived by the engine compiler.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttributeLayout {
    /// Authored attribute name.
    pub name: String,
    /// Compiler numeric type.
    pub kind: String,
    /// Engine precision enum identity.
    pub precision: u8,
    /// Offset of the attribute array in the emitter block.
    pub offset: u32,
    /// Bytes used by each particle.
    pub stride: u32,
    /// Whether liveness removed this attribute.
    pub elided: bool,
}

/// One engine compiled emitter and its generated shader source.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompiledEmitter {
    /// Authored emitter name.
    pub name: String,
    /// Engine simulation path identity.
    pub path: u8,
    /// Number of generated kernels.
    pub kernels: u32,
    /// Allocated bytes for each particle.
    pub bytes_per_particle: u32,
    /// Population possible within the compile budget.
    pub max_population: u32,
    /// Relative cost estimate at the reference population.
    pub estimated_cost_units: u64,
    /// Number of constants folded by the compiler.
    pub folded_constants: u32,
    /// Derived particle storage layout.
    pub layout: Vec<AttributeLayout>,
    /// Generated Slang, one source per kernel.
    pub sources: Vec<String>,
}

/// System cook report published by the engine.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VfxCompileReport {
    /// Engine content identity of the cooked system.
    pub cook_key: u64,
    /// Total generated kernels.
    pub kernels: u32,
    /// Sum of emitter particle sizes.
    pub total_bytes_per_particle: u32,
    /// Per-emitter reports.
    pub emitters: Vec<CompiledEmitter>,
}

/// An engine diagnostic for one authored graph node and optional pin.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VfxCompileDiagnostic {
    /// Engine diagnostic severity identity.
    pub severity: u8,
    /// Stable diagnostic code.
    pub code: String,
    /// Human explanation.
    pub message: String,
    /// Additional type or feature detail.
    pub detail: String,
    /// Authored graph node key.
    pub node: u64,
    /// Optional input or output pin name.
    pub pin: String,
    /// Emitter index in the submitted document, when the engine can identify it.
    pub emitter: Option<u32>,
    /// Engine stage identity, in authoring order, when the engine can identify it. A module
    /// reference the cook could not read names its emitter and no stage.
    pub stage: Option<u8>,
}

/// Terminal compiler refusal, including node diagnostics where available.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VfxCompileFailure {
    /// Service failure code.
    pub code: String,
    /// Service failure summary.
    pub message: String,
    /// Node-level compiler diagnostics.
    pub diagnostics: Vec<VfxCompileDiagnostic>,
}

fn invalid(reason: &str) -> Problem {
    Problem::new("read the VFX compiler result", reason)
}

fn count(input: &mut Reader<'_>) -> Result<usize> {
    let value = input.u32()?;
    if value > 4096 {
        return Err(invalid("too many VFX compiler records"));
    }
    Ok(value as usize)
}

fn done(input: &Reader<'_>) -> Result<()> {
    if input.remaining() != 0 {
        return Err(invalid("trailing VFX compiler result data"));
    }
    Ok(())
}

impl VfxCompileReport {
    /// Decode a successful `vfx.compile` service result.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        if input.u32()? != 1 {
            return Err(invalid("unsupported VFX compiler result schema"));
        }
        let cook_key = input.u64()?;
        let kernels = input.u32()?;
        let total_bytes_per_particle = input.u32()?;
        let mut emitters = Vec::new();
        for _ in 0..count(&mut input)? {
            let name = input.text()?;
            let path = input.u8()?;
            let kernel_count = input.u32()?;
            let bytes_per_particle = input.u32()?;
            let max_population = input.u32()?;
            let estimated_cost_units = input.u64()?;
            let folded_constants = input.u32()?;
            let mut layout = Vec::new();
            for _ in 0..count(&mut input)? {
                let entry = AttributeLayout {
                    name: input.text()?,
                    kind: input.text()?,
                    precision: input.u8()?,
                    offset: input.u32()?,
                    stride: input.u32()?,
                    elided: match input.u8()? {
                        0 => false,
                        1 => true,
                        _ => return Err(invalid("invalid VFX layout flag")),
                    },
                };
                layout.push(entry);
            }
            let mut sources = Vec::new();
            for _ in 0..count(&mut input)? {
                sources.push(input.text()?);
            }
            if name.is_empty() || path > 1 || sources.len() != kernel_count as usize {
                return Err(invalid("inconsistent VFX emitter result"));
            }
            emitters.push(CompiledEmitter {
                name,
                path,
                kernels: kernel_count,
                bytes_per_particle,
                max_population,
                estimated_cost_units,
                folded_constants,
                layout,
                sources,
            });
        }
        done(&input)?;
        if emitters
            .iter()
            .try_fold(0_u32, |total, emitter| total.checked_add(emitter.kernels))
            != Some(kernels)
        {
            return Err(invalid("inconsistent VFX kernel count"));
        }
        Ok(Self {
            cook_key,
            kernels,
            total_bytes_per_particle,
            emitters,
        })
    }
}

impl VfxCompileFailure {
    /// Decode a failed `vfx.compile` service result.
    pub fn decode(payload: &[u8]) -> Result<Self> {
        let mut input = Reader::new(payload);
        let schema = input.u32()?;
        if schema != 1 && schema != 2 {
            return Err(invalid("unsupported VFX compiler diagnostic schema"));
        }
        let code = input.text()?;
        let message = input.text()?;
        let mut diagnostics = Vec::new();
        for _ in 0..count(&mut input)? {
            let mut diagnostic = VfxCompileDiagnostic {
                severity: input.u8()?,
                code: input.text()?,
                message: input.text()?,
                detail: input.text()?,
                node: input.u64()?,
                pin: input.text()?,
                emitter: None,
                stage: None,
            };
            if schema == 2 {
                let emitter = input.u32()?;
                let stage = input.u8()?;
                if emitter == u32::MAX && stage == 6 {
                    // Parse and pre-compile failures have no authored graph location.
                } else if emitter == u32::MAX || stage > 6 {
                    return Err(invalid("invalid VFX compiler diagnostic scope"));
                } else {
                    diagnostic.emitter = Some(emitter);
                    diagnostic.stage = (stage < 6).then_some(stage);
                }
            }
            if diagnostic.severity > 2 || diagnostic.code.is_empty() {
                return Err(invalid("invalid VFX compiler diagnostic"));
            }
            diagnostics.push(diagnostic);
        }
        done(&input)?;
        Ok(Self {
            code,
            message,
            diagnostics,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::codec::Writer;

    #[test]
    fn compiled_emitter_preserves_source_and_layout() {
        let mut out = Writer::new();
        out.u32(1);
        out.u64(42);
        out.u32(1);
        out.u32(12);
        out.u32(1);
        out.text("smoke");
        out.u8(0);
        out.u32(1);
        out.u32(12);
        out.u32(100);
        out.u64(300);
        out.u32(2);
        out.u32(1);
        out.text("position");
        out.text("float3");
        out.u8(1);
        out.u32(0);
        out.u32(12);
        out.u8(0);
        out.u32(1);
        out.text("void cyVfxKernel() {}");
        let report = VfxCompileReport::decode(&out.finish()).unwrap();
        assert_eq!(report.emitters[0].layout[0].stride, 12);
        assert_eq!(report.emitters[0].sources[0], "void cyVfxKernel() {}");
    }

    #[test]
    fn node_and_pin_diagnostics_survive_the_service_wire() {
        let mut out = Writer::new();
        out.u32(2);
        out.text("vfx.compile");
        out.text("invalid link");
        out.u32(1);
        out.u8(2);
        out.text("graph.pin-type");
        out.text("type mismatch");
        out.text("float3");
        out.u64(7);
        out.text("value");
        out.u32(1);
        out.u8(2);
        let failure = VfxCompileFailure::decode(&out.finish()).unwrap();
        assert_eq!(failure.diagnostics[0].node, 7);
        assert_eq!(failure.diagnostics[0].pin, "value");
        assert_eq!(failure.diagnostics[0].emitter, Some(1));
        assert_eq!(failure.diagnostics[0].stage, Some(2));
    }

    #[test]
    fn a_module_diagnostic_names_its_emitter_without_a_stage() {
        let mut out = Writer::new();
        out.u32(2);
        out.text("vfx.modules");
        out.text("missing module");
        out.u32(1);
        out.u8(2);
        out.text("vfx.module.missing");
        out.text("this emitter references a module the project does not have");
        out.text("shared_drag");
        out.u64(0);
        out.text("");
        out.u32(1);
        out.u8(6);
        let failure = VfxCompileFailure::decode(&out.finish()).unwrap();
        assert_eq!(failure.diagnostics[0].detail, "shared_drag");
        assert_eq!(failure.diagnostics[0].emitter, Some(1));
        assert_eq!(failure.diagnostics[0].stage, None);
    }

    #[test]
    fn legacy_vfx_diagnostics_remain_unscoped() {
        let mut out = Writer::new();
        out.u32(1);
        out.text("vfx.compile");
        out.text("invalid node");
        out.u32(1);
        out.u8(2);
        out.text("graph.invalid-node");
        out.text("unknown node type");
        out.text("");
        out.u64(1);
        out.text("");
        let failure = VfxCompileFailure::decode(&out.finish()).unwrap();
        assert_eq!(failure.diagnostics[0].emitter, None);
        assert_eq!(failure.diagnostics[0].stage, None);
    }
}
