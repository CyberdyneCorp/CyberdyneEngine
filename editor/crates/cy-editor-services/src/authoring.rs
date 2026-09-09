//! Writing source, building it, reloading it, and playing. Tasks 3.6, 3.7 and 3.8.
//!
//! These are the second half of the loop `design.md` §3 says the milestone exists for:
//!
//! ```text
//! compose a scene  ->  write a gameplay script  ->  build and reload  ->  play  ->  LOOK  ->  decide
//! ```
//!
//! Every one of them is a registered command with typed parameters and a declared effect class, for
//! the reason `crate::viewports` states at greater length: an action reachable only through a widget
//! is a defect, and an action reachable only through the agent interface is the same defect with the
//! sign flipped. A person writes a script through the same command an agent does.
//!
//! --- THE EFFECT CLASSES, AND THE ARGUMENT FOR EACH -------------------------------------------------
//!
//! | command | class | why |
//! |---|---|---|
//! | `source.write` | irreversible, **computed down** to reversible | `design.md` §4; see below |
//! | `source.delete` | irreversible, computed down to reversible | the same rule, the same test |
//! | `project.build` | external effect | it runs a compiler outside the editor |
//! | `project.reload` | read | it changes what the runtime is running, and nothing in the project |
//! | `play.*` | read | a mode switch; the same argument `crate::viewports` makes for its thirty-seven |
//!
//! The two source commands **declare the worst case and compute the real one**. The declared class is
//! what a tool listing shows before any arguments exist, so a caller reasoning in the abstract is
//! told that writing source can be irreversible; the computed class is what the scope admits and what
//! decides whether a human is asked. `design.md` §4:
//!
//! > The class is computed, not assumed, and the confirmation rule follows from it. That keeps
//! > `editor-agent-interface`'s promise that confirmation is rare and meaningful rather than a habit.
//!
//! `project.reload` and `play.*` being **read** is the surprising row and it is deliberate. They
//! change what the editor and its runtime are doing; they record no transaction, they touch no
//! document, and there is nothing for undo to reverse. A confirmation prompt on entering play would
//! be the exact habit the rule above exists to prevent. What `play.leave` *does* discard is stated in
//! its own description, which is where a caller reads it.

use cy_editor_commands::{
    Availability, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::operation::Operation;

use crate::project::{SOURCE_DOMAIN, decode_source, encode_source};

/// Register the source, build, reload and play commands.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(source_write())?;
    registry.register(source_delete())?;
    registry.register(project_build())?;
    registry.register(project_reload())?;
    for state in PLAY_STATES {
        registry.register(play(state))?;
    }
    Ok(())
}

/// The play states, their commands and their bindings.
///
/// A table rather than three functions, so that a fourth state — if the runtime ever grows one —
/// cannot be added to `cy_editor_viewport::play::PlayState` and forgotten here.
const PLAY_STATES: [(&str, &str, &str, &str, &str); 3] = [
    (
        "play.enter",
        "Play",
        "playing",
        "F5",
        "Starts simulating the world in the attached runtime. Edits made while playing apply to \
         the running world and, unless the persistence policy says otherwise, are discarded when \
         play ends.",
    ),
    (
        "play.pause",
        "Pause",
        "paused",
        "F6",
        "Stops the simulation where it is without leaving play. What is in the world is still the \
         simulation's rather than the document's.",
    ),
    (
        "play.leave",
        "Stop",
        "editing",
        "shift+F5",
        "Leaves play and returns to authoring. Edits made during play that the persistence policy \
         did not promote are discarded, which is what the viewport has been saying throughout.",
    ),
];

/// The project host, or a refusal that says what to do about its absence.
fn host(context: &mut dyn CommandContext) -> Result<&mut dyn cy_editor_commands::ProjectHost> {
    context.project().ok_or_else(|| {
        Problem::new("reach the project", "this editor has no project open")
            .with_remedy("open a project first")
    })
}

