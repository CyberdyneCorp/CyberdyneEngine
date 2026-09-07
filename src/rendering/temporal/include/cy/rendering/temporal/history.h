#pragma once
// History resources: declared by consumers, owned by the framework. Task 8.3.
//
// `temporal-rendering` — "History resources": a consumer declares format, resolution scale and how
// many frames it keeps, and the framework owns "their allocation, double buffering, resizing, and
// release".
//
// ================================================================================================
// WHY THE FRAMEWORK OWNS THEM AND NOT THE CONSUMER
// ================================================================================================
//
// The scenario is the argument: "WHEN output resolution changes THEN every history resource SHALL
// be reallocated and marked invalid by the framework, and no consumer SHALL need to detect the
// change itself." A pass that allocated its own history has to notice the resize, and the one that
// forgets reads last frame's buffer at last frame's dimensions — which on some drivers is a garbage
// frame and on others is silence.
//
// A HISTORY CARRIES ITS PROVENANCE. "A history resource SHALL carry the view parameters it was
// produced with, so a consumer can detect staleness rather than assuming validity." That is
// `HistoryResource::produced_with`, and it is what lets a consumer be right even when something
// this framework does not know about has changed.
//
// NO DEVICE HERE. This module records what each history IS and what it COSTS; the allocation of the
// texture is the render graph's, which is layer 4 as well but is a different module and holds the
// device. `history_bytes()` is therefore an accounting figure, and it is the one the diagnostic
// requirement asks for ("history memory in use per consumer").

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {

/// The formats a history buffer is worth having. Deliberately short: this is a list of what
/// temporal accumulation actually stores, not a re-export of the RHI's format enumeration, which
/// this module may not name from where it sits in the dependency graph.
enum class HistoryFormat : u8 {
    /// 16-bit float RGBA. Colour history.
    Rgba16F = 0,
    /// 16-bit float RG. Moments, or a two-channel signal.
    Rg16F,
    /// 16-bit float, one channel. A visibility term.
    R16F,
    /// 8-bit RGBA. Display-referred history only.
    Rgba8,
    /// 32-bit float, one channel. Depth history.
    R32F,
    Count,
};

[[nodiscard]] const char* history_format_name(HistoryFormat format) noexcept;

[[nodiscard]] u32 history_bytes_per_texel(HistoryFormat format) noexcept;

/// The view parameters a history was produced with. Enough to answer "is this history describing
/// the same picture I am about to accumulate into", and no more — a full camera would make this
/// structure a second source of truth for the view.
struct HistoryProvenance {
    u32 width = 0;
    u32 height = 0;
    /// The framework's frame counter when the history was last written.
    u64 frame = 0;
    /// A hash of the projection. Any change to the projection changes it; the framework computes
    /// it, so two consumers cannot disagree about whether the projection changed.
    u64 projection_key = 0;
};

/// What a consumer asks for.
struct HistoryDeclaration {
    HistoryFormat format = HistoryFormat::Rgba16F;
    /// Fraction of the render resolution. 0.5 for a half-resolution effect.
    f32 resolution_scale = 1.0F;
    /// How many frames are retained. Two is double buffering; more is an accumulation depth.
    u32 frames = 2;
};

/// A handle. Opaque, and only the framework mints one.
struct HistoryId {
    u32 value = 0xFFFFFFFFU;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0xFFFFFFFFU; }
};

struct HistoryResource {
    HistoryId id;
    /// Which consumer declared it. Memory is reported per consumer.
    u32 consumer = 0xFFFFFFFFU;
    HistoryDeclaration declaration;
    u32 width = 0;
    u32 height = 0;
    /// False after an invalidation event, and after a resize, until the consumer writes it again.
    /// A consumer reading a resource with this false reconstructs spatially for that frame.
    bool valid = false;
    HistoryProvenance produced_with;

    [[nodiscard]] u64 bytes() const noexcept;
};

}  // namespace cy::rendering
