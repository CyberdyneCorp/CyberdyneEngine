// SPDX-License-Identifier: MIT
//! The character an animation graph plays on, and the bake that carries its events into the game.
//! Issue #29 (animation), the gaps #112 left.
//!
//! **A graph plays on a character** — the engine's built-in mannequin, or a model the project
//! imported whose import cooked a skeleton (`skeleton/` in its `.import` record), with every clip
//! the project's imports cooked (`animation/`). Which one is written beside the graph, in its
//! `.cyanimcharacter` file ([`crate::animation_graph::format_character`]); no file is the
//! mannequin. Choosing is one undoable transaction in the open world's history, under the same
//! domain as the graph's own text, so an undo puts the file back and the preview follows it.
//!
//! **The engine decides what plays**: the editor sends the skeleton, the mesh and every clip's id
//! (`animation.character.set`) and the engine loads the cooked records, refusing by name a clip
//! cooked for another skeleton. It is sent before a preview that needs it and only when it is not
//! the character last sent.
//!
//! **A bake** (`animation.bake`) cooks the graph for its character: the engine answers with the
//! rig's files and the editor writes them, replacing the rig's directory under
//! [`crate::animation_graph::RIG_DIRECTORY`] whole, so a clip the graph no longer names leaves no
//! stale file behind. Play loads every rig there.

use cy_editor_commands::AnimationCharacters;
use cy_editor_core::problem::{Problem, Result};

use crate::animation_graph::{
    CharacterChoice, DOMAIN_PREFIX, RIG_DIRECTORY, character_reference, format_character,
    parse_character, project_characters, project_clips, rig_name, validate_reference,
};
use crate::editor::Editor;
use crate::notifications::Notification;

/// The choice last resolved for a graph: the graph, its character file's text, and the choice.
pub(crate) type CachedChoice = (String, Option<String>, CharacterChoice);

impl Editor {
    /// The project's characters and clips, from a fresh look at its import records.
    ///
    /// # Errors
    ///
    /// When the project's tree or an import record cannot be read.
    pub fn list_animation_characters(&mut self) -> Result<AnimationCharacters> {
        self.asset_catalogue.refresh()?;
        let entries = self.asset_catalogue.entries();
        Ok(AnimationCharacters {
            characters: project_characters(entries)
                .into_iter()
                .map(|character| (character.model, character.skeleton, character.mesh))
                .collect(),
            clips: project_clips(entries)
                .into_iter()
                .map(|clip| (clip.name, clip.source))
                .collect(),
        })
    }

    /// The model `reference` plays on, from its character file; `None` for the mannequin.
    ///
    /// # Errors
    ///
    /// When the character file cannot be read or does not parse.
    pub fn animation_character_model(&self, reference: &str) -> Result<Option<String>> {
        let file = character_reference(reference);
        if !self.project.source_exists(&file) {
            return Ok(None);
        }
        parse_character(&self.project.read_source(&file)?).map(Some)
    }

    /// What the engine must play for `reference`, resolved again only when the graph or its
    /// character file changed: a scrub asks every frame, and a look at the project's imports is
    /// a walk of its tree.
    pub(crate) fn animation_choice(&mut self, reference: &str) -> Result<CharacterChoice> {
        let file = character_reference(reference);
        let text = if self.project.source_exists(&file) {
            Some(self.project.read_source(&file)?)
        } else {
            None
        };
        if let Some((cached, cached_text, choice)) = &self.animation_choice
            && cached == reference
            && *cached_text == text
        {
            return Ok(choice.clone());
        }
        let choice = match &text {
            None => CharacterChoice::default(),
            Some(text) => {
                let model = parse_character(text)?;
                self.asset_catalogue.refresh()?;
                CharacterChoice::for_model(self.asset_catalogue.entries(), &model)?
            }
        };
        self.animation_choice = Some((reference.to_owned(), text, choice.clone()));
        Ok(choice)
    }

    /// Send the engine `reference`'s character, unless it is the one it was last sent.
    pub(crate) fn ensure_animation_character(&mut self, reference: &str) -> Result<()> {
        let choice = self.animation_choice(reference)?;
        if self.backend.animation.wants_character(&choice) {
            self.backend.animation.character(&self.runtime, choice)?;
        }
        Ok(())
    }

