//! Viewport cost, cadence and degradation: what a viewport declares, and what it says when it
//! cannot keep up.
//!
//! `editor-viewport-and-gizmos` — "Viewport performance and degradation":
//!
//! > The viewport SHALL declare and honour a rendering budget, and SHALL degrade **visibly and
//! > explicitly** rather than stalling: reducing rate for unfocused viewports, lowering resolution,
//! > or pausing background viewports. Degradation SHALL be surfaced to the user, so that a
//! > lower-quality image is never mistaken for the project's appearance. The editor SHALL never block
//! > its interface thread on runtime rendering; a stalled runtime SHALL produce a stale-frame
//! > indication rather than a frozen editor.
//!
//! And, from "Multiple viewports and view states":
//!
//! > Each viewport SHALL **declare its cost**, and the editor SHALL be able to limit rendering of
//! > unfocused or hidden viewports rather than paying for all of them continuously.
//!
//! > **WHEN** a viewport is not visible **THEN** it SHALL not be rendered.
//!
//! --- THE LADDER, AND WHY IT IS IN THIS ORDER --------------------------------------------------------
//!
//! [`ViewportBudget::degrade`] gives up quality in the order that costs a user the least:
//!
//! 1. **Rate** first. A viewport nobody is looking at can be updated less often with no loss of
//!    information; the image is still the project's appearance, it is just older.
//! 2. **Resolution** second. This changes what the image looks like, so it is second and it is the
//!    first rung that must be surfaced as "not what the project looks like".
//! 3. **Pause** only for a viewport that is not visible, where there is no image to degrade.
//!
//! The order is a decision rather than an arbitrary sequence: reducing resolution before rate would
//! make a background viewport lie about appearance in order to buy a smoothness nobody is watching.
//!
//! --- COST IS DECLARED, NOT INFERRED ------------------------------------------------------------------
//!
//! [`ViewportCost`] is what a viewport says it needs — its pixels, and its measured frame time. The
//! arbiter that distributes a frame's budget across several viewports is `rendering-architecture`'s
//! and arrives at M7; what exists here is the declaration it will read, and the per-viewport decision
//! that can be made without one. Publishing the number now is what makes the later arbiter a
//! consumer rather than an archaeology exercise.

use crate::transport::{Degradation, TransportBudget};

/// How often a viewport asks to be rendered.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Cadence {
    /// The viewport the user is working in: the display's rate.
    #[default]
    Focused,
    /// Visible but not focused: a third of the rate, which is enough to see a change happen.
    Unfocused,
    /// Not visible. **Not rendered at all** — the requirement's scenario, and the only cadence that
    /// costs nothing.
    Hidden,
}

impl Cadence {
    /// Whether the runtime should render this viewport at all.
    #[must_use]
    pub const fn should_render(self) -> bool {
        !matches!(self, Cadence::Hidden)
    }

    /// What the transport is asked for at this cadence.
    #[must_use]
    pub fn budget(self) -> TransportBudget {
        match self {
            Cadence::Focused => TransportBudget::default(),
            Cadence::Unfocused | Cadence::Hidden => TransportBudget::unfocused(),
        }
    }

    /// A name for the statistics overlay.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Cadence::Focused => "focused",
            Cadence::Unfocused => "unfocused",
            Cadence::Hidden => "hidden",
        }
    }
}

/// What a viewport says it costs.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct ViewportCost {
    /// How many pixels it covers at full resolution.
    pub pixels: u64,
    /// What the runtime last took to render it, in microseconds.
    pub measured_micros: u32,
    /// The cadence it is being rendered at.
    pub cadence: Cadence,
}

impl ViewportCost {
    /// The share of a wall-clock second this viewport would consume at its cadence.
    ///
    /// The number an arbiter will divide, and the number a statistics overlay shows when a user
    /// asks why four viewports are slower than one.
    #[must_use]
    pub fn duty_cycle(self) -> f32 {
        if !self.cadence.should_render() {
            return 0.0;
        }
        let interval = self.cadence.budget().requested_interval_micros.max(1);
        micros(self.measured_micros) / micros(interval)
    }
}

/// A microsecond count as a float. One place, so the lint exemption is justified once.
#[allow(
    clippy::cast_precision_loss,
    reason = "a frame time is far below 2^24 microseconds"
)]
fn micros(value: u32) -> f32 {
    value as f32
}

/// What a viewport is allowed to cost, and what it does when it costs more.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct ViewportBudget {
    /// What one frame of this viewport may take, in microseconds.
    pub frame_micros: u32,
    /// How far over budget a frame may go before the rate is reduced. A ratio, so the same setting
    /// suits a viewport of any size.
    pub reduce_rate_above: f32,
    /// How far over budget before the resolution is reduced as well.
    pub reduce_resolution_above: f32,
    /// The smallest fraction of full resolution this viewport will render at.
    pub minimum_resolution_scale: f32,
}

impl Default for ViewportBudget {
    fn default() -> Self {
        Self {
            frame_micros: 16_667,
            reduce_rate_above: 1.25,
            reduce_resolution_above: 2.0,
            minimum_resolution_scale: 0.5,
        }
    }
}

