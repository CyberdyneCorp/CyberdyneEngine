//! The registry: one place every action is registered, and one call that invokes any of them.
//!
//! "WHEN an action exists THEN it SHALL be invocable from a menu, a shortcut, the palette, a script,
//! and a test without additional implementation." There is exactly one invocation path below, and
//! every one of those five is a caller of it — which is what makes that scenario true rather than
//! aspirational.

use std::collections::BTreeMap;

use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};

use crate::context::{CommandContext, Outcome};
use crate::metadata::{Availability, EffectClass, Metadata};
use crate::scope::Scope;

/// A command's identifier: its dotted name.
pub type CommandId = String;

/// The arguments an invocation supplies, keyed by parameter name.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Arguments(BTreeMap<String, Value>);

impl Arguments {
    /// No arguments.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Supply one argument.
    #[must_use]
    pub fn with(mut self, name: impl Into<String>, value: Value) -> Self {
        self.0.insert(name.into(), value);
        self
    }

    /// One argument's value, which validation has already checked the type of.
    #[must_use]
    pub fn get(&self, name: &str) -> Option<&Value> {
        self.0.get(name)
    }

    /// One argument as an entity identifier.
    #[must_use]
    pub fn entity(&self, name: &str) -> Option<u64> {
        match self.0.get(name)? {
            Value::Entity(entity) => Some(*entity),
            _ => None,
        }
    }

    /// One argument as text.
    #[must_use]
    pub fn text(&self, name: &str) -> Option<&str> {
        self.0.get(name)?.as_text()
    }

    /// The names supplied.
    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.0.keys().map(String::as_str)
    }
}

/// An availability predicate: why a command cannot be invoked right now.
///
/// `Send + Sync` because the registry is shared: a background operation reporting progress and the
/// interface thread listing the palette both read it, and a registry that could only be touched from
/// one thread would push every command into the interface thread by construction.
pub type AvailabilityFn = dyn Fn(&dyn CommandContext) -> Availability + Send + Sync;

/// A command's implementation.
pub type RunFn = dyn Fn(&mut dyn CommandContext, &Arguments) -> Result<Outcome> + Send + Sync;

/// What class of consequence *this* invocation has, given its arguments.
///
/// Takes `&mut` for the same reason [`CommandContext::project`] does: the state that decides the
/// answer — whether the editor can restore a file it is about to overwrite — is reached through an
/// accessor that hands out a mutable borrow. **An implementation must not mutate anything.** It is
/// called before the scope is checked, so a predicate with a side effect would be a way to act
/// outside a scope, which is the one thing this crate exists to prevent.
pub type EffectFn = dyn Fn(&mut dyn CommandContext, &Arguments) -> EffectClass + Send + Sync;

/// One registered action.
pub struct Command {
    metadata: Metadata,
    availability: Box<AvailabilityFn>,
    effect: Option<Box<EffectFn>>,
    run: Box<RunFn>,
}

impl Command {
    /// A command that is always available.
    ///
    /// The common case, and the one worth making short: most commands are unavailable only because
    /// of state a predicate would have to ask for anyway.
    pub fn new(
        metadata: Metadata,
        run: impl Fn(&mut dyn CommandContext, &Arguments) -> Result<Outcome> + Send + Sync + 'static,
    ) -> Self {
        Self {
            metadata,
            availability: Box::new(|_| Availability::Available),
            effect: None,
            run: Box::new(run),
        }
    }

    /// Give the command an availability predicate that can explain itself.
    #[must_use]
    pub fn available_when(
        mut self,
        predicate: impl Fn(&dyn CommandContext) -> Availability + Send + Sync + 'static,
    ) -> Self {
        self.availability = Box::new(predicate);
        self
    }

    /// Let the command compute its effect class from the invocation rather than declaring one.
    ///
    /// `design.md` §4, on writing source: "The class is **computed, not assumed**, and the
    /// confirmation rule follows from it. That keeps `editor-agent-interface`'s promise that
    /// confirmation is rare and meaningful rather than a habit."
    ///
    /// The declared [`Metadata::effect`] stays, and stays the **ceiling**: it is what a tool listing
    /// shows before any arguments exist, so a caller reasoning about consequence in the abstract is
    /// told the worst case. What the computed class does is let a particular invocation be admitted
    /// as the narrower thing it actually is.
    #[must_use]
    pub fn effect_when(
        mut self,
        effect: impl Fn(&mut dyn CommandContext, &Arguments) -> EffectClass + Send + Sync + 'static,
    ) -> Self {
        self.effect = Some(Box::new(effect));
        self
    }

