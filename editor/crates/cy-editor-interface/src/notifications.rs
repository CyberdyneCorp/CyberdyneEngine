//! Notifications that do not interrupt, and modals that are reserved for decisions.
//!
//! `editor-ui-ux`: "Non-blocking information SHALL be presented as **notifications that do not steal
//! focus** or block input, and SHALL be dismissible and reviewable afterwards. **Modal dialogs SHALL
//! be reserved for decisions that genuinely cannot proceed without input**, and their number SHALL
//! be minimised. A notification SHALL offer an action where one is meaningful."
//!
//! --- HOW "DOES NOT STEAL FOCUS" IS ENFORCED --------------------------------------------------------
//!
//! By there being nothing to steal it with. [`NotificationCentre`] has no method that takes focus, no
//! flag that requests it, and no callback that could run while a user is typing: it drains the
//! service's log when the shell pumps it, and everything it produces is a value the shell draws
//! wherever it draws notifications. The test that matters is the specification's own scenario — an
//! import fails while a user is mid-edit — and what it asserts is that the in-progress edit is
//! exactly where it was.
//!
//! --- WHY A MODAL IS A CONSTRUCTOR THAT CAN FAIL ----------------------------------------------------
//!
//! "A modal dialog used for information that could be a notification" is a forbidden pattern, and it
//! is forbidden because it is *easy*: a modal is the shortest way to be sure somebody saw something.
//! So [`Modal::decision`] refuses one with fewer than two choices — that is information, and
//! information is a notification — and [`Modal::destructive`] refuses one that does not say what
//! will be lost, which is the other forbidden pattern, "a confirmation prompt that does not say what
//! will be lost".

use std::time::{Duration, Instant};

use cy_editor_commands::Arguments;
use cy_editor_core::observe::Cursor;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_services::{Editor, Notification, Severity};
use cy_editor_visual::Semantic;

/// Something a notification offers to do about itself.
///
/// A command identifier rather than a closure: the action a notification offers is an action, and
/// "every user-invokable action SHALL be registered in a command registry" has no exception for the
/// ones offered by notifications.
#[derive(Clone, PartialEq, Debug)]
pub struct Offer {
    /// What the button says.
    pub label: String,
    /// The command it invokes.
    pub command: String,
    /// The arguments it invokes it with.
    pub arguments: Arguments,
}

impl Offer {
    /// An offer to invoke a command.
    pub fn new(label: impl Into<String>, command: impl Into<String>) -> Self {
        Self {
            label: label.into(),
            command: command.into(),
            arguments: Arguments::new(),
        }
    }

    /// Supply an argument.
    #[must_use]
    pub fn with(mut self, name: impl Into<String>, value: cy_editor_core::Value) -> Self {
        self.arguments = self.arguments.with(name, value);
        self
    }
}

/// One notification, as the interface holds it.
#[derive(Clone, PartialEq, Debug)]
pub struct Toast {
    /// What the editor said.
    pub notification: Notification,
    /// What it offers to do about it, when something is meaningful.
    pub offer: Option<Offer>,
    /// Whether the user has dismissed it. Dismissed is not deleted: it stays reviewable.
    pub dismissed: bool,
    /// When it first appeared, so a transient one can retire itself.
    ///
    /// See [`NotificationCentre::retire_transient`]. It is on the toast rather than on the
    /// notification because it is a fact about the *screen* — the same message arriving twice is two
    /// toasts and one history of two entries.
    pub shown_at: Instant,
}

impl Toast {
    /// The semantic role this notification is drawn in.
    #[must_use]
    pub const fn role(&self) -> Semantic {
        match self.notification.severity {
            Severity::Info => Semantic::Active,
            Severity::Warning => Semantic::Warning,
            Severity::Error => Semantic::Error,
        }
    }

    /// The line a user reads: the state in words, then the message, then the remedy.
    ///
    /// The severity is spelled out because colour is never the sole encoding, and the remedy is
    /// included because a notification that reports a failure without one is the forbidden "message
    /// that states a failure without stating a cause or a remedy".
    #[must_use]
    pub fn line(&self) -> String {
        let mut line = format!("{} · {}", self.role().label(), self.notification.message);
        if let Some(remedy) = self
            .notification
            .problem
            .as_ref()
            .and_then(|problem| problem.remedy.as_ref())
        {
            line.push_str(" — ");
            line.push_str(remedy);
        }
        line
    }
}

/// How many notifications are shown at once before the rest are only in the history.
pub const VISIBLE: usize = 4;

/// How many are kept for review.
pub const KEPT: usize = 128;

