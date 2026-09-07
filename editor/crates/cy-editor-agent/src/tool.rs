//! The command registry, projected as tools.
//!
//! `editor-agent-interface`, "Tools are a projection of the command registry":
//!
//! > Every registered command SHALL be exposed as a tool, automatically, with its identifier,
//! > description, typed parameters and availability predicate taken from the command's own metadata.
//! > The projection SHALL NOT be a hand-maintained list. A command added for a menu SHALL be an
//! > agent tool without further work, a renamed command SHALL NOT leave a stale tool behind, and a
//! > removed command SHALL disappear.
//!
//! [`project`] iterates the registry and returns one descriptor per command. There is no list of
//! exposed tools anywhere in this crate, and there is no place to add one: a command that is not in
//! the registry is not a tool, and every command that is in it is.
//!
//! # The exclusion is on the command, which is where it moved at M5.5
//!
//! The specification says an unsuitable command "SHALL be **excluded by a declared property on the
//! command**, and the exclusion SHALL state a reason — never by omission from a list".
//!
//! At M5 that property did not exist, so this module held the exclusions beside the registry and its
//! own note said what the fix was: "`Metadata::agent_exclusion: Option<String>` and deleting this
//! type; it is one field and one function, and it belongs in the change that owns
//! `cy-editor-commands`." This is that change, and the field is there. A contributor removing a
//! command now removes its exclusion with it, because they are the same object, and there is no
//! table left over to go stale.
//!
//! `cy_editor_commands::Metadata::validate` refuses an exclusion whose reason says nothing, so the
//! registry itself is what enforces "the reason SHALL be reportable" — the same way it enforces a
//! description a caller can act on.
//!
//! # An excluded command is reported, not hidden
//!
//! [`project`] returns it with [`ToolDescriptor::exclusion`] set. An agent that could not see a
//! command at all could not be told why it may not use it, and would spend its next turn looking for
//! the thing it already has an answer about.

use cy_editor_commands::metadata::{EffectClass, Metadata, ParameterSpec};
use cy_editor_commands::registry::Registry;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

/// One parameter, as a caller that cannot see the interface reads it.
#[derive(Clone, PartialEq, Debug)]
pub struct ParameterDescriptor {
    /// The name the argument is keyed by.
    pub name: String,
    /// What it holds.
    pub kind: ValueKind,
    /// What it *means*. Taken from the command's own metadata, which the registry refuses to accept
    /// without — so a tool description cannot be less informative than the command it projects.
    pub description: String,
    /// Whether the invocation must supply it.
    pub required: bool,
    /// What happens when it is omitted. `None` only for a required parameter.
    pub default: Option<Value>,
}

impl ParameterDescriptor {
    fn of(spec: &ParameterSpec) -> Self {
        Self {
            name: spec.name.clone(),
            kind: spec.kind,
            description: spec.description.clone(),
            required: spec.required,
            default: spec.default.clone(),
        }
    }
}

/// One command, as a tool.
///
/// Every field is derived from the command's own metadata. Nothing here is written by hand beside
/// the command it describes, which is one of the twelve patterns
/// `editor-agent-interface`'s "Forbidden agent interface patterns" names.
#[derive(Clone, PartialEq, Debug)]
pub struct ToolDescriptor {
    /// The command's identifier, which is what an invocation names.
    pub name: String,
    /// What a menu would show. Present for a caller rendering a list to a person.
    pub title: String,
    /// Where it is grouped.
    pub category: String,
    /// What it does, written for a caller that cannot see the interface.
    pub description: String,
    /// Its typed parameters.
    pub parameters: Vec<ParameterDescriptor>,
    /// What class of consequence invoking it has.
    ///
    /// "WHEN an agent lists the available tools THEN each SHALL state its effect class." It is on
    /// the descriptor rather than only in the prose, so a caller reasons about consequence before
    /// invoking rather than parsing a sentence.
    pub effect: EffectClass,
    /// Whether invoking it needs a human's confirmation or a deliberate grant.
    pub needs_confirmation: bool,
    /// Why it is not offered, when it is not. A tool with an exclusion is REPORTED rather than
    /// omitted; see the module note.
    pub exclusion: Option<String>,
    /// Whether the command works out its effect class from the invocation rather than declaring
    /// one.
    ///
    /// When this is true, [`ToolDescriptor::effect`] is the **worst case** and a particular
    /// invocation may be admitted as something narrower — writing a source file the editor can
    /// restore, say. Stated rather than left implicit, because an agent that budgeted its
    /// confirmations against the declared class alone would ask a human about work that undo
    /// already covers.
    pub effect_may_narrow: bool,
}