    /// What the command says about itself.
    #[must_use]
    pub const fn metadata(&self) -> &Metadata {
        &self.metadata
    }

    /// Whether this command's effect class depends on the invocation.
    #[must_use]
    pub const fn computes_effect(&self) -> bool {
        self.effect.is_some()
    }
}

/// Every action the editor can perform.
#[derive(Default)]
pub struct Registry {
    commands: BTreeMap<CommandId, Command>,
}

impl Registry {
    /// An empty registry.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Register a command, **refusing** metadata a machine caller could not act on.
    ///
    /// This is where "applies from the first command registered" is enforced. See the crate note.
    pub fn register(&mut self, command: Command) -> Result<()> {
        command.metadata.validate()?;
        let id = command.metadata.id.clone();
        if self.commands.contains_key(&id) {
            return Err(Problem::new(
                format!("register {id}"),
                "a command with that identifier is already registered",
            )
            .with_remedy(
                "identifiers are the contract a binding and a script hold; pick another",
            ));
        }
        self.commands.insert(id, command);
        Ok(())
    }

    /// How many commands are registered.
    #[must_use]
    pub fn len(&self) -> usize {
        self.commands.len()
    }

    /// Whether nothing is registered.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.commands.is_empty()
    }

    /// One command's metadata.
    #[must_use]
    pub fn metadata(&self, id: &str) -> Option<&Metadata> {
        self.commands.get(id).map(Command::metadata)
    }

    /// Every command's metadata, in identifier order.
    pub fn all(&self) -> impl Iterator<Item = &Metadata> {
        self.commands.values().map(Command::metadata)
    }

    /// Whether this command works out its effect class from the invocation rather than declaring
    /// one. `false` for a command that is not registered.
    #[must_use]
    pub fn computes_effect(&self, id: &str) -> bool {
        self.commands.get(id).is_some_and(Command::computes_effect)
    }

    /// What class of consequence one particular invocation has.
    ///
    /// The declared class for almost every command, and the computed one where the command declares
    /// a predicate — see [`Command::effect_when`]. Arguments are validated first, because an effect
    /// class computed from arguments the registry would have refused is a class for an invocation
    /// that will never happen.
    pub fn effect_of(
        &self,
        id: &str,
        context: &mut dyn CommandContext,
        arguments: &Arguments,
    ) -> Result<EffectClass> {
        let command = self.commands.get(id).ok_or_else(|| {
            Problem::not_found(format!("a command named {id:?}"))
                .with_remedy("list the registry to see what there is")
        })?;
        let arguments = validate_arguments(&command.metadata, arguments)?;
        Ok(command
            .effect
            .as_ref()
            .map_or(command.metadata.effect, |effect| {
                effect(context, &arguments)
            }))
    }

    /// Whether a command can be invoked right now, and why not when it cannot.
    #[must_use]
    pub fn availability(&self, id: &str, context: &dyn CommandContext) -> Availability {
        match self.commands.get(id) {
            Some(command) => (command.availability)(context),
            None => Availability::Unavailable(
                Problem::not_found(format!("a command named {id:?}"))
                    .with_remedy("list the registry to see what there is"),
            ),
        }
    }

    /// Invoke a command.
    ///
    /// In order: the command must exist, the scope must admit its effect class, the arguments must
    /// match the declared parameters, and the availability predicate must say yes. Each failure
    /// names what went wrong and, where one exists, what would make it succeed.
    pub fn invoke(
        &self,
        id: &str,
        scope: &Scope,
        context: &mut dyn CommandContext,
        arguments: &Arguments,
    ) -> Result<Outcome> {
        let command = self.commands.get(id).ok_or_else(|| {
            Problem::not_found(format!("a command named {id:?}"))
                .with_remedy("list the registry to see what there is")
        })?;

        // A command that declares one class checks it before anything else, so a refusal for being
        // out of scope does not depend on the arguments being right. One that COMPUTES its class
        // cannot: the answer needs the arguments, so they are validated first and the computed
        // class is what the scope admits. See `Command::effect_when`.
        if !command.computes_effect() {
            scope.admit(&command.metadata)?;
        }
        let arguments = validate_arguments(&command.metadata, arguments)?;
        if let Some(effect) = command.effect.as_ref() {
            scope.admit_effect(&command.metadata, effect(context, &arguments))?;
        }

        match (command.availability)(context) {
            Availability::Available => {}
            Availability::Unavailable(problem) => return Err(problem),
        }

        (command.run)(context, &arguments)
    }

    /// The projection an agent reads: every command, with its parameters and its effect class.
    ///
    /// `editor-agent-interface` calls this "the projection of the command registry", and requires
    /// that "WHEN an agent lists the available tools THEN each SHALL state its effect class".
    #[must_use]
    pub fn projection(&self) -> Vec<String> {
        self.all().map(Metadata::summary).collect()
    }
}

