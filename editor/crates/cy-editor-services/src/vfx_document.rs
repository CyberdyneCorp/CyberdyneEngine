// SPDX-License-Identifier: MIT
//! Project storage contract for editable VFX documents.

use std::path::{Component, Path};

use cy_editor_core::codec::Reader;
use cy_editor_core::problem::{Problem, Result};

const MAX_ITEMS: u32 = 4096;

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
    validate_payload(&bytes)
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

fn identifier(input: &mut Reader<'_>) -> Result<()> {
    let name = input.text()?;
    if name.is_empty()
        || !name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(malformed());
    }
    Ok(())
}

fn names(input: &mut Reader<'_>) -> Result<()> {
    for _ in 0..read_count(input)? {
        identifier(input)?;
    }
    Ok(())
}

fn validate_emitter(input: &mut Reader<'_>, version: u32) -> Result<()> {
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
    names(input)?;
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
    Ok(())
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

fn validate_payload(bytes: &[u8]) -> Result<()> {
    let mut input = Reader::new(bytes);
    let version = input.u32()?;
    if !(1..=3).contains(&version) {
        return Err(malformed());
    }
    identifier(&mut input)?;
    for _ in 0..read_count(&mut input)? {
        validate_emitter(&mut input, version)?;
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
    if version >= 3 {
        let mut names = std::collections::HashSet::new();
        let mut paths = std::collections::HashSet::new();
        for _ in 0..read_count(&mut input)? {
            let name = input.text()?;
            if name.is_empty()
                || !name
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
                || !names.insert(name)
            {
                return Err(malformed());
            }
            let path = input.text()?;
            crate::vfx_module::validate_reference(&path)?;
            if !paths.insert(path) {
                return Err(malformed());
            }
        }
    }
    if input.remaining() != 0 {
        return Err(malformed());
    }
    Ok(())
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
}
