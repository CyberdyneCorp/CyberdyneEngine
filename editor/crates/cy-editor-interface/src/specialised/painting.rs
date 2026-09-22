// SPDX-License-Identifier: MIT
//! The shared spatial painting surface used by terrain and the later field editors.
//!
//! It owns only an in-progress gesture and brush presentation state. A completed gesture becomes a
//! versioned value which a command records, so cancelling pointer capture has no persistent effect.

use cy_editor_core::problem::{Problem, Result};

pub use cy_editor_core::brush::{Brush, STROKE_SCHEMA_VERSION, Sample, Stroke};

/// Stable identity of the one shared painting surface.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct SurfaceId(u64);

impl SurfaceId {
    /// Numeric identity used by interface tests and persisted layouts.
    #[must_use]
    pub const fn as_u64(self) -> u64 {
        self.0
    }
}

/// Presentation state shared by every spatial brush editor.
#[derive(Debug)]
pub struct PaintingSurface {
    id: SurfaceId,
    brush: Brush,
    active: Option<Stroke>,
}

impl PaintingSurface {
    /// Create one surface with a stable host-owned identity.
    #[must_use]
    pub fn new(id: u64) -> Self {
        Self {
            id: SurfaceId(id),
            brush: Brush::default(),
            active: None,
        }
    }

    /// Identity proving all painting domains use this instance.
    #[must_use]
    pub const fn id(&self) -> SurfaceId {
        self.id
    }

    /// Current brush controls.
    #[must_use]
    pub const fn brush(&self) -> Brush {
        self.brush
    }

    /// Update brush controls after validation.
    pub fn set_brush(&mut self, brush: Brush) -> Result<()> {
        brush.validate()?;
        self.brush = brush;
        Ok(())
    }

    /// Start a gesture, capturing the brush so later UI changes cannot rewrite it.
    pub fn begin(&mut self, first: Sample) -> Result<()> {
        if self.active.is_some() {
            return Err(Problem::new(
                "begin a painting gesture",
                "another gesture is already active",
            ));
        }
        self.brush.validate()?;
        self.active = Some(Stroke {
            brush: self.brush,
            samples: vec![first],
        });
        Ok(())
    }

    /// Append a sample when it is far enough from the previous one.
    pub fn sample(&mut self, sample: Sample) -> Result<bool> {
        let stroke = self.active.as_mut().ok_or_else(|| {
            Problem::new("sample a painting gesture", "no gesture is active")
                .with_remedy("begin the gesture at pointer-down")
        })?;
        let previous = stroke.samples.last().expect("begin supplies a sample");
        if (sample.x - previous.x).hypot(sample.y - previous.y) < stroke.brush.spacing {
            return Ok(false);
        }
        stroke.samples.push(sample);
        Ok(true)
    }

    /// Complete a gesture. Its caller records this value as one transaction.
    pub fn finish(&mut self) -> Result<Stroke> {
        self.active.take().ok_or_else(|| {
            Problem::new("finish a painting gesture", "no gesture is active")
                .with_remedy("begin the gesture at pointer-down")
        })
    }

    /// Cancel pointer capture without producing persistent authored state.
    pub fn cancel(&mut self) {
        self.active = None;
    }

    /// Whether pointer capture currently owns a gesture.
    #[must_use]
    pub const fn is_active(&self) -> bool {
        self.active.is_some()
    }

    /// Samples in the in-progress gesture, for transient overlay drawing only.
    pub fn active_samples(&self) -> impl Iterator<Item = Sample> + '_ {
        self.active
            .iter()
            .flat_map(|stroke| stroke.samples.iter().copied())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_completed_gesture_round_trips_as_one_versioned_value() {
        let mut surface = PaintingSurface::new(7);
        surface.begin(Sample::new(0.1, 0.2, 0.8).unwrap()).unwrap();
        assert!(surface.sample(Sample::new(0.5, 0.6, 1.0).unwrap()).unwrap());
        let stroke = surface.finish().unwrap();
        assert_eq!(Stroke::decode(&stroke.encode()).unwrap(), stroke);
        assert!(!surface.is_active());
    }

    #[test]
    fn cancelling_a_gesture_produces_nothing() {
        let mut surface = PaintingSurface::new(7);
        surface.begin(Sample::new(0.1, 0.2, 0.8).unwrap()).unwrap();
        surface.cancel();
        assert!(surface.finish().is_err());
    }

    #[test]
    fn spacing_filters_dense_pointer_events_without_changing_order() {
        let mut surface = PaintingSurface::new(7);
        surface
            .set_brush(Brush {
                spacing: 0.2,
                ..Brush::default()
            })
            .unwrap();
        surface.begin(Sample::new(0.0, 0.0, 1.0).unwrap()).unwrap();
        assert!(!surface.sample(Sample::new(0.1, 0.0, 1.0).unwrap()).unwrap());
        assert!(surface.sample(Sample::new(0.3, 0.0, 1.0).unwrap()).unwrap());
        assert_eq!(surface.finish().unwrap().samples.len(), 2);
    }
}
