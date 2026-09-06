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
//! # Where the exclusion lives, stated plainly
//!
//! The specification says an unsuitable command "SHALL be **excluded by a declared property on the
//! command**, and the exclusion SHALL state a reason — never by omission from a list".
//!
//! `cy_editor_commands::Metadata` has no such property, and adding one is a single
//! `Option<String>` field in a crate this change does not own. So [`Exclusions`] is that property
//! held beside the registry rather than on it: an exclusion cannot be constructed without a reason,
//! every exclusion is reported by [`project`] rather than making a tool vanish, and
//! [`ToolDescriptor::exclusion`] carries it — which satisfies the two things the requirement is
//! actually for ("the exclusion and its reason", "the reason SHALL be reportable").
//!
//! What it does not yet satisfy is locality: the reason lives next to the projection instead of next
//! to the command it describes, so a contributor removing a command has two places to look. The fix
//! is `Metadata::agent_exclusion: Option<String>` and deleting this type; it is one field and one
//! function, and it belongs in the change that owns `cy-editor-commands`.

use std::collections::BTreeMap;

use cy_editor_commands::metadata::{EffectClass, Metadata, ParameterSpec};
use cy_editor_commands::registry::Registry;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

/// Why a command is not offered to an agent.
///
/// The reason is required and is checked for length, because "excluded" with no explanation is
/// exactly the state the requirement forbids: an agent that is told a command exists and cannot be
/// invoked learns nothing it can act on.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Exclusion {
    /// The command's identifier.
    pub command: String,
    /// Why, in a sentence a caller can act on.
    pub reason: String,
}

impl Exclusion {
    /// Declare an exclusion, refusing one whose reason says nothing.
    pub fn new(command: impl Into<String>, reason: impl Into<String>) -> Result<Self> {
        let command = command.into();
        let reason = reason.into();
        if reason.trim().len() < MINIMUM_REASON {
            return Err(Problem::new(
                format!("exclude {command}"),
                "the exclusion states no reason an agent could act on",
            )
            .with_remedy(
                "say why the command is unsuitable — what it would do that an agent must not, or \
                 what it needs that an agent cannot supply",
            ));
        }
        Ok(Self { command, reason })
    }
}

/// The shortest exclusion reason this crate accepts. Twenty characters is not a quality bar; it is a
/// floor that catches the two failures that actually occur — an empty string, and the word "no".
const MINIMUM_REASON: usize = 20;

/// The commands an agent is not offered, and why.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Exclusions {
    by_command: BTreeMap<String, String>,
}

impl Exclusions {
    /// Nothing excluded, which is the default: a command is a tool unless somebody says otherwise.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Declare one.
    pub fn declare(&mut self, exclusion: Exclusion) {
        self.by_command.insert(exclusion.command, exclusion.reason);
    }

    /// The reason a command is excluded, when it is.
    #[must_use]
    pub fn reason(&self, command: &str) -> Option<&str> {
        self.by_command.get(command).map(String::as_str)
    }

    /// How many are declared.
    #[must_use]
    pub fn len(&self) -> usize {
        self.by_command.len()
    }

    /// Whether nothing is excluded.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.by_command.is_empty()
    }
}

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
                "{}({parameters}) [{}] — {}",
                self.name,
                self.effect.name(),
                self.description
            ),
        }
    }

    fn of(metadata: &Metadata, exclusion: Option<&str>) -> Self {
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
            exclusion: exclusion.map(ToString::to_string),
        }
    }
}

/// Project the whole registry.
///
/// In identifier order, because the registry is: two listings of the same editor are the same
/// listing, which is what lets an agent cache one and lets a test compare two.
#[must_use]
pub fn project(registry: &Registry, exclusions: &Exclusions) -> Vec<ToolDescriptor> {
    registry
        .all()
        .map(|metadata| ToolDescriptor::of(metadata, exclusions.reason(&metadata.id)))
        .collect()
}

/// The one tool a caller asked for, or nothing.
#[must_use]
pub fn project_one(
    registry: &Registry,
    exclusions: &Exclusions,
    command: &str,
) -> Option<ToolDescriptor> {
    registry
        .metadata(command)
        .map(|metadata| ToolDescriptor::of(metadata, exclusions.reason(command)))
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
        let tools = project(&registry(), &Exclusions::new());
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
        let tools = project(&registry(), &Exclusions::new());
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
        let tools = project(&registry, &Exclusions::new());
        assert!(tools.iter().any(|tool| tool.name == "assets.delete"));

        registry = Registry::new();
        assert!(project(&registry, &Exclusions::new()).is_empty());
    }

    #[test]
    fn an_exclusion_is_reported_rather_than_making_the_tool_vanish() {
        let mut exclusions = Exclusions::new();
        exclusions.declare(
            Exclusion::new(
                "assets.delete",
                "deleting from disk needs a human at the interface, because undo cannot put the \
                 file back",
            )
            .unwrap(),
        );
        let tools = project(&registry(), &exclusions);
        let deleting = tools
            .iter()
            .find(|tool| tool.name == "assets.delete")
            .unwrap();
        assert!(!deleting.is_offered());
        assert!(
            deleting
                .exclusion
                .as_deref()
                .unwrap()
                .contains("undo cannot")
        );
        assert!(deleting.summary().contains("[excluded]"));
    }

    #[test]
    fn an_exclusion_with_no_reason_cannot_be_declared() {
        let refused = Exclusion::new("assets.delete", "no").unwrap_err();
        assert!(refused.remedy.as_deref().unwrap().contains("why"));
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
        let projected = project_one(&registry, &Exclusions::new(), "scene.rename").unwrap();
        assert_eq!(
            projected.parameters[0].description,
            registry.metadata("scene.rename").unwrap().parameters[0].description
        );
    }
}
