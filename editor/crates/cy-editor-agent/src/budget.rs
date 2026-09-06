//! The limits that stop an agent starving the editor.
//!
//! `editor-agent-interface`, "Agents hold a budget":
//!
//! > An agent connection SHALL NOT be able to starve the editor. The interface SHALL bound the rate
//! > of invocations, the cost of viewport renders, and the number of concurrent operations, and
//! > SHALL **report those limits to the agent** rather than failing opaquely.
//!
//! The reporting is the half that is easy to skip and the half that matters. An agent told "rate
//! limited" retries and is limited again; one told "sixty invocations per window, forty-one used,
//! the window resets in 0.7 seconds" waits. So every refusal here carries the whole state, and
//! [`BudgetReport`] is offered before anything is refused.
//!
//! # Why the clock is supplied rather than read
//!
//! `charge` takes the caller's monotonic reading. A budget that read a clock could not be tested
//! without sleeping, and a test that sleeps is a test that is flaky on a loaded machine — which is
//! the same argument `cy::assets::FileWatcher::poll` makes for taking `now_ns`.

use cy_editor_core::problem::{Problem, Result};

/// What a connection may spend.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Budget {
    /// Invocations allowed within one window.
    pub invocations_per_window: u32,
    /// How long the window is, in milliseconds.
    pub window_millis: u64,
    /// Operations that may be in flight at once. A long import and a long bake are one each.
    pub concurrent_operations: u32,
    /// Viewport renders allowed within one window. Separate from invocations because a render costs
    /// a frame of the editor's own budget and an invocation usually costs nothing.
    pub renders_per_window: u32,
}

impl Default for Budget {
    /// Deliberately generous for interaction and firm against a loop. Sixty invocations a second is
    /// far more than an agent reasoning between calls will use and far less than a runaway loop
    /// will attempt; four renders a second is a quarter of the editor's frames.
    fn default() -> Self {
        Self {
            invocations_per_window: 60,
            window_millis: 1000,
            concurrent_operations: 4,
            renders_per_window: 4,
        }
    }
}

/// What is left, and when it refills. Handed to the agent rather than kept.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct BudgetReport {
    /// The limits in force.
    pub budget: Budget,
    /// Invocations used in the current window.
    pub invocations_used: u32,
    /// Renders used in the current window.
    pub renders_used: u32,
    /// Operations in flight.
    pub operations_running: u32,
    /// Milliseconds until the window resets.
    pub window_resets_in_millis: u64,
}

impl BudgetReport {
    /// A sentence an agent can act on, which is the whole point of reporting rather than refusing.
    #[must_use]
    pub fn describe(&self) -> String {
        format!(
            "{} of {} invocations and {} of {} renders used; {} of {} operations running; the \
             window resets in {} ms",
            self.invocations_used,
            self.budget.invocations_per_window,
            self.renders_used,
            self.budget.renders_per_window,
            self.operations_running,
            self.budget.concurrent_operations,
            self.window_resets_in_millis
        )
    }
}

/// What a charge cost.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Throttle {
    /// It was within budget.
    Allowed,
    /// It was not. The agent is told what is left and when.
    Refused,
}

/// One connection's spending, over a sliding window.
#[derive(Clone, Debug)]
pub struct Spending {
    budget: Budget,
    window_started_millis: u64,
    invocations: u32,
    renders: u32,
    operations: u32,
}

impl Spending {
    /// A fresh budget, with the window starting at `now_millis`.
    #[must_use]
    pub const fn new(budget: Budget, now_millis: u64) -> Self {
        Self {
            budget,
            window_started_millis: now_millis,
            invocations: 0,
            renders: 0,
            operations: 0,
        }
    }

    /// The limits and what is left. Safe to ask at any time and never refused: an agent that has to
    /// spend an invocation to find out whether it may invoke is an agent in a loop.
    #[must_use]
    pub fn report(&self, now_millis: u64) -> BudgetReport {
        let elapsed = now_millis.saturating_sub(self.window_started_millis);
        let expired = elapsed >= self.budget.window_millis;
        BudgetReport {
            budget: self.budget,
            invocations_used: if expired { 0 } else { self.invocations },
            renders_used: if expired { 0 } else { self.renders },
            operations_running: self.operations,
            window_resets_in_millis: if expired {
                self.budget.window_millis
            } else {
                self.budget.window_millis - elapsed
            },
        }
    }

    fn roll(&mut self, now_millis: u64) {
        if now_millis.saturating_sub(self.window_started_millis) >= self.budget.window_millis {
            self.window_started_millis = now_millis;
            self.invocations = 0;
            self.renders = 0;
        }
    }

