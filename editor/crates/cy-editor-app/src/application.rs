//! The application: services, a command registry, and the hosting mode they run under.

use cy_editor_commands::{Arguments, Outcome, Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_core::problem::Result;
use cy_editor_sdk::HostingMode;
use cy_editor_services::{Editor, RuntimeSession};

/// A running editor.
pub struct Application {
    /// The authoritative state.
    pub editor: Editor,
    /// Every action the editor can perform.
    pub registry: Registry,
    /// What the current caller is permitted to do. The human at the interface is unrestricted; an
    /// agent connection declares its own and gets the narrowest useful default.
    pub scope: Scope,
}

impl Application {
    /// An editor with the built-in commands registered and no runtime attached.
    ///
    /// Fails only if a built-in command's metadata would not satisfy a caller that cannot see the
    /// interface — which makes construction a check of the registry every time it runs.
    pub fn new(actor: Actor) -> Result<Self> {
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry)?;
        Ok(Self {
            editor: Editor::new(actor),
            registry,
            scope: Scope::unrestricted(),
        })
    }

    /// Attach a hosted runtime over a Unix domain socket.
    ///
    /// Failure is reported and **not fatal**: an editor with no runtime is a mode rather than a
    /// broken state, and refusing to start because a runtime was not listening would make the
    /// out-of-process decision cost more than it saves.
    #[cfg(unix)]
    pub fn attach_hosted_runtime(&mut self, endpoint: impl AsRef<std::path::Path>) -> Result<()> {
        self.editor.runtime = RuntimeSession::connect_hosted(endpoint)?;
        Ok(())
    }

    /// The mode the editor is hosting its engine in.
    #[must_use]
    pub const fn hosting_mode(&self) -> HostingMode {
        self.editor.hosting_mode()
    }

    /// Invoke a command by identifier under the application's current scope.
    pub fn invoke(&mut self, id: &str, arguments: &Arguments) -> Result<Outcome> {
        self.registry
            .invoke(id, &self.scope, &mut self.editor, arguments)
    }

    /// One frame of housekeeping. Bounded and non-blocking; see [`Editor::pump`].
    pub fn pump(&mut self) {
        self.editor.pump();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_application_starts_with_no_runtime_and_a_validated_registry() {
        let application = Application::new(Actor::human("designer")).unwrap();
        assert_eq!(application.hosting_mode(), HostingMode::NoRuntime);
        assert!(!application.registry.is_empty());
        for metadata in application.registry.all() {
            metadata.validate().unwrap();
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_runtime_that_is_not_listening_is_reported_and_is_not_fatal() {
        let mut application = Application::new(Actor::human("designer")).unwrap();
        let problem = application
            .attach_hosted_runtime("/nonexistent/cyberdyne.sock")
            .unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
        assert_eq!(application.hosting_mode(), HostingMode::NoRuntime);

        // And the editor is still perfectly usable.
        application
            .editor
            .open_document("worlds/city.cyworld")
            .unwrap();
        application
            .invoke("scene.create-entity", &Arguments::new())
            .unwrap();
    }
}
