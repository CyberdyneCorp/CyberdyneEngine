#pragma once
// What a recorded frame did, and which of the three ways it was recorded. M8.c tasks 5.4 and 5.5.
//
// Split out of `capture.h` so that `slice.h` can carry a `CaptureReport` without pulling the RHI,
// the pipeline layer and the particle renderer into every translation unit of the sample. The
// implementation is capture.cpp; the argument for each mode is in capture.h.

#include <cy/core/base/types.h>

namespace cy::sample::slice {

/// What a capture records into the frame.
enum class CaptureMode : cy::u8 {
    /// An empty `FrameSinks`: assembled, compiled, barriered, executed, and nothing recorded. The
    /// state of every caller in this tree before M8.c, kept as the control task 5.5 asks for.
    Assembled = 0,
    /// The pipeline layer's five callbacks.
    Recorded,
    /// The same, plus the particle renderer attached to the transparent stage.
    RecordedWithParticles,
};

/// What one recorded frame did, read off the recording rather than predicted from the draw list.
struct CaptureReport {
    bool device = false;
    bool captured = false;
    cy::u32 passes = 0;
    cy::u32 prepass_draws = 0;
    cy::u32 opaque_draws = 0;
    cy::u32 transparent_draws = 0;
    cy::u32 skipped_draws = 0;
    cy::u32 extensions_run = 0;
    cy::u32 particles_drawn = 0;
    cy::u32 particles_dropped = 0;
    cy::u64 uploaded_bytes = 0;
    cy::u32 validation_errors = 0;
    /// Texels above the frame's clear value. "The renderer drew nothing" and "the scene was empty"
    /// are different defects and a black picture alone cannot tell them apart.
    cy::u32 lit_texels = 0;
    /// Texels that differ from the PREVIOUS capture by more than one 8-bit step in any channel.
    /// What makes the before-and-after pair a MEASUREMENT rather than two files a reader compares.
    cy::u32 differing_texels = 0;
};

}  // namespace cy::sample::slice
