// SPDX-License-Identifier: MIT
//! Project storage contract for editable VFX documents.

use std::path::{Component, Path};

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};

const MAX_ITEMS: u32 = 4096;

struct ModulePlan {
    roots: Vec<String>,
    mappings: Vec<(String, String)>,
}

/// Transaction kind prefix for a saved VFX authoring document.
pub const DOMAIN_PREFIX: &str = "vfx_document:";

/// Validate a project-relative VFX authoring document reference.
pub fn validate_reference(reference: &str) -> Result<()> {
    let path = Path::new(reference);
    if path
        .extension()
        .is_none_or(|extension| extension != "cyvfxdoc")
        || !path
            .components()
            .all(|component| matches!(component, Component::Normal(_)))
    {
        return Err(Problem::new(
            "access a VFX document",
            "the path must be a project-relative .cyvfxdoc file",
        ));
    }
    Ok(())
}

/// Check the versioned ASCII envelope before writing it to a project.
pub fn validate_source(source: &str) -> Result<()> {
    parse_payload(&decode_source(source)?)?;
    Ok(())
}

fn decode_source(source: &str) -> Result<Vec<u8>> {
    let payload = source
        .strip_prefix("cyvfxdoc 1\n")
        .ok_or_else(|| Problem::new("save a VFX document", "expected cyvfxdoc 1 source"))?;
    if payload.is_empty()
        || payload.len() % 2 != 0
        || !payload.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return Err(Problem::new(
            "save a VFX document",
            "expected an even-length hexadecimal authoring payload",
        ));
    }
    let mut bytes = Vec::with_capacity(payload.len() / 2);
    for pair in payload.as_bytes().chunks_exact(2) {
        let digits = std::str::from_utf8(pair).map_err(|_| malformed())?;
        bytes.push(u8::from_str_radix(digits, 16).map_err(|_| malformed())?);
    }
    Ok(bytes)
}

/// Bundle explicit project module sources for the engine compile and preview services.
pub fn bundle_source(source: String, read: impl FnMut(&str) -> Result<String>) -> Result<String> {
    use std::fmt::Write as _;

    let plan = parse_payload(&decode_source(&source)?)?;
    if plan.roots.is_empty() || plan.mappings.is_empty() {
        return Ok(source);
    }
    let sources = resolve_module_sources(plan, read)?;
    let mut out = Writer::new();
    out.u32(1);
    out.text(&source);
    out.u32(u32::try_from(sources.len()).map_err(|_| malformed())?);
    for (name, module_source) in sources {
        out.text(&name);
        out.text(&module_source);
    }
    let mut bundled = String::from("cyvfxbundle 1\n");
    for byte in out.finish() {
        let _ = write!(bundled, "{byte:02x}");
    }
    Ok(bundled)
}

fn resolve_module_sources(
    plan: ModulePlan,
    mut read: impl FnMut(&str) -> Result<String>,
) -> Result<Vec<(String, String)>> {
    let mappings: std::collections::HashMap<_, _> = plan.mappings.into_iter().collect();
    let mut queue = plan.roots;
    let mut queued: std::collections::HashSet<String> = queue.iter().cloned().collect();
    let mut sources = Vec::new();
    let mut cursor = 0;
    while cursor < queue.len() {
        let name = queue[cursor].clone();
        cursor += 1;
        let path = mappings.get(&name).ok_or_else(|| {
            Problem::new(
                "compile a VFX system",
                format!("module {name} has no asset path"),
            )
        })?;
        let module_source = read(path)?;
        let metadata = crate::vfx_module::inspect_source(&module_source)?;
        if metadata.name != *name {
            return Err(Problem::new(
                "compile a VFX system",
                format!("module {name} source declares {}", metadata.name),
            ));
        }
        for dependency in metadata.dependencies {
            if queued.insert(dependency.clone()) {
                queue.push(dependency);
            }
        }
        sources.push((name, module_source));
    }
    Ok(sources)
}

fn malformed() -> Problem {
    Problem::new("save a VFX document", "malformed VFX authoring payload")
}

