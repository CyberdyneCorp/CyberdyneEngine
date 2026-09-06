//! Notifications: what the editor tells the user, held so that nothing is missed.
//!
//! A bounded [`cy_editor_core::observe::EventLog`] rather than a callback list, for the reason
//! `observe` gives at length — and for one more that is specific to notifications: the most
//! important notification in this editor is "the hosted runtime crashed", and it arrives on a
//! background thread at a moment when the interface may not have asked for anything yet. A log with
//! per-reader cursors delivers it on the next frame; a callback list would have needed a subscriber
//! to already exist.

use cy_editor_core::observe::{Cursor, EventLog, Revision, Versioned};
use cy_editor_core::problem::Problem;

/// How loud a notification is.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Severity {
    /// Something happened that the user asked for.
    Info,
    /// Something worked but is worth knowing about.
    Warning,
    /// Something failed.
    Error,
}

/// One thing to tell the user.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Notification {
    /// How loud.
    pub severity: Severity,
    /// What to say.
    pub message: String,
    /// The failure behind it, when there is one — so the user can be told what would fix it.
    pub problem: Option<Problem>,
}

impl Notification {
    /// An informational notification.
    pub fn info(message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Info,
            message: message.into(),
            problem: None,
        }
    }

    /// A warning.
    pub fn warning(message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Warning,
            message: message.into(),
            problem: None,
        }
    }

    /// An error, carrying the problem so its remedy can be offered.
    pub fn error(message: impl Into<String>, problem: Problem) -> Self {
        Self {
            severity: Severity::Error,
            message: message.into(),
            problem: Some(problem),
        }
    }
}

/// How many notifications are held before the oldest is dropped.
///
/// Bounded, and the drop is reported by [`EventLog::dropped`] — the same rule the undo history
/// follows, for the same reason: a user who missed something and cannot tell is worse off than one
/// who knows they did.
const CAPACITY: usize = 256;

/// Everything the editor has to say.
#[derive(Debug)]
pub struct NotificationService {
    log: EventLog<Notification>,
    revision: Versioned<()>,
}

impl Default for NotificationService {
    fn default() -> Self {
        Self::new()
    }
}

impl NotificationService {
    /// An empty service.
    #[must_use]
    pub fn new() -> Self {
        Self {
            log: EventLog::new(CAPACITY),
            revision: Versioned::new(()),
        }
    }

    /// Post a notification.
    pub fn post(&mut self, notification: Notification) {
        self.log.push(notification);
        self.revision.update(|()| {});
    }

    /// The revision, so a status bar rebuilds only when something was posted.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.revision.revision()
    }

    /// A cursor at the end, for a reader that only wants what happens next.
    #[must_use]
    pub fn cursor(&self) -> Cursor {
        self.log.cursor_at_end()
    }

    /// Everything since `cursor`.
    pub fn drain_from(&self, cursor: &mut Cursor) -> Vec<&Notification> {
        self.log.drain_from(cursor)
    }

    /// How many notifications were dropped because the log was full.
    #[must_use]
    pub fn dropped(&self) -> u64 {
        self.log.dropped()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_reader_that_arrives_late_still_sees_what_is_held() {
        let mut service = NotificationService::new();
        service.post(Notification::info("Opened worlds/city.cyworld"));
        service.post(Notification::error(
            "The hosted runtime stopped",
            Problem::new("the hosted runtime", "it closed the connection")
                .with_remedy("restart it; your documents are unaffected"),
        ));

        let mut cursor = Cursor::default();
        let notifications = service.drain_from(&mut cursor);
        assert_eq!(notifications.len(), 2);
        assert_eq!(notifications[1].severity, Severity::Error);
        assert!(notifications[1].problem.as_ref().unwrap().remedy.is_some());
    }
}
