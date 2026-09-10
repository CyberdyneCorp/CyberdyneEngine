#pragma once
// The presentation half: the heads-up interface, the 2D menu, the sound, and the assembled frame.
// M8.b section 12.
//
// It is a separate object from `Slice` for the reason M8.a's artefact split a driver from a
// program: the simulation decides what is true and the presentation decides what is shown, and a
// milestone whose exit criteria include "the interface holds its frame budget" wants the second
// measurable without the first.
//
// WHAT IT DOES NOT DO. It records no pass. `FrameAssembly` hands a pass's record callback to its
// CALLER — "it does not own the shaders or the pipelines" — and this sample supplies none: what it
// asserts is that the frame is ASSEMBLED, that every draw resolves to a material slot the table
// holds, and that the whole chain from an authored `MeshRenderer` to a sorted draw runs.
// Rasterising it would mean a second renderer beside `samples/03-first-light`'s. README.md says so
// where a reader of the picture will look for it.

#include <cy/audio/interactive.h>
#include <cy/audio/tiers.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/2d/sprite.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/assembly/scene_index.h>
#include <cy/servers/render/snapshot.h>
#include <cy/ui/interaction.h>
#include <cy/ui/layout.h>
#include <cy/ui/paint.h>
#include <cy/ui/store.h>

#include "slice.h"

namespace cy::sample::slice {

/// What the interface is told about the game this tick. Numbers, not widgets: the interface reads
/// them through `cy::ui`'s own binding sources.
struct HudState {
    f32 health = 1.0F;
    f32 energy = 1.0F;
    u32 squad_alive = 0;
    u32 contacts = 0;
    u32 cooldown_ticks = 0;
    bool menu_open = false;
};

/// What the frame is told about the view this tick.
struct ViewState {
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 target{0.0F, 0.0F, 0.0F};
    f32 focal_length = 35.0F;
    Vec3 sun{0.3F, 0.8F, 0.4F};
};

class Presentation {
public:
    explicit Presentation(Allocator& allocator) noexcept;
    ~Presentation();

    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    /// Build the interface tree, the menu's draw set, the sound bed and the frame's workspaces.
    [[nodiscard]] Status build(const Options& options, Report& report) noexcept;

    /// One frame of interface: measure, arrange, flatten, budget, audit.
    [[nodiscard]] Status update_interface(const HudState& state, Report& report,
                                          f64& microseconds) noexcept;

    /// One frame of sound: score every source, assign the tiers, advance the music.
    [[nodiscard]] Status update_audio(Vec3 listener, u32 tick, Report& report) noexcept;

    /// One frame of the 2D menu. Only while it is open, which is what makes it a menu.
    [[nodiscard]] Status update_menu(bool open, Report& report) noexcept;

    /// One assembled frame over the snapshot the extract stage published.
    [[nodiscard]] Status update_frame(const cy::render::RenderSnapshot& snapshot,
                                      const ViewState& view, Report& report,
                                      f64& microseconds) noexcept;

    /// The matrices the last assembled frame used, and the instances it drew.
    [[nodiscard]] const cy::Mat4& view_matrix() const noexcept { return view_matrix_; }
    [[nodiscard]] const cy::Mat4& projection() const noexcept { return projection_; }
    [[nodiscard]] u32 viewport_width() const noexcept { return width_; }
    [[nodiscard]] u32 viewport_height() const noexcept { return height_; }
    [[nodiscard]] Span<const u64> drawn() const noexcept { return drawn_.span(); }
    [[nodiscard]] const cy::rendering::assembly::SceneIndex& scene_index() const noexcept {
        return *index_;
    }

    /// The interface's own flattened primitives and the menu's own batched instances, for the
    /// picture. Both are the modules' outputs rather than a second description of them.
    [[nodiscard]] Span<const cy::ui::Primitive> primitives() const noexcept;
    [[nodiscard]] Span<const cy::rendering2d::Instance2D> menu_instances() const noexcept {
        return menu_instances_.span();
    }
    [[nodiscard]] Span<const cy::u32> menu_tints() const noexcept { return menu_tints_.span(); }
    /// Reference units to pixels. The interface is laid out in reference units and drawn scaled;
    /// the picture has to apply the same factor or it draws a 1920-wide bar on a 1600-wide frame.
    [[nodiscard]] f32 interface_scale() const noexcept { return scale_; }

private:
    [[nodiscard]] Status build_interface() noexcept;
    [[nodiscard]] Status build_menu() noexcept;
    [[nodiscard]] Status build_audio(u32 characters) noexcept;
    [[nodiscard]] Status build_frame() noexcept;

    Allocator* allocator_ = nullptr;
    u32 width_ = 1600;
    u32 height_ = 900;
    f32 scale_ = 1.0F;

    // The interface.
    cy::ui::ElementStore store_;
    cy::ui::Interaction interaction_;
    cy::ui::PrimitiveBuffer buffer_;
    Array<cy::ui::AccessibilityNode> nodes_;
    Array<cy::ui::AccessibilityFinding> findings_;
    cy::ui::ElementId health_bar_;
    cy::ui::ElementId energy_bar_;
    cy::ui::ElementId reticle_;
    cy::ui::ElementId fire_button_;

    // The menu.
    Array<cy::rendering2d::Draw2D> menu_draws_;
    Array<cy::rendering2d::Layer2D> menu_layers_;
    Array<cy::u32> menu_order_;
    Array<cy::rendering2d::Instance2D> menu_instances_;
    Array<cy::u32> menu_tints_;
    Array<cy::rendering2d::Batch2D> menu_batches_;

    // The sound.
    Array<cy::audio::SourceScoring> sources_;
    Array<cy::audio::TierAssignment> assignments_;
    cy::audio::MusicalClip music_;
    cy::audio::MusicTransition transition_;
    Array<cy::audio::MusicLayer> layers_;
    cy::u64 playhead_ = 0;

    // The frame.
    cy::rendering::assembly::FrameAssembly* assembly_ = nullptr;
    cy::rendering::assembly::SceneIndex* index_ = nullptr;
    cy::rendering::RenderGraph* graph_ = nullptr;
    Array<cy::render::LightDescription> lights_;
    Array<u64> drawn_;
    cy::Mat4 view_matrix_ = cy::Mat4::identity();
    cy::Mat4 projection_ = cy::Mat4::identity();
    bool frame_ready_ = false;
};

}  // namespace cy::sample::slice
