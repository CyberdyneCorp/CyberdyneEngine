//! What a command says about itself, and the validation that makes it worth reading.

use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

/// What class of consequence invoking a command has.
///
/// `editor-agent-interface`: "Every command SHALL carry an **effect class** — read, reversible
/// mutation, irreversible mutation, or external effect — and the class SHALL be part of the tool's
/// description, so an agent can reason about consequence **before** invoking rather than
/// discovering it afterwards."
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum EffectClass {
    /// Changes nothing. Safe to invoke to find something out.
    Read,
    /// Changes project state through a transaction, so undo reverses it.
    ReversibleMutation,
    /// Changes something undo cannot reverse: deleting an asset from disk, overwriting a source.
    IrreversibleMutation,
    /// Reaches outside the editor: publishing, invoking a service, sending a message.
    ExternalEffect,
}

impl EffectClass {
    /// The name a caller reads in a tool listing.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            EffectClass::Read => "read",
            EffectClass::ReversibleMutation => "reversible-mutation",
            EffectClass::IrreversibleMutation => "irreversible-mutation",
            EffectClass::ExternalEffect => "external-effect",
        }
    }

    /// What a caller should understand by the class, in one sentence.
    #[must_use]
    pub const fn consequence(self) -> &'static str {
        match self {
            EffectClass::Read => "changes nothing",
            EffectClass::ReversibleMutation => "changes the project, and undo reverses it",
            EffectClass::IrreversibleMutation => "changes something undo cannot put back",
            EffectClass::ExternalEffect => "has an effect outside the editor",
        }
    }

    /// Whether an agent must obtain explicit human confirmation before invoking this.
    ///
    /// "An operation that cannot be undone ... SHALL require **explicit human confirmation** when
    /// invoked by an agent, unless the connection has been granted that effect class deliberately
    /// and for a stated duration. Reversible edits SHALL NOT require confirmation. **Undo is the
    /// confirmation**, and prompting for ordinary work trains the human to approve without reading."
    #[must_use]
    pub const fn needs_confirmation(self) -> bool {
        matches!(
            self,
            EffectClass::IrreversibleMutation | EffectClass::ExternalEffect
        )
    }

    /// Every class, for a listing.
    pub const ALL: [EffectClass; 4] = [
        EffectClass::Read,
        EffectClass::ReversibleMutation,
        EffectClass::IrreversibleMutation,
        EffectClass::ExternalEffect,
    ];
}

/// One typed parameter, with its meaning.
#[derive(Clone, PartialEq, Debug)]
pub struct ParameterSpec {
    /// The name arguments are keyed by.
    pub name: String,
    /// What it holds.
    pub kind: ValueKind,
    /// What it *means* — not what it is called. "The node to move", not "node".
    pub description: String,
    /// Whether the command can be invoked without it.
    pub required: bool,
    /// The value used when it is not supplied. Only meaningful for an optional parameter.
    pub default: Option<Value>,
}

impl ParameterSpec {
    /// A required parameter.
    pub fn required(
        name: impl Into<String>,
        kind: ValueKind,
        description: impl Into<String>,
    ) -> Self {
        Self {
            name: name.into(),
            kind,
            description: description.into(),
            required: true,
            default: None,
        }
    }

    /// An optional parameter with the value used when it is absent.
    pub fn optional(
        name: impl Into<String>,
        kind: ValueKind,
        description: impl Into<String>,
        default: Value,
    ) -> Self {
        Self {
            name: name.into(),
            kind,
            description: description.into(),
            required: false,
            default: Some(default),
        }
    }
}

/// Everything a command says about itself.
#[derive(Clone, PartialEq, Debug)]
pub struct Metadata {
    /// A stable dotted identifier: `scene.create-entity`. What a binding, a script and an agent name.
    pub id: String,
    /// What a menu shows.
    pub label: String,
    /// Where it is grouped: "Scene", "Edit", "Assets".
    pub category: String,
    /// What it does, written for a caller that cannot see the interface.
    pub description: String,
    /// Its typed parameters.
    pub parameters: Vec<ParameterSpec>,
    /// Its effect class.
    pub effect: EffectClass,
    /// A default keyboard binding, when it has one. Not required: most commands do not.
    pub default_binding: Option<String>,
    /// Why this command is not offered to an agent, when it is not.
    ///
    /// `editor-agent-interface`: "Where a command is unsuitable for agent invocation, it SHALL be
    /// **excluded by a declared property on the command**, and the exclusion SHALL state a reason —
    /// **never by omission from a list**."
    ///
    /// So it is a field here, beside the command it describes, rather than an entry in a table the
    /// agent interface keeps: a contributor deleting a command deletes its exclusion with it, and
    /// there is no list to go stale. The projection reports an excluded command *with* its reason
    /// rather than hiding it, because an agent that cannot see a command cannot be told why.
    pub agent_exclusion: Option<String>,
}

