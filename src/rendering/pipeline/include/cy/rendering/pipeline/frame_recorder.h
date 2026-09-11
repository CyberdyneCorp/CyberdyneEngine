#pragma once
// THE RECORD CALLBACKS `FrameSinks` HAS BEEN WAITING FOR. M8.c tasks 1b.1 and 1b.2.
//
// ================================================================================================
// THIS IS THE FILE THE MILESTONE IS ABOUT
// ================================================================================================
//
// `FrameSinks::passes` is a callback per stage and `ForwardFrame` is explicit that a stage with
// none "is declared and records nothing, which is a legitimate frame and what a structural test
// wants". Every caller in the tree wanted the structural frame, so the engine had a frame nothing
// drew into. `FrameRecorder::sinks()` returns that struct with real callbacks in it.
//
// What each one does, and why it is the shape it is:
//
//   Prepare        Copies the ring's lights and draw records into the frame's OWN buffers.
//                  `ForwardFrame` calls that pass "the only pass that writes them" and declares
//                  `Access::TransferWrite` on both; a caller that recorded nothing left the
//                  transfer barrier around a transfer that never happened.
//   DepthPrepass   The opaque draws, position stream only, depth written and compared
//                  GreaterOrEqual. `render::kDepthPassStreams` made structural.
//   Opaque         The same draws, three streams, depth compared EQUAL and not written, shaded
//                  against the cluster's light list and the GPU material table.
//   Transparent    The transparent layer, back to front as the sort produced it, alpha blended,
//                  depth tested and not written.
//   PostProcess    `cy/fullscreen.slang`'s own resolve: exposure, tonemap, straight into the
//                  frame's output. That is the pass that makes the frame's colour visible, and it
//                  is the standard library's own entry point rather than a copy of it.
//
// ================================================================================================
// WHAT IT DOES NOT OWN, AND THE SEAM EACH ABSENCE LEAVES
// ================================================================================================
//
// **Geometry.** `GeometrySource` below is what a caller fills: three stream buffers, an index
// buffer and a per-draw lookup. The mesh table is the render server's, and this module holds no
// copy of it for the same reason `FrameAssembly` holds none.
//
// **The sky, the screen-space passes and the interface.** Those stages are declared by the frame
// and left to whoever owns them. `PassExtension` is how a consumer attaches to one — task 1b.4's
// particle renderer is the first, and it reaches the frame through this seam rather than through a
// second frame of its own.
//
// ================================================================================================
// A REFUSAL RATHER THAN A WRONG DRAW
// ================================================================================================
//
// `bind()` fails when the frame's derived prepass mode wants attachments the depth pipeline was not
// created with, or when the frame is multisampled and the pipelines are not. Both would be a
// dynamic-rendering format mismatch — a validation error at draw time, from a pipeline created a
// hundred frames earlier. Creating the missing pipeline here instead is the thing `shader-system`
// forbids outright: "blocking the frame to compile a pipeline state SHALL NOT occur in shipping
// builds".

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/pipeline/frame_bindings.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::pipeline {

// The assembly's own vocabulary, brought in by name rather than by a `using namespace`: these four
// are what this file is about, and anything else from that module would be a dependency this layer
// has not admitted to.
using assembly::AssemblyDescription;
using assembly::AssemblyReport;
using assembly::FrameAssembly;
using assembly::FrameSinks;

/// Where one draw's indices and vertices are. Returned by the caller's lookup.
struct DrawGeometry {
    /// Null for a non-indexed draw, in which case `vertex_count` is what is drawn.
    rhi::BufferHandle indices;
    bool wide_indices = false;
    u32 index_count = 0;
    u32 first_index = 0;
    i32 vertex_offset = 0;
    u32 vertex_count = 0;
};

/// Where a draw's geometry comes from. False means "this draw has nothing to draw", which is a
/// legitimate answer — a mesh still streaming in — and is counted rather than treated as an error.
using DrawGeometryFn = bool (*)(const render::DrawItem& item, const GpuDrawInstance& instance,
                                void* user, DrawGeometry& out) noexcept;

/// The three vertex streams and the lookup. The caller's, entirely.
struct GeometrySource {
    /// Indexed by `kPositionStream`, `kNormalStream`, `kUvStream`. The depth pass binds the first
    /// alone; the forward passes bind all three.
    rhi::BufferHandle streams[3];
    DrawGeometryFn geometry = nullptr;
    void* user = nullptr;

    [[nodiscard]] bool complete() const noexcept {
        return geometry != nullptr && !streams[kPositionStream].is_null() &&
               !streams[kNormalStream].is_null() && !streams[kUvStream].is_null();
    }
};

class FrameRecorder;

/// What an extension's callback is handed: the graph's own context, plus everything the layer had
/// already bound when the extension was reached.
struct ExtensionContext {
    rhi::CommandBuffer* commands = nullptr;
    const GraphExecutor* executor = nullptr;
    const FrameRecorder* recorder = nullptr;
    FramePassKind kind = FramePassKind::Count;
    u32 width = 0;
    u32 height = 0;
    /// True while a rendering scope is open — every stage but `Prepare`. An extension inside one
    /// records draws; an extension outside one records copies and dispatches.
    bool inside_rendering = false;
};

using ExtensionRecordFn = void (*)(const ExtensionContext& context, void* user) noexcept;

/// A consumer attached to one of the frame's stages.
struct PassExtension {
    FramePassKind kind = FramePassKind::Count;
    ExtensionRecordFn record = nullptr;
    void* user = nullptr;
};

inline constexpr u32 kMaxPassExtensions = 8;

/// What one recorded frame did. Read off the recording itself rather than predicted from the draw
/// list: a draw whose geometry lookup answered false is counted in `skipped`, and a stage whose
/// callback ran is counted in `passes`.
struct RecorderReport {
    u32 passes = 0;
    /// The depth prepass's draws. The same instances the opaque pass draws, counted separately
    /// because they are a second pass over the same list and adding them to `opaque_draws` would
    /// make "how many draws did the frame shade" the wrong number.
    u32 prepass_draws = 0;
    u32 opaque_draws = 0;
    u32 transparent_draws = 0;
    /// Draws whose geometry lookup answered false. Counted ONCE PER PASS that asked, so an
    /// instance whose mesh is still streaming in is counted twice in a frame with a prepass — which
    /// is the honest number for "how many draws did a pass have to skip".
    u32 skipped_draws = 0;
    u32 extensions_run = 0;
    /// Bytes copied by the Prepare pass into the frame's own buffers.
    u64 uploaded_bytes = 0;

    [[nodiscard]] u32 draws() const noexcept { return opaque_draws + transparent_draws; }
};

/// The callbacks, and the state they read.
///
/// NOT THREAD-SAFE, and one per view — the same contract `FrameAssembly` states for itself. It
/// holds no allocation: everything it reads belongs to the assembly, the pipelines or the caller.
class FrameRecorder {
public:
    FrameRecorder() noexcept = default;

    FrameRecorder(const FrameRecorder&) = delete;
    FrameRecorder& operator=(const FrameRecorder&) = delete;
    FrameRecorder(FrameRecorder&&) = delete;
    FrameRecorder& operator=(FrameRecorder&&) = delete;

    [[nodiscard]] Status initialize(FramePipelines& pipelines, FrameBindings& bindings) noexcept;

    void set_geometry(const GeometrySource& source) noexcept { geometry_ = source; }
    [[nodiscard]] const GeometrySource& geometry() const noexcept { return geometry_; }

    /// Attach a consumer to a stage. Refuses a stage the layer records itself only for `Prepare` —
    /// everything else composes, because a particle renderer drawing after the transparent draws is
    /// exactly the arrangement `vfx-system` wants.
    [[nodiscard]] Status add_extension(const PassExtension& extension) noexcept;
    void clear_extensions() noexcept { extension_count_ = 0; }
    [[nodiscard]] u32 extension_count() const noexcept { return extension_count_; }

    /// Point the recorder at the frame it is about to record, and check the two ways a pipeline can
    /// disagree with it. Call after `initialize` and before `FrameAssembly::assemble`.
    [[nodiscard]] Status bind(FrameAssembly& assembly) noexcept;

    /// The sinks, with the layer's callbacks in them. `surfaces` is left null: a surface query is
    /// the mesh table's business and `FrameSinks` documents what null gives.
    [[nodiscard]] FrameSinks sinks() noexcept;

    /// Reset the counters. Called by `bind`; exposed so a caller can read a frame's numbers and
    /// then clear them without rebinding.
    void reset_report() noexcept { report_ = RecorderReport{}; }
    [[nodiscard]] const RecorderReport& report() const noexcept { return report_; }

    // --- What the callbacks read. Public because `RecordFn` is a plain function pointer, so the
    // callbacks are free functions and cannot see a private member.

    [[nodiscard]] FrameAssembly* assembly() const noexcept { return assembly_; }
    [[nodiscard]] FramePipelines* pipelines() const noexcept { return pipelines_; }
    [[nodiscard]] FrameBindings* bindings() const noexcept { return bindings_; }
    [[nodiscard]] RecorderReport& mutable_report() noexcept { return report_; }
    [[nodiscard]] Span<const PassExtension> extensions() const noexcept {
        return {extensions_, extension_count_};
    }

private:
    FrameAssembly* assembly_ = nullptr;
    FramePipelines* pipelines_ = nullptr;
    FrameBindings* bindings_ = nullptr;
    GeometrySource geometry_;
    PassExtension extensions_[kMaxPassExtensions];
    u32 extension_count_ = 0;
    RecorderReport report_;
};

/// The `FrameUpload` one assembled frame implies, built from the assembly's own outputs.
///
/// A free function rather than a method so that the one place a caller supplies something the
/// assembly does not have — the instance rows — is an argument it cannot forget to pass.
[[nodiscard]] FrameUpload upload_for(const FrameAssembly& assembly, const AssemblyReport& report,
                                     const Mat4& relative_to_clip, const Mat4& relative_to_view,
                                     Span<const InstanceTransform> instances,
                                     const GlobalsData& globals,
                                     const u32 material_offsets[4]) noexcept;

}  // namespace cy::rendering::pipeline