/// Check arguments against declared parameters, filling defaults.
///
/// Returns a *new* argument set rather than validating in place, so that a handler reads the same
/// map whether the caller supplied an optional parameter or not — which removes the commonest
/// handler bug, which is forgetting the default.
fn validate_arguments(metadata: &Metadata, supplied: &Arguments) -> Result<Arguments> {
    let mut resolved = Arguments::new();

    for parameter in &metadata.parameters {
        match supplied.get(&parameter.name) {
            Some(value) => {
                if value.kind() != parameter.kind {
                    return Err(Problem::new(
                        format!("invoke {}", metadata.id),
                        format!(
                            "the parameter {:?} is declared {} and a {} was supplied",
                            parameter.name,
                            parameter.kind,
                            value.kind()
                        ),
                    )
                    .with_remedy(format!("{}: {}", parameter.name, parameter.description)));
                }
                resolved.0.insert(parameter.name.clone(), value.clone());
            }
            None if parameter.required => {
                return Err(Problem::new(
                    format!("invoke {}", metadata.id),
                    format!(
                        "the required parameter {:?} was not supplied",
                        parameter.name
                    ),
                )
                .with_remedy(format!(
                    "supply {} ({}): {}",
                    parameter.name, parameter.kind, parameter.description
                )));
            }
            None => {
                let default = parameter
                    .default
                    .clone()
                    .unwrap_or_else(|| ValueKind::empty(parameter.kind));
                resolved.0.insert(parameter.name.clone(), default);
            }
        }
    }

    // An argument nobody declared is a caller's mistake, and silently ignoring it is how a typo in a
    // parameter name becomes an hour of wondering why nothing happened.
    for name in supplied.names() {
        if !metadata
            .parameters
            .iter()
            .any(|parameter| parameter.name == name)
        {
            return Err(Problem::new(
                format!("invoke {}", metadata.id),
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
            )));
        }
    }

    Ok(resolved)
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::ids::DocumentId;
    use cy_editor_documents::Document;
    use cy_editor_documents::selection::Selection;

    use super::*;
    use crate::metadata::{EffectClass, ParameterSpec};
    use crate::scope::{DocumentScope, Scope};

    /// The thirty-line test double the crate note promises.
    struct Fixture {
        document: Document,
        selection: Selection,
        notifications: Vec<String>,
    }

    impl Fixture {
        fn new() -> Self {
            Self {
                document: Document::new("worlds/city.cyworld"),
                selection: Selection::new(),
                notifications: Vec::new(),
            }
        }
    }

    impl CommandContext for Fixture {
        fn active_document(&self) -> Option<DocumentId> {
            Some(self.document.id())
        }
        fn document(&self, id: DocumentId) -> Option<&Document> {
            (id == self.document.id()).then_some(&self.document)
        }
        fn document_mut(&mut self, id: DocumentId) -> Option<&mut Document> {
            (id == self.document.id()).then_some(&mut self.document)
        }
        fn actor(&self) -> Actor {
            Actor::human("designer")
        }
        fn selection(&self) -> &Selection {
            &self.selection
        }
        fn set_selection(&mut self, selection: Selection) {
            self.selection = selection;
        }
        fn notify(&mut self, message: &str) {
            self.notifications.push(message.to_string());
        }
    }

    fn create_entity() -> Command {
        Command::new(
            Metadata::new(
                "scene.create-entity",
                "Create Entity",
                "Scene",
                "Creates an empty entity in the active document and selects it.",
                EffectClass::ReversibleMutation,
            )
            .with(ParameterSpec::optional(
                "name",
                ValueKind::Text,
                "The name to give the new entity; it is left unnamed when omitted.",
                Value::Text(String::new()),
            )),
            |context, arguments| {
                let document = context.active_document().ok_or_else(|| {
                    Problem::new("create an entity", "no document is open")
                        .with_remedy("open a document first")
                })?;
                let actor = context.actor();
                let name = arguments.text("name").unwrap_or_default().to_string();
                let document = context
                    .document_mut(document)
                    .ok_or_else(|| Problem::not_found("the active document"))?;
                let node = document.with_transaction("Create entity", actor, |document| {
                    document.create_node(None)
                })?;
                Ok(Outcome::new(format!(
                    "Created {}",
                    if name.is_empty() { "an entity" } else { &name }
                ))
                .with("entity", Value::Text(node.to_string())))
            },
        )
    }

    fn registry() -> Registry {
        let mut registry = Registry::new();
        registry.register(create_entity()).unwrap();
        registry
    }

    #[test]
    fn one_action_is_invocable_from_anything_that_has_the_registry() {
        let registry = registry();
        let mut fixture = Fixture::new();
        let outcome = registry
            .invoke(
                "scene.create-entity",
                &Scope::unrestricted(),
                &mut fixture,
                &Arguments::new(),
            )
            .unwrap();
        assert!(outcome.values.contains_key("entity"));
        assert_eq!(fixture.document.history().entries().len(), 1);
    }

    #[test]
    fn a_command_whose_metadata_a_machine_could_not_act_on_cannot_be_registered() {
        let mut registry = Registry::new();
        let bad = Command::new(
            Metadata::new("x", "X", "X", "X", EffectClass::Read),
            |_, _| Ok(Outcome::new("")),
        );
        assert!(registry.register(bad).is_err());
        assert!(registry.is_empty(), "and nothing was registered");
    }

    #[test]
    fn an_out_of_scope_effect_class_is_refused_naming_the_scope() {
        let registry = registry();
        let mut fixture = Fixture::new();
        let scope = Scope::new("read only", DocumentScope::All, [EffectClass::Read]);
        let problem = registry
            .invoke(
                "scene.create-entity",
                &scope,
                &mut fixture,
                &Arguments::new(),
            )
            .unwrap_err();
        assert!(problem.because.contains("\"read only\""), "{problem}");
        assert_eq!(
            fixture.document.history().entries().len(),
            0,
            "and nothing ran"
        );
    }

    #[test]
    fn a_wrongly_typed_argument_is_refused_with_the_parameters_meaning() {
        let registry = registry();
        let mut fixture = Fixture::new();
        let problem = registry
            .invoke(
                "scene.create-entity",
                &Scope::unrestricted(),
                &mut fixture,
                &Arguments::new().with("name", Value::Float(1.0)),
            )
            .unwrap_err();
        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("The name to give"),
            "{problem}"
        );
    }

    #[test]
    fn a_misspelt_argument_is_refused_rather_than_ignored() {
        let registry = registry();
        let mut fixture = Fixture::new();
        let problem = registry
            .invoke(
                "scene.create-entity",
                &Scope::unrestricted(),
                &mut fixture,
                &Arguments::new().with("nmae", Value::Text("lamp".into())),
            )
            .unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("name"),
            "{problem}"
        );
    }

    #[test]
    fn an_unavailable_command_explains_itself_rather_than_being_greyed() {
        let mut registry = Registry::new();
        registry
            .register(
                Command::new(
                    Metadata::new(
                        "edit.undo",
                        "Undo",
                        "Edit",
                        "Reverses the most recent change to the active document.",
                        EffectClass::ReversibleMutation,
                    ),
                    |_, _| Ok(Outcome::new("Undone")),
                )
                .available_when(|context| match context.active_document() {
                    Some(id)
                        if context
                            .document(id)
                            .is_some_and(|document| document.history_status().undoable > 0) =>
                    {
                        Availability::Available
                    }
                    _ => Availability::Unavailable(
                        Problem::new("undo", "there is nothing to undo in this document")
                            .with_remedy("make a change first"),
                    ),
                }),
            )
            .unwrap();

        let mut fixture = Fixture::new();
        let availability = registry.availability("edit.undo", &fixture);
        assert!(!availability.is_available());
        assert!(availability.reason().unwrap().remedy.is_some());

        let problem = registry
            .invoke(
                "edit.undo",
                &Scope::unrestricted(),
                &mut fixture,
                &Arguments::new(),
            )
            .unwrap_err();
        assert_eq!(problem.because, "there is nothing to undo in this document");
    }

    #[test]
    fn the_projection_states_every_commands_effect_class() {
        let registry = registry();
        for line in registry.projection() {
            assert!(
                EffectClass::ALL
                    .iter()
                    .any(|effect| line.contains(effect.name())),
                "a tool listing must state consequence before invocation: {line}"
            );
        }
    }

    #[test]
    fn a_duplicate_identifier_is_refused() {
        let mut registry = registry();
        assert!(registry.register(create_entity()).is_err());
    }
}