impl Metadata {
    /// A command's metadata, with no parameters and no binding.
    pub fn new(
        id: impl Into<String>,
        label: impl Into<String>,
        category: impl Into<String>,
        description: impl Into<String>,
        effect: EffectClass,
    ) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            category: category.into(),
            description: description.into(),
            parameters: Vec::new(),
            effect,
            default_binding: None,
            agent_exclusion: None,
        }
    }

    /// Add a parameter.
    #[must_use]
    pub fn with(mut self, parameter: ParameterSpec) -> Self {
        self.parameters.push(parameter);
        self
    }

    /// Give the command a default keyboard binding.
    #[must_use]
    pub fn bound_to(mut self, binding: impl Into<String>) -> Self {
        self.default_binding = Some(binding.into());
        self
    }

    /// Declare that an agent may not invoke this, and say why.
    ///
    /// The reason is checked for length by [`Metadata::validate`] for the same reason a description
    /// is: "excluded" with no explanation leaves an agent knowing a command exists, knowing it
    /// cannot use it, and unable to act on either fact.
    #[must_use]
    pub fn not_for_agents(mut self, reason: impl Into<String>) -> Self {
        self.agent_exclusion = Some(reason.into());
        self
    }

    /// Why an agent may not invoke this, when it may not.
    #[must_use]
    pub fn agent_exclusion(&self) -> Option<&str> {
        self.agent_exclusion.as_deref()
    }

    /// Refuse metadata a caller that cannot see the interface could not act on.
    ///
    /// Every check here corresponds to a sentence of `editor-agent-interface`, and every one of them
    /// is a mistake that is easy to make and invisible afterwards: a description that describes the
    /// menu item rather than the action, a parameter named `value` with nothing saying what value,
    /// an identifier that reads like a label. The registry calls this, so none of them can be
    /// registered.
    pub fn validate(&self) -> Result<()> {
        if self.id.trim().is_empty() {
            return Err(Problem::new("register a command", "it has no identifier")
                .with_remedy("give it a stable dotted identifier such as scene.create-entity"));
        }
        if !self
            .id
            .chars()
            .all(|character| character.is_ascii_lowercase() || "0123456789.-".contains(character))
        {
            return Err(Problem::new(
                format!("register {}", self.id),
                "an identifier is lower-case ASCII with dots and hyphens",
            )
            .with_remedy("a script and an agent type this; keep it typeable and stable"));
        }
        if self.label.trim().is_empty() {
            return Err(
                Problem::new(format!("register {}", self.id), "it has no label")
                    .with_remedy("give it the text a menu would show"),
            );
        }
        if self.category.trim().is_empty() {
            return Err(
                Problem::new(format!("register {}", self.id), "it has no category")
                    .with_remedy("say which group it belongs to, such as Scene or Assets"),
            );
        }
        if self.description.trim().len() < MINIMUM_DESCRIPTION {
            return Err(Problem::new(
                format!("register {}", self.id),
                "its description is too short to tell a caller what it does",
            )
            .with_remedy(
                "write a sentence for someone who cannot see the interface: what it changes, and \
                 what it needs",
            ));
        }
        if let Some(reason) = &self.agent_exclusion
            && reason.trim().len() < MINIMUM_DESCRIPTION
        {
            return Err(Problem::new(
                format!("register {}", self.id),
                "it is excluded from the agent interface and states no reason an agent could act \
                 on",
            )
            .with_remedy(
                "say why the command is unsuitable — what it would do that an agent must not, or \
                 what it needs that an agent cannot supply",
            ));
        }
        for parameter in &self.parameters {
            if parameter.name.trim().is_empty() {
                return Err(Problem::new(
                    format!("register {}", self.id),
                    "a parameter has no name",
                ));
            }
            if parameter.description.trim().len() < MINIMUM_DESCRIPTION {
                return Err(Problem::new(
                    format!("register {}", self.id),
                    format!(
                        "the parameter {:?} has no description saying what it means",
                        parameter.name
                    ),
                )
                .with_remedy(
                    "say what the value is for, not what its type is; the type is already declared",
                ));
            }
            if !parameter.required && parameter.default.is_none() {
                return Err(Problem::new(
                    format!("register {}", self.id),
                    format!(
                        "the parameter {:?} is optional and has no default, so a caller cannot \
                         know what happens when it is omitted",
                        parameter.name
                    ),
                )
                .with_remedy("give it a default, or make it required"));
            }
            if let Some(default) = parameter
                .default
                .as_ref()
                .filter(|default| default.kind() != parameter.kind)
            {
                return Err(Problem::new(
                    format!("register {}", self.id),
                    format!(
                        "the parameter {:?} is declared {} and its default is {}",
                        parameter.name,
                        parameter.kind,
                        default.kind()
                    ),
                ));
            }
        }
        Ok(())
    }

    /// A one-line projection for a tool listing an agent reads.
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
        format!(
            "{}({parameters}) [{}] — {}",
            self.id,
            self.effect.name(),
            self.description
        )
    }
}