fn read_count(input: &mut Reader<'_>) -> Result<u32> {
    let count = input.u32()?;
    if count > MAX_ITEMS {
        return Err(malformed());
    }
    Ok(count)
}

fn identifier_text(name: &str) -> Result<()> {
    if name.is_empty()
        || !name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(malformed());
    }
    Ok(())
}

fn identifier(input: &mut Reader<'_>) -> Result<()> {
    identifier_text(&input.text()?)
}

fn names(input: &mut Reader<'_>) -> Result<Vec<String>> {
    let mut names = Vec::new();
    for _ in 0..read_count(input)? {
        let name = input.text()?;
        identifier_text(&name)?;
        names.push(name);
    }
    Ok(names)
}

fn validate_emitter(input: &mut Reader<'_>, version: u32) -> Result<Vec<String>> {
    identifier(input)?;
    if input.u8()? > 1 {
        return Err(malformed());
    }
    identifier(input)?;
    let mut stages = [false; 6];
    for _ in 0..read_count(input)? {
        let stage = usize::from(input.u8()?);
        if stage >= stages.len() || stages[stage] {
            return Err(malformed());
        }
        stages[stage] = true;
        if !input.text()?.starts_with("cyvfxcanvas 1\n") {
            return Err(malformed());
        }
    }
    let modules = names(input)?;
    names(input)?;
    if version >= 2 {
        if input.u32()? == 0 {
            return Err(malformed());
        }
        for _ in 0..read_count(input)? {
            identifier(input)?;
            let kind = input.text()?;
            if !matches!(
                kind.as_str(),
                "float" | "vec2" | "vec3" | "vec4" | "int" | "bool"
            ) {
                return Err(malformed());
            }
            let minimum = f32::from_bits(input.u32()?);
            let maximum = f32::from_bits(input.u32()?);
            let tolerance = f32::from_bits(input.u32()?);
            if !minimum.is_finite()
                || !maximum.is_finite()
                || !tolerance.is_finite()
                || minimum > maximum
                || tolerance < 0.0
            {
                return Err(malformed());
            }
            if !matches!(
                input.text()?.as_str(),
                "Auto" | "Float32" | "Float16" | "Unorm8" | "Snorm16"
            ) {
                return Err(malformed());
            }
        }
    }
    Ok(modules)
}

fn validate_parameter(input: &mut Reader<'_>) -> Result<()> {
    identifier(input)?;
    let kind = input.text()?;
    if !matches!(
        kind.as_str(),
        "float" | "vec2" | "vec3" | "vec4" | "int" | "bool"
    ) {
        return Err(malformed());
    }
    for _ in 0..4 {
        if !f32::from_bits(input.u32()?).is_finite() {
            return Err(malformed());
        }
    }
    if input.u8()? > 1 {
        return Err(malformed());
    }
    Ok(())
}