impl ViewportBudget {
    /// What to give up, given what the last frame actually cost.
    ///
    /// Returns the degradation to request and the resolution scale to render at. Deciding both in
    /// one place is what keeps a viewport from reporting `ReducedRate` while rendering at half
    /// resolution — two fields set by two callers is how a quality indicator ends up lying.
    #[must_use]
    pub fn degrade(self, cost: ViewportCost) -> (Degradation, f32) {
        if !cost.cadence.should_render() {
            return (Degradation::Paused, 1.0);
        }
        let over = micros(cost.measured_micros) / micros(self.frame_micros.max(1));
        if over >= self.reduce_resolution_above {
            let scale = (self.reduce_resolution_above / over)
                .sqrt()
                .clamp(self.minimum_resolution_scale, 1.0);
            return (Degradation::ReducedResolution, scale);
        }
        if over >= self.reduce_rate_above {
            return (Degradation::ReducedRate, 1.0);
        }
        (Degradation::None, 1.0)
    }

    /// Whether an image produced under this degradation may be presented as the project's
    /// appearance.
    ///
    /// A reduced RATE still shows the shipping image, just less often; a reduced RESOLUTION does
    /// not. That distinction is the whole reason [`Degradation`] is a reason rather than a boolean.
    #[must_use]
    pub const fn image_is_representative(degradation: Degradation) -> bool {
        matches!(degradation, Degradation::None | Degradation::ReducedRate)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_hidden_viewport_is_not_rendered_and_costs_nothing() {
        // "WHEN a viewport is not visible THEN it SHALL not be rendered."
        assert!(!Cadence::Hidden.should_render());
        assert!(Cadence::Focused.should_render());
        assert!(Cadence::Unfocused.should_render());

        let cost = ViewportCost {
            pixels: 1920 * 1080,
            measured_micros: 8_000,
            cadence: Cadence::Hidden,
        };
        assert!(cost.duty_cycle().abs() < f32::EPSILON);
        assert_eq!(
            ViewportBudget::default().degrade(cost),
            (Degradation::Paused, 1.0)
        );
    }

    #[test]
    fn an_unfocused_viewport_asks_for_less_and_therefore_costs_less() {
        let measured = 8_000;
        let focused = ViewportCost {
            pixels: 1920 * 1080,
            measured_micros: measured,
            cadence: Cadence::Focused,
        };
        let unfocused = ViewportCost {
            cadence: Cadence::Unfocused,
            ..focused
        };
        assert!(unfocused.duty_cycle() < focused.duty_cycle() * 0.5);
    }

    #[test]
    fn the_ladder_gives_up_rate_before_it_gives_up_appearance() {
        let budget = ViewportBudget::default();
        let within = ViewportCost {
            pixels: 0,
            measured_micros: 16_000,
            cadence: Cadence::Focused,
        };
        assert_eq!(budget.degrade(within), (Degradation::None, 1.0));

        let over = ViewportCost {
            measured_micros: 24_000,
            ..within
        };
        assert_eq!(budget.degrade(over).0, Degradation::ReducedRate);

        let far_over = ViewportCost {
            measured_micros: 66_668,
            ..within
        };
        let (degradation, scale) = budget.degrade(far_over);
        assert_eq!(degradation, Degradation::ReducedResolution);
        assert!(
            scale < 1.0 && scale >= budget.minimum_resolution_scale,
            "{scale}"
        );
    }

    #[test]
    fn resolution_never_falls_below_the_floor_however_bad_it_gets() {
        let budget = ViewportBudget::default();
        let dire = ViewportCost {
            pixels: 0,
            measured_micros: 5_000_000,
            cadence: Cadence::Focused,
        };
        let (_, scale) = budget.degrade(dire);
        assert!(
            (scale - budget.minimum_resolution_scale).abs() < 1e-6,
            "{scale}"
        );
    }

    #[test]
    fn a_reduced_rate_still_shows_the_shipping_image_and_a_reduced_resolution_does_not() {
        assert!(ViewportBudget::image_is_representative(Degradation::None));
        assert!(ViewportBudget::image_is_representative(
            Degradation::ReducedRate
        ));
        assert!(!ViewportBudget::image_is_representative(
            Degradation::ReducedResolution
        ));
        assert!(!ViewportBudget::image_is_representative(
            Degradation::Paused
        ));
    }

    #[test]
    fn degradation_and_scale_are_decided_together_so_they_cannot_disagree() {
        // Two fields set by two callers is how a quality indicator ends up saying "full quality"
        // over an upscaled image. One function returns both.
        // The implication that matters: an upscaled image is always reported as one. The converse
        // does not hold exactly at the threshold, where the ladder has just stepped onto the
        // resolution rung and the scale it computes is still 1 — which is honest rather than a bug,
        // and is why this asserts one direction and says so.
        let budget = ViewportBudget::default();
        for micros in [0, 8_000, 16_667, 20_000, 33_334, 40_000, 100_000] {
            let (degradation, scale) = budget.degrade(ViewportCost {
                pixels: 0,
                measured_micros: micros,
                cadence: Cadence::Focused,
            });
            if scale < 1.0 {
                assert_eq!(
                    degradation,
                    Degradation::ReducedResolution,
                    "{micros} µs upscales the image and does not say so"
                );
            }
            if degradation != Degradation::ReducedResolution {
                assert!(
                    (scale - 1.0).abs() < f32::EPSILON,
                    "{micros} µs reported {degradation:?} at scale {scale}"
                );
            }
        }
    }
}
