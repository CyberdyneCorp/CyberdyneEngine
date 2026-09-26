// SPDX-License-Identifier: MIT
//! Project storage contract for reusable VFX stage modules.

use std::path::{Component, Path};

use cy_editor_core::codec::Reader;
use cy_editor_core::problem::{Problem, Result};

const MAX_ITEMS: u32 = 4096;

/// Transaction kind prefix for a saved module.
pub const DOMAIN_PREFIX: &str = "vfx_module:";

fn invalid(reason: &str) -> Problem {
    Problem::new("save a VFX module", reason)
}

fn identifier(value: &str) -> Result<()> {
    if value.is_empty()
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(invalid("invalid module identifier"));
    }
    Ok(())
}

fn count(input: &mut Reader<'_>) -> Result<u32> {
    let value = input.u32()?;
    if value > MAX_ITEMS {
        return Err(invalid("too many module entries"));
    }
    Ok(value)
}

/// Check that an asset path stays inside the project.
pub fn validate_reference(reference: &str) -> Result<()> {
    let path = Path::new(reference);
    if path
        .extension()
        .is_none_or(|extension| extension != "cyvfxmodule")
        || !path
            .components()
            .all(|component| matches!(component, Component::Normal(_)))
    {
        return Err(invalid("expected a project-relative .cyvfxmodule path"));
    }
    Ok(())
}

/// Validate the versioned module envelope before saving.
pub fn validate_source(source: &str) -> Result<()> {
    let payload = source
        .strip_prefix("cyvfxmodule 1\n")
        .ok_or_else(|| invalid("expected cyvfxmodule 1 source"))?;
    if payload.is_empty()
        || payload.len() % 2 != 0
        || !payload.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return Err(invalid("invalid hexadecimal module payload"));
    }
    let bytes = payload
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let digits = std::str::from_utf8(pair).map_err(|_| invalid("invalid payload"))?;
            u8::from_str_radix(digits, 16).map_err(|_| invalid("invalid payload"))
        })
        .collect::<Result<Vec<_>>>()?;
    let mut input = Reader::new(&bytes);
    if input.u32()? != 1 {
        return Err(invalid("unsupported module version"));
    }
    let name = input.text()?;
    identifier(&name)?;
    if input.u8()? > 5 {
        return Err(invalid("unknown module stage"));
    }
    let mut inputs = std::collections::HashSet::new();
    for _ in 0..count(&mut input)? {
        let attribute = input.text()?;
        identifier(&attribute)?;
        if !inputs.insert(attribute) {
            return Err(invalid("duplicate module input"));
        }
        if !matches!(
            input.text()?.as_str(),
            "float" | "vec2" | "vec3" | "vec4" | "int" | "bool"
        ) {
            return Err(invalid("invalid module input type"));
        }
    }
    let mut dependencies = std::collections::HashSet::new();
    for _ in 0..count(&mut input)? {
        let dependency = input.text()?;
        identifier(&dependency)?;
        if dependency == name || !dependencies.insert(dependency) {
            return Err(invalid("self or duplicate module dependency"));
        }
    }
    if !input
        .text()?
        .starts_with(&format!("cyvfxcanvas 1\nmodule {name}\n"))
        || input.remaining() != 0
    {
        return Err(invalid("invalid module canvas or trailing data"));
    }
    Ok(())
}