/// Refuse a path the current invocation's scope does not cover.
///
/// `editor-agent-interface`: "An agent connection SHALL declare the scope it operates within: which
/// documents, **which directories** ... Operations outside that scope SHALL be refused with the
/// scope as the reason." The directories are the connection's, carried on the context for the
/// duration of the invocation — see `crate::editor::Editor::invoke`.
pub(crate) fn within_scope(context: &dyn CommandContext, path: &str) -> Result<()> {
    let Some((scope, directories)) = context.permitted_paths() else {
        return Ok(());
    };
    if directories
        .iter()
        .any(|prefix| path.starts_with(prefix.as_str()))
    {
        return Ok(());
    }
    Err(Problem::new(
        format!("write {path}"),
        format!("the scope {scope:?} does not include it"),
    )
    .with_remedy(if directories.is_empty() {
        "this connection was granted no directories; grant one deliberately".to_string()
    } else {
        format!(
            "the directories it may touch are: {}",
            directories.join(", ")
        )
    }))
}

/// What the argument named `path` says, refused when it says nothing.
fn path_of(arguments: &cy_editor_commands::Arguments) -> Result<String> {
    let path = arguments
        .text("path")
        .unwrap_or_default()
        .trim()
        .to_string();
    if path.is_empty() {
        return Err(Problem::new("write a source file", "no path was given")
            .with_remedy("name a project-relative path, such as game/Player.swift"));
    }
    Ok(path)
}

/// The effect class one source edit actually has.
///
/// Shared by both source commands because it is one rule, and a second copy of it is how the two
/// would eventually disagree about what "reversible" means.
fn effect_of_edit(
    context: &mut dyn CommandContext,
    arguments: &cy_editor_commands::Arguments,
) -> EffectClass {
    let Ok(path) = path_of(arguments) else {
        return EffectClass::IrreversibleMutation;
    };
    match context.project() {
        Some(project) if project.source_is_restorable(&path) => EffectClass::ReversibleMutation,
        _ => EffectClass::IrreversibleMutation,
    }
}

fn source_write() -> Command {
    Command::new(
        Metadata::new(
            "source.write",
            "Write Source File",
            "Source",
            "Writes a project source file, creating it and any directories above it. The edit is \
             one transaction on the document that stands for the file, so it undoes, appears in \
             the history naming whoever made it, and survives a crash. Reversible when the editor \
             can read what the file held; irreversible when it cannot.",
            EffectClass::IrreversibleMutation,
        )
        .with(ParameterSpec::required(
            "path",
            ValueKind::Text,
            "The project-relative path of the file to write, such as game/Player.swift.",
        ))
        .with(ParameterSpec::required(
            "contents",
            ValueKind::Text,
            "The whole new contents of the file. This is a replacement, not an append.",
        )),
        |context, arguments| {
            let path = path_of(arguments)?;
            within_scope(context, &path)?;
            let contents = arguments.text("contents").unwrap_or_default().to_string();

            let project = host(context)?;
            let before = if project.source_exists(&path) {
                Some(project.read_source(&path)?)
            } else {
                None
            };
            if before.as_deref() == Some(contents.as_str()) {
                return Ok(Outcome::new(format!("{path} already held that"))
                    .with("path", Value::Text(path)));
            }

            let document = context.open_document(&path)?;
            let actor = context.actor();
            let scope = context
                .document_mut(document)
                .ok_or_else(|| Problem::not_found("the document standing for the file"))?;
            scope.begin(format!("Write {path}"), actor);
            let recorded = scope.record(Operation::Domain {
                node: None,
                kind: SOURCE_DOMAIN.to_string(),
                before: encode_source(before.as_deref()),
                after: encode_source(Some(&contents)),
            });
            // The record happens first and the write second, so a write that fails leaves no
            // history entry claiming it succeeded. The reverse order is the one that is wrong in a
            // way nobody notices until the file system is full.
            let written = recorded.and_then(|()| host(context)?.put_source(&path, Some(&contents)));
            let scope = context
                .document_mut(document)
                .ok_or_else(|| Problem::not_found("the document standing for the file"))?;
            match written {
                Ok(()) => {
                    scope.commit()?;
                }
                Err(problem) => {
                    scope.cancel()?;
                    return Err(problem);
                }
            }
            Ok(Outcome::new(format!("Wrote {path}"))
                .with("path", Value::Text(path))
                .with("created", Value::Bool(before.is_none())))
        },
    )
    .effect_when(effect_of_edit)
}