impl ToolDescriptor {
    /// Whether an agent may invoke this at all.
    #[must_use]
    pub fn is_offered(&self) -> bool {
        self.exclusion.is_none()
    }

    /// A one-line rendering, for a listing a person or a machine reads.
    #[must_use]
    pub fn summary(&self) -> String {
        let parameters = self
            .parameters
            .iter()
            .map(|parameter| {
                let marker = if parameter.required { "" } else { "?" };
                format!("{}{marker}: {}", parameter.name, parameter.kind)
            })
            .collect::<Vec<_>>()
            .join(", ");
        match &self.exclusion {
            Some(reason) => format!("{}({parameters}) [excluded] — {reason}", self.name),
            None => format!(
                "{}({parameters}) [{}{}] — {}",
                self.name,
                self.effect.name(),
                if self.effect_may_narrow {
                    ", at most"
                } else {
                    ""
                },
                self.description
            ),
        }
    }

    fn of(metadata: &Metadata, effect_may_narrow: bool) -> Self {
        Self {
            name: metadata.id.clone(),
            title: metadata.label.clone(),
            category: metadata.category.clone(),
            description: metadata.description.clone(),
            parameters: metadata
                .parameters
                .iter()
                .map(ParameterDescriptor::of)
                .collect(),
            effect: metadata.effect,
            needs_confirmation: metadata.effect.needs_confirmation(),
            exclusion: metadata.agent_exclusion().map(ToString::to_string),
            effect_may_narrow,
        }
    }
}

/// Project the whole registry.
///
/// In identifier order, because the registry is: two listings of the same editor are the same
/// listing, which is what lets an agent cache one and lets a test compare two.
#[must_use]
pub fn project(registry: &Registry) -> Vec<ToolDescriptor> {
    registry
        .all()
        .map(|metadata| ToolDescriptor::of(metadata, registry.computes_effect(&metadata.id)))
        .collect()
}

/// The one tool a caller asked for, or nothing.
#[must_use]
pub fn project_one(registry: &Registry, command: &str) -> Option<ToolDescriptor> {
    registry
        .metadata(command)
        .map(|metadata| ToolDescriptor::of(metadata, registry.computes_effect(command)))
}

// --- The wire's values, turned into the registry's -------------------------------------------------
//
// `crate::transport::AgentRequest::Invoke` carries arguments as RENDERED strings, and its own
// comment says why: a transport that carried `cy_editor_core::Value` would make the editor's value
// type part of the protocol, which is the coupling the seam exists to prevent. This is the
// conversion that comment promises — "the projection above converts" — and it lives here, beside the
// projection, rather than in whichever transport happens to be first.

/// Turn one rendered argument into the value the parameter declares.
///
/// The rendering is [`Value`]'s own `Display`, so this is its inverse and the two are tested against
/// each other. A value of the wrong shape is refused with what the parameter means, because a caller
/// that typed a name where an entity belonged learns nothing from "invalid argument".
pub fn coerce(kind: ValueKind, text: &str) -> Result<Value> {
    let trimmed = text.trim();
    let refuse = |wanted: &str| {
        Problem::new(
            format!("read {text:?} as {kind}"),
            format!("it is not {wanted}"),
        )
        .with_remedy(format!("supply {wanted}"))
    };
    match kind {
        ValueKind::Nil => Ok(Value::Nil),
        ValueKind::Text => Ok(Value::Text(text.to_string())),
        ValueKind::Bool => match trimmed {
            "true" | "1" | "yes" => Ok(Value::Bool(true)),
            "false" | "0" | "no" => Ok(Value::Bool(false)),
            _ => Err(refuse("true or false")),
        },
        ValueKind::Int => trimmed
            .parse()
            .map(Value::Int)
            .map_err(|_| refuse("a whole number")),
        ValueKind::Float => trimmed
            .parse()
            .map(Value::Float)
            .map_err(|_| refuse("a number")),
        ValueKind::Double => trimmed
            .parse()
            .map(Value::Double)
            .map_err(|_| refuse("a number")),
        ValueKind::Entity => trimmed
            .trim_start_matches("entity#")
            .parse()
            .map(Value::Entity)
            .map_err(|_| refuse("an entity identity, as a command's result printed it")),
        ValueKind::Bytes => Ok(Value::Bytes(text.as_bytes().to_vec())),
        ValueKind::Vec2 => lanes::<2>(trimmed)
            .map(Value::Vec2)
            .ok_or_else(|| refuse("two numbers")),
        ValueKind::Vec3 => lanes::<3>(trimmed)
            .map(Value::Vec3)
            .ok_or_else(|| refuse("three numbers, as \"(x, y, z)\"")),
        ValueKind::Vec4 => lanes::<4>(trimmed)
            .map(Value::Vec4)
            .ok_or_else(|| refuse("four numbers")),
        ValueKind::Quat => lanes::<4>(trimmed)
            .map(Value::Quat)
            .ok_or_else(|| refuse("four numbers, in x, y, z, w order")),
    }
}