    /// Choose `reference`'s character: write its character file (or remove it for the mannequin)
    /// as one transaction, then show the preview on it when the engine previews this graph.
    pub(crate) fn save_animation_character(
        &mut self,
        reference: &str,
        model: Option<&str>,
    ) -> Result<()> {
        validate_reference(reference)?;
        let file = character_reference(reference);
        // Choosing again looks at the project's imports again, so a model imported since is seen.
        self.animation_choice = None;
        match model {
            Some(model) => {
                // Refused before anything is written: a model with no skeleton plays nothing.
                self.asset_catalogue.refresh()?;
                CharacterChoice::for_model(self.asset_catalogue.entries(), model)?;
                self.save_graph_source(
                    &file,
                    &format_character(model),
                    DOMAIN_PREFIX,
                    "animation character",
                )?;
            }
            None => {
                if self.project.source_exists(&file) {
                    self.remove_graph_source(&file, DOMAIN_PREFIX, "animation character")?;
                }
            }
        }
        self.character_follows(reference);
        Ok(())
    }

    /// THE PREVIEW FOLLOWS THE CHARACTER: after a choice, an undo or a redo of one, a graph the
    /// engine previews is shown again on the character its file now names.
    pub(crate) fn character_follows(&mut self, reference: &str) {
        let Some(settings) = self.backend.animation.settings().cloned() else {
            return;
        };
        if settings.reference != reference || !self.runtime.is_connected() {
            return;
        }
        let Ok(source) = self.project.read_source(reference) else {
            return;
        };
        if let Err(problem) = self.ensure_animation_character(reference) {
            self.notifications.post(Notification::error(
                "The animation graph's character could not be shown",
                problem,
            ));
            return;
        }
        let _ = self
            .backend
            .animation
            .preview(&self.runtime, settings, &source);
    }

    /// Ask the engine to bake `reference` for its character.
    pub(crate) fn request_animation_bake(&mut self, reference: &str) -> Result<u64> {
        validate_reference(reference)?;
        let choice = self.animation_choice(reference)?;
        if choice.is_mannequin() {
            return Err(Problem::new(
                format!("bake {reference}"),
                "it plays on the built-in mannequin, which a game cannot load",
            )
            .with_remedy("choose an imported character first: animation.character.set"));
        }
        let source = self.project.read_source(reference)?;
        let sent = self
            .backend
            .animation
            .bake(&self.runtime, reference, &source, &choice)?;
        Ok(sent.map_or(0, cy_editor_protocol::RequestId::as_u64))
    }

    /// Write every bake the engine answered: each rig's directory replaced by what it baked. A
    /// bake the graph's errors refused writes nothing and leaves the previous rig in place.
    pub fn finish_animation_bakes(&mut self) {
        for (reference, report) in self.backend.animation.take_baked() {
            if !report.baked {
                self.notifications.post(Notification::warning(format!(
                    "{reference} was not baked: the graph has an error; animation.status names it"
                )));
                continue;
            }
            let directory = self
                .project
                .root()
                .join(RIG_DIRECTORY)
                .join(rig_name(&reference));
            match write_rig(&directory, &report.files) {
                Ok(()) => self.notifications.post(Notification::info(format!(
                    "Baked {reference} into {RIG_DIRECTORY}/{}",
                    rig_name(&reference)
                ))),
                Err(problem) => self.notifications.post(Notification::error(
                    "The baked rig could not be written",
                    problem,
                )),
            }
        }
    }
}

/// Replace `directory` with `files`, each path relative to it.
fn write_rig(directory: &std::path::Path, files: &[(String, Vec<u8>)]) -> Result<()> {
    let failed = |action: &str, error: std::io::Error| {
        Problem::new(
            format!("{action} {}", directory.display()),
            error.to_string(),
        )
    };
    match std::fs::remove_dir_all(directory) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(failed("clear", error)),
    }
    for (path, bytes) in files {
        let target = directory.join(path);
        if let Some(parent) = target.parent() {
            std::fs::create_dir_all(parent).map_err(|error| failed("create", error))?;
        }
        std::fs::write(&target, bytes).map_err(|error| failed("write into", error))?;
    }
    Ok(())
}