    /// Charge one invocation.
    pub fn charge_invocation(&mut self, now_millis: u64) -> Result<Throttle> {
        self.roll(now_millis);
        if self.invocations >= self.budget.invocations_per_window {
            return Err(self.refusal(now_millis, "invocations"));
        }
        self.invocations += 1;
        Ok(Throttle::Allowed)
    }

    /// Charge one viewport render.
    ///
    /// "A viewport render requested by an agent SHALL be scheduled against the same frame budget the
    /// editor's own viewport holds, and SHALL degrade rather than stall the interface." The
    /// scheduling is the viewport's; this is the ceiling on how often one may be asked for.
    pub fn charge_render(&mut self, now_millis: u64) -> Result<Throttle> {
        self.roll(now_millis);
        if self.renders >= self.budget.renders_per_window {
            return Err(self.refusal(now_millis, "viewport renders"));
        }
        self.renders += 1;
        Ok(Throttle::Allowed)
    }

    /// Take one of the concurrent-operation slots.
    pub fn begin_operation(&mut self, now_millis: u64) -> Result<Throttle> {
        if self.operations >= self.budget.concurrent_operations {
            return Err(self.refusal(now_millis, "concurrent operations"));
        }
        self.operations += 1;
        Ok(Throttle::Allowed)
    }

    /// Give one back. Saturating, so a caller that settles an operation twice does not underflow
    /// into an effectively unlimited budget.
    pub const fn end_operation(&mut self) {
        self.operations = self.operations.saturating_sub(1);
    }

    fn refusal(&self, now_millis: u64, what: &str) -> Problem {
        let report = self.report(now_millis);
        Problem::new(
            format!("spend one of this connection's {what}"),
            "the connection is at its budget",
        )
        .with_remedy(format!(
            "wait and retry: {}. Ask for the budget rather than retrying blindly.",
            report.describe()
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_loop_is_throttled_and_told_what_is_left() {
        // "WHEN an agent issues requests as fast as it can THEN the editor SHALL remain interactive
        // and the agent SHALL be told it is being throttled."
        let budget = Budget {
            invocations_per_window: 3,
            window_millis: 1000,
            ..Budget::default()
        };
        let mut spending = Spending::new(budget, 0);
        for _ in 0..3 {
            assert_eq!(spending.charge_invocation(0).unwrap(), Throttle::Allowed);
        }
        let refused = spending.charge_invocation(0).unwrap_err();
        let remedy = refused.remedy.unwrap();
        assert!(remedy.contains("3 of 3 invocations"), "{remedy}");
        assert!(remedy.contains("1000 ms"), "{remedy}");
    }

    #[test]
    fn the_window_rolls_and_the_budget_refills() {
        let budget = Budget {
            invocations_per_window: 1,
            window_millis: 100,
            ..Budget::default()
        };
        let mut spending = Spending::new(budget, 0);
        spending.charge_invocation(0).unwrap();
        assert!(spending.charge_invocation(50).is_err());
        assert!(spending.charge_invocation(100).is_ok());
    }

    #[test]
    fn renders_have_their_own_ceiling() {
        // A render costs a frame of the editor's own budget; an invocation usually costs nothing. One
        // ceiling for both would either throttle reasoning or admit a render storm.
        let budget = Budget {
            invocations_per_window: 100,
            renders_per_window: 1,
            window_millis: 1000,
            ..Budget::default()
        };
        let mut spending = Spending::new(budget, 0);
        spending.charge_render(0).unwrap();
        assert!(spending.charge_render(0).is_err());
        assert!(spending.charge_invocation(0).is_ok());
    }

    #[test]
    fn concurrent_operations_are_bounded_and_returned() {
        let budget = Budget {
            concurrent_operations: 2,
            ..Budget::default()
        };
        let mut spending = Spending::new(budget, 0);
        spending.begin_operation(0).unwrap();
        spending.begin_operation(0).unwrap();
        assert!(spending.begin_operation(0).is_err());
        spending.end_operation();
        assert!(spending.begin_operation(0).is_ok());
    }

    #[test]
    fn settling_an_operation_twice_does_not_grant_an_unlimited_budget() {
        let mut spending = Spending::new(Budget::default(), 0);
        spending.begin_operation(0).unwrap();
        spending.end_operation();
        spending.end_operation();
        assert_eq!(spending.report(0).operations_running, 0);
    }

    #[test]
    fn asking_for_the_budget_costs_nothing() {
        let mut spending = Spending::new(Budget::default(), 0);
        let before = spending.report(0);
        for _ in 0..10 {
            let _ = spending.report(0);
        }
        assert_eq!(spending.report(0), before);
        spending.charge_invocation(0).unwrap();
        assert_eq!(spending.report(0).invocations_used, 1);
    }
}
