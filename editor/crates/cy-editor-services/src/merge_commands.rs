// SPDX-License-Identifier: MIT
//! Registered commands for semantic comparison and conflict resolution.

use cy_editor_commands::{Command, EffectClass, Metadata, Outcome, ParameterSpec, Registry};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

/// Register the source-revision comparison and resolution surface shared by UI, scripts, and MCP.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(Command::new(
        Metadata::new(
            "document.merge-start",
            "Compare and Merge Revisions",
            "Document",
            "Loads two source-control revisions of the active authored document and starts an identity-keyed three-way merge.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "base",
            ValueKind::Text,
            "The provider revision from which local and incoming work diverged.",
        ))
        .with(ParameterSpec::required(
            "incoming",
            ValueKind::Text,
            "The provider revision to merge into the open local document.",
        )),
        |context, arguments| {
            let summary = context.start_semantic_merge(
                arguments.text("base").unwrap_or_default(),
                arguments.text("incoming").unwrap_or_default(),
            )?;
            Ok(Outcome::new(summary))
        },
    ))?;
    registry.register(Command::new(
        Metadata::new(
            "document.merge-resolve",
            "Resolve Merge Conflict",
            "Document",
            "Chooses local, incoming, or a typed replacement for one semantic conflict; the final decision commits the whole merge as one transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "index",
            ValueKind::Int,
            "The zero-based conflict index shown by the Merge panel.",
        ))
        .with(ParameterSpec::required(
            "choice",
            ValueKind::Text,
            "One of local, incoming, or replacement.",
        ))
        .with(ParameterSpec::optional(
            "replacement",
            ValueKind::Text,
            "For replacement: a typed literal such as text:value, int:7, float:1.5, bool:true, or nil.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let raw_index = arguments
                .get("index")
                .and_then(Value::as_int)
                .unwrap_or(-1);
            let index = usize::try_from(raw_index).map_err(|_| {
                Problem::new("resolve a merge conflict", "the conflict index cannot be negative")
            })?;
            let choice = arguments.text("choice").unwrap_or_default();
            let replacement = if choice == "replacement" {
                Some(parse_literal(
                    arguments.text("replacement").unwrap_or_default(),
                )?)
            } else {
                None
            };
            let summary = context.resolve_semantic_merge(index, choice, replacement)?;
            Ok(Outcome::new(summary))
        },
    ))?;
    Ok(())
}

fn parse_literal(text: &str) -> Result<Value> {
    if text == "nil" {
        return Ok(Value::Nil);
    }
    let (kind, payload) = text.split_once(':').ok_or_else(|| {
        Problem::new(
            "parse a merge replacement",
            "the replacement has no type prefix",
        )
        .with_remedy("use text:value, bool:true, int:7, float:1.5, double:1.5, or nil")
    })?;
    match kind {
        "text" => Ok(Value::Text(payload.to_string())),
        "bool" => payload
            .parse::<bool>()
            .map(Value::Bool)
            .map_err(|_| invalid_literal(kind, payload)),
        "int" => payload
            .parse::<i64>()
            .map(Value::Int)
            .map_err(|_| invalid_literal(kind, payload)),
        "float" => payload
            .parse::<f32>()
            .map(Value::Float)
            .map_err(|_| invalid_literal(kind, payload)),
        "double" => payload
            .parse::<f64>()
            .map(Value::Double)
            .map_err(|_| invalid_literal(kind, payload)),
        other => Err(Problem::new(
            "parse a merge replacement",
            format!("{other:?} is not a supported replacement type"),
        )),
    }
}

fn invalid_literal(kind: &str, payload: &str) -> Problem {
    Problem::new(
        "parse a merge replacement",
        format!("{payload:?} is not a valid {kind}"),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn replacement_literals_are_typed_before_the_document_sees_them() {
        assert_eq!(parse_literal("int:7").unwrap(), Value::Int(7));
        assert_eq!(
            parse_literal("text:int:7").unwrap(),
            Value::Text("int:7".into())
        );
        assert_eq!(parse_literal("nil").unwrap(), Value::Nil);
        assert!(parse_literal("float:nope").is_err());
    }
}