/// The shortest description this registry accepts.
///
/// Twenty characters is not a quality bar; it is a floor that catches the two failures that
/// actually occur — an empty string, and the label repeated as the description.
const MINIMUM_DESCRIPTION: usize = 20;

/// Whether a command can be invoked right now, and why not when it cannot.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Availability {
    /// It can be invoked.
    Available,
    /// It cannot, for this reason.
    ///
    /// "Command availability SHALL be queryable, so that a disabled action can explain why it is
    /// unavailable" — and an agent "SHALL receive the specific reason and, where one exists, the
    /// operation that would make it available", which is why this carries a [`Problem`] with its
    /// remedy rather than a string.
    Unavailable(Problem),
}

impl Availability {
    /// Whether the command can be invoked.
    #[must_use]
    pub const fn is_available(&self) -> bool {
        matches!(self, Availability::Available)
    }

    /// The reason it cannot, when it cannot.
    #[must_use]
    pub const fn reason(&self) -> Option<&Problem> {
        match self {
            Availability::Available => None,
            Availability::Unavailable(problem) => Some(problem),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn good() -> Metadata {
        Metadata::new(
            "scene.create-entity",
            "Create Entity",
            "Scene",
            "Creates an empty entity in the active document, optionally under a parent.",
            EffectClass::ReversibleMutation,
        )
    }

    #[test]
    fn good_metadata_validates() {
        good().validate().unwrap();
    }

    #[test]
    fn a_description_that_only_repeats_the_label_is_refused() {
        let mut metadata = good();
        metadata.description = "Create Entity".into();
        let problem = metadata.validate().unwrap_err();
        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("cannot see the interface")
        );
    }

    #[test]
    fn a_parameter_with_no_meaning_is_refused() {
        let metadata = good().with(ParameterSpec::required(
            "parent",
            ValueKind::Entity,
            "the parent",
        ));
        let problem = metadata.validate().unwrap_err();
        assert!(problem.because.contains("parent"), "{problem}");
    }

    #[test]
    fn an_optional_parameter_must_say_what_happens_when_it_is_omitted() {
        let metadata = good().with(ParameterSpec {
            name: "parent".into(),
            kind: ValueKind::Entity,
            description: "The entity to create the new one under.".into(),
            required: false,
            default: None,
        });
        let problem = metadata.validate().unwrap_err();
        assert!(problem.because.contains("no default"), "{problem}");
    }

    #[test]
    fn a_default_of_the_wrong_type_is_refused() {
        let metadata = good().with(ParameterSpec::optional(
            "parent",
            ValueKind::Entity,
            "The entity to create the new one under; a root when omitted.",
            Value::Float(0.0),
        ));
        assert!(metadata.validate().is_err());
    }

    #[test]
    fn an_identifier_a_script_cannot_type_is_refused() {
        let mut metadata = good();
        metadata.id = "Scene: Create Entity".into();
        assert!(metadata.validate().is_err());
    }

    #[test]
    fn only_the_irreversible_classes_need_confirmation() {
        assert!(!EffectClass::Read.needs_confirmation());
        assert!(
            !EffectClass::ReversibleMutation.needs_confirmation(),
            "undo is the confirmation; prompting for ordinary work trains people to approve blindly"
        );
        assert!(EffectClass::IrreversibleMutation.needs_confirmation());
        assert!(EffectClass::ExternalEffect.needs_confirmation());
    }

    #[test]
    fn an_exclusion_lives_on_the_command_and_must_say_why() {
        // `editor-agent-interface`: excluded "by a declared property on the command", never by
        // omission from a list — and the reason has to be one an agent could act on.
        let excluded = good().not_for_agents(
            "it opens a file chooser, which needs a person at the interface to answer",
        );
        excluded.validate().unwrap();
        assert!(excluded.agent_exclusion().unwrap().contains("person"));

        let empty = good().not_for_agents("no");
        let problem = empty.validate().unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("why"),
            "{problem}"
        );
    }

    #[test]
    fn a_command_is_offered_to_agents_unless_somebody_says_otherwise() {
        assert!(good().agent_exclusion().is_none());
    }

    #[test]
    fn a_summary_states_the_effect_class_before_invocation() {
        let metadata = good().with(ParameterSpec::optional(
            "parent",
            ValueKind::Entity,
            "The entity to create the new one under; a root when omitted.",
            Value::Entity(0),
        ));
        let summary = metadata.summary();
        assert!(summary.contains("[reversible-mutation]"), "{summary}");
        assert!(summary.contains("parent?: entity"), "{summary}");
    }
}