fn parse_payload(bytes: &[u8]) -> Result<ModulePlan> {
    let mut input = Reader::new(bytes);
    let version = input.u32()?;
    if !(1..=3).contains(&version) {
        return Err(malformed());
    }
    identifier(&mut input)?;
    let mut module_names = Vec::new();
    for _ in 0..read_count(&mut input)? {
        for name in validate_emitter(&mut input, version)? {
            if !module_names.contains(&name) {
                module_names.push(name);
            }
        }
    }
    for _ in 0..read_count(&mut input)? {
        validate_parameter(&mut input)?;
    }
    if version >= 2 {
        for _ in 0..read_count(&mut input)? {
            identifier(&mut input)?;
            if input.u32()? == 0 || input.u32()? == 0 || input.u8()? > 1 {
                return Err(malformed());
            }
        }
    }
    let mut mappings = Vec::new();
    if version >= 3 {
        let mut names = std::collections::HashSet::new();
        let mut paths = std::collections::HashSet::new();
        for _ in 0..read_count(&mut input)? {
            let name = input.text()?;
            identifier_text(&name)?;
            if !names.insert(name.clone()) {
                return Err(malformed());
            }
            let path = input.text()?;
            crate::vfx_module::validate_reference(&path)?;
            if !paths.insert(path.clone()) {
                return Err(malformed());
            }
            mappings.push((name, path));
        }
        if module_names
            .iter()
            .any(|name| !mappings.iter().any(|(mapped, _)| mapped == name))
        {
            return Err(Problem::new(
                "save a VFX document",
                "a referenced module has no asset path",
            ));
        }
    }
    if input.remaining() != 0 {
        return Err(malformed());
    }
    Ok(ModulePlan {
        roots: module_names,
        mappings,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::codec::Writer;

    fn source(bytes: &[u8]) -> String {
        use std::fmt::Write as _;
        let mut source = String::from("cyvfxdoc 1\n");
        for byte in bytes {
            write!(source, "{byte:02x}").unwrap();
        }
        source
    }

    #[test]
    fn reference_cannot_escape_the_project() {
        assert!(validate_reference("effects/smoke.cyvfxdoc").is_ok());
        assert!(validate_reference("../outside.cyvfxdoc").is_err());
        assert!(validate_reference("/outside.cyvfxdoc").is_err());
        assert!(validate_reference("effects/smoke.cygraph").is_err());
    }

    #[test]
    fn envelope_refuses_truncated_or_foreign_payloads() {
        assert!(
            validate_source("cyvfxdoc 1\n0100000006000000737061726b730000000000000000").is_ok()
        );
        assert!(validate_source("cyvfxdoc 1\n0102").is_err());
        assert!(validate_source("cyvfxdoc 1\n0").is_err());
        assert!(validate_source("cyvfxdoc 2\n0102").is_err());
        assert!(validate_source("cyvfxdoc 1\nxyz!").is_err());
    }

    #[test]
    fn version_two_declarations_are_validated_before_save() {
        let mut out = Writer::new();
        out.u32(2);
        out.text("sparks");
        out.u32(1);
        out.text("smoke");
        out.u8(0);
        out.text("Sprite");
        out.u32(0);
        out.u32(0);
        out.u32(0);
        out.u32(2048);
        out.u32(1);
        out.text("position");
        out.text("vec3");
        out.u32((-100.0_f32).to_bits());
        out.u32(100.0_f32.to_bits());
        out.u32(0.01_f32.to_bits());
        out.text("Auto");
        out.u32(0);
        out.u32(1);
        out.text("on_death");
        out.u32(128);
        out.u32(2);
        out.u8(0);
        let mut bytes = out.finish();
        assert!(validate_source(&source(&bytes)).is_ok());
        *bytes.last_mut().unwrap() = 2;
        assert!(validate_source(&source(&bytes)).is_err());
    }

    #[test]
    fn mapped_module_source_is_bundled_for_engine_requests() {
        let mut out = Writer::new();
        out.u32(3);
        out.text("sparks");
        out.u32(1);
        out.text("smoke");
        out.u8(0);
        out.text("Sprite");
        out.u32(0);
        out.u32(1);
        out.text("shared_drag");
        out.u32(0);
        out.u32(1024);
        out.u32(1);
        out.text("velocity");
        out.text("vec3");
        out.u32(0.0_f32.to_bits());
        out.u32(100.0_f32.to_bits());
        out.u32(0.0_f32.to_bits());
        out.text("Auto");
        out.u32(0);
        out.u32(0);
        out.u32(2);
        out.text("shared_drag");
        out.text("effects/shared_drag.cyvfxmodule");
        out.text("unused");
        out.text("effects/missing.cyvfxmodule");
        let document = source(&out.finish());
        let fixture = include_str!(
            "../../../../samples/05b-editor-window/project/effects/shared_drag.cyvfxmodule"
        );
        let bundled = bundle_source(document.clone(), |path| {
            assert_eq!(path, "effects/shared_drag.cyvfxmodule");
            Ok(fixture.into())
        })
        .unwrap();
        assert!(bundled.starts_with("cyvfxbundle 1\n"));
        assert!(bundled.len() > document.len());
        assert!(bundle_source(document, |_| Err(malformed())).is_err());
    }
}