/// What the editor has said, and what the user has done about it.
#[derive(Debug, Default)]
pub struct NotificationCentre {
    toasts: Vec<Toast>,
    cursor: Cursor,
}

impl NotificationCentre {
    /// An empty centre.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Take everything the editor has said since the last pump.
    ///
    /// Called once a frame by the shell. It cannot interrupt anything: it appends to a list.
    pub fn pump(&mut self, editor: &Editor) -> usize {
        let arrived: Vec<Notification> = editor
            .notifications
            .drain_from(&mut self.cursor)
            .into_iter()
            .cloned()
            .collect();
        let count = arrived.len();
        for notification in arrived {
            let offer = offer_for(&notification);
            self.toasts.push(Toast {
                notification,
                offer,
                dismissed: false,
                shown_at: Instant::now(),
            });
        }
        if self.toasts.len() > KEPT {
            self.toasts.drain(..self.toasts.len() - KEPT);
        }
        count
    }

    /// The notifications on screen: the most recent undismissed ones, newest first.
    #[must_use]
    pub fn showing(&self) -> Vec<&Toast> {
        self.toasts
            .iter()
            .rev()
            .filter(|toast| !toast.dismissed)
            .take(VISIBLE)
            .collect()
    }

    /// The notifications on screen, each with its position in the history.
    ///
    /// The index is what [`NotificationCentre::dismiss`] takes, and it is **not** the position in
    /// [`NotificationCentre::showing`] — that list is newest-first and skips what has been
    /// dismissed. Handing an interface the two separately is how a user clicks "dismiss" on one
    /// notification and watches a different one disappear, so the pair travels together.
    #[must_use]
    pub fn showing_indexed(&self) -> Vec<(usize, &Toast)> {
        self.toasts
            .iter()
            .enumerate()
            .rev()
            .filter(|(_, toast)| !toast.dismissed)
            .take(VISIBLE)
            .collect()
    }

    /// Retire the informational notifications that have been on screen longer than `after`.
    ///
    /// Returns how many were retired. Only [`Severity::Info`] with nothing to offer: a warning, a
    /// failure, and anything carrying an action a user might take stay until they are dismissed,
    /// because those are the ones that were worth interrupting for.
    ///
    /// This exists because "notifications SHALL NOT interrupt" has a slow failure mode as well as a
    /// fast one. A toast that steals focus interrupts immediately; a stack of four that never leaves
    /// covers the panel underneath and interrupts every subsequent glance. Retiring the transient
    /// ones costs nothing — [`NotificationCentre::history`] still has all of them, which is what
    /// "reviewable afterwards" means.
    pub fn retire_transient(&mut self, after: Duration) -> usize {
        let now = Instant::now();
        let mut retired = 0;
        for toast in &mut self.toasts {
            let transient = !toast.dismissed
                && toast.offer.is_none()
                && toast.notification.severity == Severity::Info;
            if transient && now.duration_since(toast.shown_at) >= after {
                toast.dismissed = true;
                retired += 1;
            }
        }
        retired
    }

    /// Everything the editor has said, oldest first — the reviewable history.
    ///
    /// "SHALL be dismissible and **reviewable afterwards**": dismissing removes a notification from
    /// the screen and not from here, because the commonest thing a user does after dismissing one is
    /// wonder what it said.
    #[must_use]
    pub fn history(&self) -> &[Toast] {
        &self.toasts
    }

    /// How many are on screen.
    #[must_use]
    pub fn showing_count(&self) -> usize {
        self.showing().len()
    }

    /// Dismiss the notification at a position in the history.
    pub fn dismiss(&mut self, index: usize) {
        if let Some(toast) = self.toasts.get_mut(index) {
            toast.dismissed = true;
        }
    }

    /// Dismiss everything on screen.
    pub fn dismiss_all(&mut self) {
        for toast in &mut self.toasts {
            toast.dismissed = true;
        }
    }
}

/// The action a notification offers, derived from what it is about.
///
/// Two today, both from failures the editor already produces: a document with recoverable
/// transactions offers recovery, and anything carrying a problem offers the log. A notification with
/// nothing useful to offer offers nothing, which is honest — a button that does nothing is worse
/// than no button.
fn offer_for(notification: &Notification) -> Option<Offer> {
    if notification.message.contains("recoverable transaction") {
        return Some(Offer::new("Recover", "file.recover"));
    }
    if notification.severity == Severity::Error {
        return Some(Offer::new("Show log", "window.output-log"));
    }
    None
}

/// One choice in a modal.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Choice {
    /// What the button says.
    pub label: String,
    /// Whether choosing it is the destructive one, so the interface can draw it as such.
    pub destructive: bool,
}