/// The lanes of a vector, however the caller spelled the separators.
///
/// `"(1, 2, 3)"`, `"1,2,3"` and `"1 2 3"` are all accepted, because all three are what somebody
/// types and refusing two of them would be a rule with no purpose behind it.
fn lanes<const N: usize>(text: &str) -> Option<[f32; N]> {
    let inner = text.trim_start_matches('(').trim_end_matches(')');
    let parsed: Vec<f32> = inner
        .split([',', ' '])
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .filter_map(|part| part.parse().ok())
        .collect();
    parsed.try_into().ok()
}

/// Build a command's arguments from rendered name and value pairs.
///
/// Each value is read as the kind the command's own parameter declares, so the *command* decides
/// what its arguments mean and the transport carries text. A name the command does not have is
/// refused here rather than passed through, with the names it does have — the registry would refuse
/// it a moment later, and refusing it here means the message names the parameter rather than the
/// invocation.
pub fn arguments(
    registry: &Registry,
    command: &str,
    supplied: &[(String, String)],
) -> Result<cy_editor_commands::registry::Arguments> {
    let metadata = registry.metadata(command).ok_or_else(|| {
        Problem::not_found(format!("a command named {command:?}"))
            .with_remedy("list the tools to see what there is")
    })?;
    let mut arguments = cy_editor_commands::registry::Arguments::new();
    for (name, text) in supplied {
        let parameter = metadata
            .parameters
            .iter()
            .find(|parameter| parameter.name == *name)
            .ok_or_else(|| {
                Problem::new(
                    format!("invoke {command}"),
                    format!("it has no parameter named {name:?}"),
                )
                .with_remedy(format!(
                    "its parameters are: {}",
                    metadata
                        .parameters
                        .iter()
                        .map(|parameter| parameter.name.as_str())
                        .collect::<Vec<_>>()
                        .join(", ")
                ))
            })?;
        arguments = arguments.with(name.clone(), coerce(parameter.kind, text)?);
    }
    Ok(arguments)
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_commands::context::{CommandContext, Outcome};
    use cy_editor_commands::registry::{Arguments, Command};

    fn registry() -> Registry {
        let mut registry = Registry::new();
        registry
            .register(Command::new(
                Metadata::new(
                    "scene.create-entity",
                    "Create Entity",
                    "Scene",
                    "Creates an empty entity in the active document, optionally under a parent.",
                    EffectClass::ReversibleMutation,
                )
                .with(ParameterSpec::optional(
                    "parent",
                    ValueKind::Entity,
                    "The entity to create the new one under; a root when omitted.",
                    Value::Entity(0),
                )),
                |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::new("Created")),
            ))
            .unwrap();
        registry
            .register(Command::new(
                Metadata::new(
                    "assets.delete",
                    "Delete Asset",
                    "Assets",
                    "Deletes an asset from disk. This cannot be undone from within the editor.",
                    EffectClass::IrreversibleMutation,
                ),
                |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::new("Deleted")),
            ))
            .unwrap();
        registry
    }

    #[test]
    fn a_command_registered_for_a_menu_is_a_tool_with_no_further_work() {
        let tools = project(&registry());
        assert_eq!(tools.len(), 2);
        let created = tools
            .iter()
            .find(|tool| tool.name == "scene.create-entity")
            .unwrap();
        assert_eq!(created.parameters.len(), 1);
        assert_eq!(created.parameters[0].name, "parent");
        assert!(!created.parameters[0].required);
        assert!(created.parameters[0].default.is_some());
        assert!(created.is_offered());
    }

    #[test]
    fn every_tool_states_its_effect_class_before_it_is_invoked() {
        let tools = project(&registry());
        let deleting = tools
            .iter()
            .find(|tool| tool.name == "assets.delete")
            .unwrap();
        assert_eq!(deleting.effect, EffectClass::IrreversibleMutation);
        assert!(deleting.needs_confirmation);
        assert!(deleting.summary().contains("[irreversible-mutation]"));
    }

    #[test]
    fn the_projection_follows_the_registry_rather_than_a_list() {
        // A renamed command leaves no stale tool and a removed one disappears, because there is
        // nothing to leave behind: the projection is computed, not maintained.
        let mut registry = registry();
        let tools = project(&registry);
        assert!(tools.iter().any(|tool| tool.name == "assets.delete"));

        registry = Registry::new();
        assert!(project(&registry).is_empty());
    }

    #[test]
    fn an_exclusion_is_reported_rather_than_making_the_tool_vanish() {
        let mut registry = Registry::new();
        registry
            .register(Command::new(
                Metadata::new(
                    "file.open-dialog",
                    "Open...",
                    "File",
                    "Opens the platform's file chooser so a person can pick a project to open.",
                    EffectClass::Read,
                )
                .not_for_agents(
                    "it opens a file chooser, which needs a person at the interface to answer; \
                     invoke file.open with a path instead",
                ),
                |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::default()),
            ))
            .unwrap();
        let tools = project(&registry);
        let dialog = &tools[0];
        assert!(!dialog.is_offered());
        assert!(dialog.exclusion.as_deref().unwrap().contains("file.open"));
        assert!(dialog.summary().contains("[excluded]"));
    }

    #[test]
    fn an_exclusion_with_no_reason_cannot_be_registered() {
        // The registry refuses it, which is what makes "never by omission from a list" hold: there
        // is no list, and the property cannot be set to something meaningless.
        let mut registry = Registry::new();
        let refused = registry
            .register(Command::new(
                Metadata::new(
                    "file.open-dialog",
                    "Open...",
                    "File",
                    "Opens the platform's file chooser so a person can pick a project to open.",
                    EffectClass::Read,
                )
                .not_for_agents("no"),
                |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::default()),
            ))
            .unwrap_err();
        assert!(refused.remedy.as_deref().unwrap().contains("why"));
    }

    #[test]
    fn a_command_that_computes_its_effect_says_so_in_its_listing() {
        let mut registry = Registry::new();
        registry
            .register(
                Command::new(
                    Metadata::new(
                        "source.write",
                        "Write Source File",
                        "Source",
                        "Writes a project source file; reversible when the editor can read what \
                         it held.",
                        EffectClass::IrreversibleMutation,
                    ),
                    |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::default()),
                )
                .effect_when(|_, _| EffectClass::ReversibleMutation),
            )
            .unwrap();
        let tool = project_one(&registry, "source.write").unwrap();
        assert!(tool.effect_may_narrow);
        assert_eq!(tool.effect, EffectClass::IrreversibleMutation);
        assert!(tool.summary().contains("[irreversible-mutation, at most]"));
    }

    #[test]
    fn a_description_cannot_go_stale_because_it_is_never_copied() {
        // "WHEN a command's parameters change THEN its tool description SHALL change with it,
        // without a separate edit." Nothing here stores a description; changing the command's
        // metadata changes the projection by construction.
        let mut registry = Registry::new();
        registry
            .register(Command::new(
                Metadata::new(
                    "scene.rename",
                    "Rename",
                    "Scene",
                    "Renames the selected node, which is what the hierarchy shows.",
                    EffectClass::ReversibleMutation,
                )
                .with(ParameterSpec::required(
                    "name",
                    ValueKind::Text,
                    "The new name, which must not be empty.",
                )),
                |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::default()),
            ))
            .unwrap();
        let projected = project_one(&registry, "scene.rename").unwrap();
        assert_eq!(
            projected.parameters[0].description,
            registry.metadata("scene.rename").unwrap().parameters[0].description
        );
    }
}