fn source_delete() -> Command {
    Command::new(
        Metadata::new(
            "source.delete",
            "Delete Source File",
            "Source",
            "Removes a project source file. Recorded as one transaction carrying what the file \
             held, so undo puts it back when the editor could read it; irreversible when it could \
             not.",
            EffectClass::IrreversibleMutation,
        )
        .with(ParameterSpec::required(
            "path",
            ValueKind::Text,
            "The project-relative path of the file to remove.",
        )),
        |context, arguments| {
            let path = path_of(arguments)?;
            within_scope(context, &path)?;
            let project = host(context)?;
            if !project.source_exists(&path) {
                return Err(
                    Problem::new(format!("delete {path}"), "the project has no such file")
                        .with_remedy("read the sources resource to see what there is"),
                );
            }
            let before = project.read_source(&path).ok();

            let document = context.open_document(&path)?;
            let actor = context.actor();
            let scope = context
                .document_mut(document)
                .ok_or_else(|| Problem::not_found("the document standing for the file"))?;
            scope.begin(format!("Delete {path}"), actor);
            let recorded = scope.record(Operation::Domain {
                node: None,
                kind: SOURCE_DOMAIN.to_string(),
                before: encode_source(before.as_deref()),
                after: encode_source(None),
            });
            let removed = recorded.and_then(|()| host(context)?.put_source(&path, None));
            let scope = context
                .document_mut(document)
                .ok_or_else(|| Problem::not_found("the document standing for the file"))?;
            match removed {
                Ok(()) => {
                    scope.commit()?;
                }
                Err(problem) => {
                    scope.cancel()?;
                    return Err(problem);
                }
            }
            Ok(Outcome::new(format!("Deleted {path}"))
                .with("recoverable", Value::Bool(before.is_some())))
        },
    )
    .effect_when(effect_of_edit)
}

fn project_build() -> Command {
    Command::new(
        Metadata::new(
            "project.build",
            "Build Script Module",
            "Project",
            "Compiles the project's script sources into the next hot-reload generation. Runs in \
             the background: this returns as soon as the work is queued, and the operations \
             resource says how it is getting on. Each generation is a different library file, \
             because a loader asked for a path it already holds returns the image it already has.",
            EffectClass::ExternalEffect,
        )
        .bound_to("Ctrl+B"),
        |context, _| {
            let project = host(context)?;
            let started = project.build()?;
            Ok(Outcome::new(started.clone()).with("operation", Value::Text(started)))
        },
    )
}

fn project_reload() -> Command {
    Command::new(
        Metadata::new(
            "project.reload",
            "Reload Script Module",
            "Project",
            "Asks the attached runtime to load the newest built generation of the script module, \
             keeping the live state the world already holds. Refused with the reason when nothing \
             has been built, when the build is still running, or when no runtime is attached.",
            EffectClass::Read,
        )
        .with(ParameterSpec::optional(
            "module",
            ValueKind::Text,
            "Which module to reload; the project's own when omitted.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let module = arguments.text("module").unwrap_or_default().to_string();
            let project = host(context)?;
            let said = project.reload(&module)?;
            Ok(Outcome::new(said))
        },
    )
}

fn play(
    state: (
        &'static str,
        &'static str,
        &'static str,
        &'static str,
        &'static str,
    ),
) -> Command {
    let (id, label, value, binding, description) = state;
    Command::new(
        Metadata::new(id, label, "Play", description, EffectClass::Read).bound_to(binding),
        move |context, _| {
            let project = host(context)?;
            let said = project.set_play(value)?;
            Ok(Outcome::new(said).with("play", Value::Text(value.to_string())))
        },
    )
    .available_when(move |context| {
        // Asking to enter play twice is not an error worth a refusal, but asking to *leave* it when
        // nothing is playing is a caller that has lost track, and telling it so is cheaper than
        // letting it believe something happened.
        match context.play_state() {
            Some(current) if current == value => Availability::Unavailable(
                Problem::new(id, format!("the runtime is already {current}"))
                    .with_remedy("read the play resource before switching"),
            ),
            _ => Availability::Available,
        }
    })
}

/// Read a source file back as text, for the resource that lists them.
///
/// Here rather than in the agent interface because a panel showing a script reads the same thing:
/// `editor-agent-interface` requires resource content to come "from the same services the editor's
/// own panels read".
pub fn source_text(context: &mut dyn CommandContext, path: &str) -> Result<String> {
    host(context)?.read_source(path)
}

/// What a source domain operation held before it, for a history view.
#[must_use]
pub fn source_before(operation: &Operation) -> Option<Option<String>> {
    match operation {
        Operation::Domain { kind, before, .. } if kind == SOURCE_DOMAIN => {
            Some(decode_source(before))
        }
        _ => None,
    }
}