impl Choice {
    /// An ordinary choice.
    pub fn new(label: impl Into<String>) -> Self {
        Self {
            label: label.into(),
            destructive: false,
        }
    }

    /// The choice that loses something.
    pub fn destructive(label: impl Into<String>) -> Self {
        Self {
            label: label.into(),
            destructive: true,
        }
    }
}

/// A decision the editor genuinely cannot proceed without.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Modal {
    /// The question.
    pub question: String,
    /// What happens — and for a destructive choice, precisely what will be lost.
    pub consequence: String,
    /// The choices, at least two.
    pub choices: Vec<Choice>,
}

impl Modal {
    /// A decision, refusing anything that is really information.
    pub fn decision(
        question: impl Into<String>,
        consequence: impl Into<String>,
        choices: Vec<Choice>,
    ) -> Result<Self> {
        let question = question.into();
        if choices.len() < 2 {
            return Err(Problem::new(
                format!("show a modal asking {question:?}"),
                "it offers fewer than two choices, which makes it information rather than a \
                 decision",
            )
            .with_remedy(
                "post it as a notification; a modal is for a decision the editor cannot proceed \
                 without",
            ));
        }
        let consequence = consequence.into();
        if consequence.trim().is_empty() {
            return Err(Problem::new(
                format!("show a modal asking {question:?}"),
                "it does not say what happens",
            )
            .with_remedy("state the consequence of each choice, in the words a user needs"));
        }
        Ok(Self {
            question,
            consequence,
            choices,
        })
    }

    /// A confirmation that says **precisely what will be lost**.
    ///
    /// The forbidden pattern is "a confirmation prompt that does not say what will be lost", so the
    /// loss is a parameter and an empty one is refused.
    pub fn destructive(
        question: impl Into<String>,
        will_be_lost: impl Into<String>,
        confirm: impl Into<String>,
    ) -> Result<Self> {
        let will_be_lost = will_be_lost.into();
        if will_be_lost.trim().is_empty() {
            return Err(Problem::new(
                "show a confirmation",
                "it does not say what will be lost",
            )
            .with_remedy(
                "name the thing and its quantity: \"12 unsaved changes to worlds/city.cyworld\"",
            ));
        }
        Self::decision(
            question,
            format!("This will lose {will_be_lost}."),
            vec![Choice::new("Cancel"), Choice::destructive(confirm)],
        )
    }
}

/// How long an informational notification stays on screen before it retires itself.
///
/// Long enough to read a sentence, short enough that a run of successful commands does not build a
/// wall over the panel underneath. It is a constant here rather than a caller's choice so that every
/// window agrees, and so that changing it is one edit with this reasoning beside it.
pub const TRANSIENT: Duration = Duration::from_secs(6);

#[cfg(test)]
mod tests {
    use cy_editor_core::Value;
    use cy_editor_core::ids::{FieldId, TypeId};

    use super::*;

    #[test]
    fn the_index_a_dismissal_uses_is_the_one_that_came_with_the_toast() {
        // A regression test for a defect that is invisible in code review and obvious in use: the
        // position in `showing()` is newest-first and skips dismissed entries, and `dismiss` takes a
        // position in the history. Passing one to the other dismisses the wrong notification.
        let mut editor = Editor::default();
        for message in ["first", "second", "third"] {
            editor.notifications.post(Notification::info(message));
        }
        let mut centre = NotificationCentre::new();
        centre.pump(&editor);

        let (index, toast) = centre.showing_indexed()[0];
        assert_eq!(
            toast.notification.message, "third",
            "showing is newest first"
        );
        centre.dismiss(index);
        assert!(
            centre.history()[2].dismissed,
            "dismissing the newest toast dismissed something else"
        );
        assert_eq!(centre.showing_count(), 2);
    }

    #[test]
    fn an_informational_notification_retires_and_a_failure_does_not() {
        // "Notifications SHALL NOT interrupt" fails slowly as well as quickly: four toasts that
        // never leave cover the panel underneath and interrupt every subsequent glance. What must
        // not retire is anything a user might still need to act on.
        let mut editor = Editor::default();
        editor.notifications.post(Notification::info("saved"));
        editor.notifications.post(Notification::warning(
            "a world has recoverable transactions",
        ));
        editor.notifications.post(Notification::error(
            "open the world",
            Problem::new("open the world", "it is not there").with_remedy("check the path"),
        ));
        let mut centre = NotificationCentre::new();
        centre.pump(&editor);
        assert_eq!(centre.showing_count(), 3);

        // Zero, so the test does not sleep: everything already on screen is older than nothing.
        assert_eq!(centre.retire_transient(Duration::ZERO), 1);
        assert_eq!(centre.showing_count(), 2);
        assert!(
            centre
                .showing()
                .iter()
                .all(|toast| toast.notification.severity != Severity::Info),
            "an informational notification survived retirement"
        );
        // And it is still reviewable, which is the half that makes retiring it acceptable at all.
        assert_eq!(centre.history().len(), 3);
    }

