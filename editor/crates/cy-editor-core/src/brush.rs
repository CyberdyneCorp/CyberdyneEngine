//! Versioned spatial brush values shared by editor surfaces and domain services.

use crate::codec::{Reader, Writer};
use crate::problem::{Problem, Result};

/// Wire schema understood by terrain authoring and asynchronous evaluation.
pub const STROKE_SCHEMA_VERSION: u32 = 1;

/// Controls captured with every completed brush gesture.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Brush {
    /// Radius in domain-world units.
    pub radius: f32,
    /// Tool strength in the inclusive range zero to one.
    pub strength: f32,
    /// Edge softness in the inclusive range zero to one.
    pub falloff: f32,
    /// Minimum normalised distance between samples.
    pub spacing: f32,
}

impl Default for Brush {
    fn default() -> Self {
        Self {
            radius: 8.0,
            strength: 0.5,
            falloff: 0.5,
            spacing: 0.01,
        }
    }
}

impl Brush {
    /// Refuse values which could create an unbounded or non-replayable stroke.
    pub fn validate(self) -> Result<()> {
        if !self.radius.is_finite() || !(0.01..=10_000.0).contains(&self.radius) {
            return Err(Problem::new(
                "configure a brush",
                "radius must be finite and between 0.01 and 10000",
            ));
        }
        for (name, value) in [("strength", self.strength), ("falloff", self.falloff)] {
            if !value.is_finite() || !(0.0..=1.0).contains(&value) {
                return Err(Problem::new(
                    "configure a brush",
                    format!("{name} must be finite and between zero and one"),
                ));
            }
        }
        if !self.spacing.is_finite() || !(0.0..=1.0).contains(&self.spacing) {
            return Err(Problem::new(
                "configure a brush",
                "spacing must be finite and between zero and one",
            ));
        }
        Ok(())
    }
}

/// One pressure-sensitive point in normalised surface coordinates.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Sample {
    /// Horizontal coordinate in the inclusive range zero to one.
    pub x: f32,
    /// Vertical coordinate in the inclusive range zero to one.
    pub y: f32,
    /// Input pressure in the inclusive range zero to one.
    pub pressure: f32,
}

impl Sample {
    /// Construct one validated, deterministic sample.
    pub fn new(x: f32, y: f32, pressure: f32) -> Result<Self> {
        if [x, y, pressure]
            .into_iter()
            .any(|value| !value.is_finite() || !(0.0..=1.0).contains(&value))
        {
            return Err(Problem::new(
                "sample a painting gesture",
                "coordinates and pressure must be finite and between zero and one",
            ));
        }
        Ok(Self { x, y, pressure })
    }
}

/// One complete gesture, ready to become one document transaction.
#[derive(Clone, PartialEq, Debug)]
pub struct Stroke {
    /// Brush state captured at pointer-down.
    pub brush: Brush,
    /// Ordered input samples.
    pub samples: Vec<Sample>,
}

impl Stroke {
    /// Deterministic, versioned payload handed to commands and domain evaluators.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u32(STROKE_SCHEMA_VERSION);
        writer.f32(self.brush.radius);
        writer.f32(self.brush.strength);
        writer.f32(self.brush.falloff);
        writer.f32(self.brush.spacing);
        writer.u32(u32::try_from(self.samples.len()).unwrap_or(u32::MAX));
        for sample in &self.samples {
            writer.f32(sample.x);
            writer.f32(sample.y);
            writer.f32(sample.pressure);
        }
        writer.finish()
    }

    /// Decode only the known schema, refusing truncated, newer, and trailing data.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let version = reader.u32()?;
        if version != STROKE_SCHEMA_VERSION {
            return Err(Problem::new(
                "read a painting stroke",
                format!("stroke schema {version} is unsupported"),
            ));
        }
        let brush = Brush {
            radius: reader.f32()?,
            strength: reader.f32()?,
            falloff: reader.f32()?,
            spacing: reader.f32()?,
        };
        brush.validate()?;
        let count = reader.u32()? as usize;
        if count == 0 || count > 1_000_000 {
            return Err(Problem::new(
                "read a painting stroke",
                "a stroke must contain between one and one million samples",
            ));
        }
        let mut samples = Vec::with_capacity(count.min(4096));
        for _ in 0..count {
            samples.push(Sample::new(reader.f32()?, reader.f32()?, reader.f32()?)?);
        }
        if !reader.is_empty() {
            return Err(Problem::new(
                "read a painting stroke",
                format!("{} trailing bytes remain", reader.remaining()),
            ));
        }
        Ok(Self { brush, samples })
    }
}