    #[test]
    fn a_background_failure_does_not_interrupt_an_edit_in_progress() {
        // "WHEN an import fails while a user is editing a value THEN a notification SHALL appear
        // without taking focus, and SHALL remain reviewable."
        let mut editor = Editor::default();
        let mut inspector = crate::inspector::GeneratedInspector::new();
        inspector.begin_edit(TypeId::from_raw(1), FieldId::from_raw(1), Value::Float(2.5));

        let mut centre = NotificationCentre::new();
        editor.notifications.post(Notification::error(
            "Importing meshes/rock.gltf failed",
            Problem::new("import meshes/rock.gltf", "the file is not valid glTF")
                .with_remedy("re-export it, or import the .glb beside it"),
        ));
        assert_eq!(centre.pump(&editor), 1);

        assert_eq!(
            inspector.editing().map(|edit| edit.value.clone()),
            Some(Value::Float(2.5)),
            "the notification did not touch what the user was typing"
        );
        assert_eq!(centre.showing().len(), 1);
        assert!(centre.showing()[0].offer.is_some(), "and it offers the log");
    }

    #[test]
    fn a_dismissed_notification_is_still_reviewable() {
        let mut editor = Editor::default();
        editor
            .notifications
            .post(Notification::info("Opened worlds/city.cyworld"));
        let mut centre = NotificationCentre::new();
        centre.pump(&editor);

        centre.dismiss(0);
        assert!(centre.showing().is_empty(), "it left the screen");
        assert_eq!(centre.history().len(), 1, "and not the history");
    }

    #[test]
    fn a_notification_states_its_severity_in_words_and_offers_the_remedy() {
        let mut editor = Editor::default();
        editor.notifications.post(Notification::error(
            "The hosted runtime stopped",
            Problem::new("keep the runtime", "it closed the connection")
                .with_remedy("restart it; your documents are unaffected"),
        ));
        let mut centre = NotificationCentre::new();
        centre.pump(&editor);

        let line = centre.showing()[0].line();
        assert!(line.starts_with("Error · "), "{line}");
        assert!(line.contains("restart it"), "{line}");
    }

    #[test]
    fn only_the_most_recent_few_are_on_screen_and_the_rest_are_in_the_history() {
        let mut editor = Editor::default();
        for index in 0..20 {
            editor
                .notifications
                .post(Notification::info(format!("Imported asset {index}")));
        }
        let mut centre = NotificationCentre::new();
        centre.pump(&editor);

        assert_eq!(centre.showing_count(), VISIBLE);
        assert_eq!(centre.history().len(), 20);
    }

    #[test]
    fn a_recovery_offer_is_made_where_one_is_meaningful() {
        let mut editor = Editor::default();
        editor.notifications.post(Notification::warning(
            "worlds/city.cyworld has 3 recoverable transaction(s) from a previous session",
        ));
        let mut centre = NotificationCentre::new();
        centre.pump(&editor);
        assert_eq!(
            centre.showing()[0]
                .offer
                .as_ref()
                .map(|offer| offer.command.as_str()),
            Some("file.recover")
        );
    }

    #[test]
    fn an_informational_modal_is_refused_and_told_to_be_a_notification() {
        let problem = Modal::decision(
            "Import finished",
            "Twelve assets were imported.",
            vec![Choice::new("OK")],
        )
        .unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("notification"),
            "{problem}"
        );
    }

    #[test]
    fn a_confirmation_says_precisely_what_will_be_lost() {
        let modal = Modal::destructive(
            "Close worlds/city.cyworld without saving?",
            "12 changes made since the last save, including 3 new nodes",
            "Discard",
        )
        .unwrap();
        assert!(
            modal.consequence.contains("12 changes"),
            "{}",
            modal.consequence
        );
        assert!(modal.choices.iter().any(|choice| choice.destructive));

        assert!(
            Modal::destructive("Close?", "   ", "Discard").is_err(),
            "a confirmation that does not say what is lost is the forbidden pattern"
        );
    }

    #[test]
    fn an_offer_carries_the_arguments_the_command_needs() {
        let offer =
            Offer::new("Open", "file.open").with("asset", Value::Text("meshes/rock.gltf".into()));
        assert_eq!(
            offer.arguments.text("asset"),
            Some("meshes/rock.gltf"),
            "a notification's action is a command invocation like any other"
        );
    }
}
