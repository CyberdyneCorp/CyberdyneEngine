// The heads-up interface, the 2D menu, the sound and the assembled frame. M8.b section 12.

#include "presentation.h"

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/text/linebreak.h>
#include <cy/text/unicode.h>

#include <cmath>
#include <new>
#include <string_view>

namespace cy::sample::slice {
namespace {

using cy::ui::Dirty;
using cy::ui::ElementFlags;
using cy::ui::ElementId;
using cy::ui::kNoElement;
using cy::ui::Rect;

constexpr f32 kBarWidth = 320.0F;
constexpr f32 kBarHeight = 22.0F;

/// The interface's own text measurement, over `cy::text`.
///
/// `cy::ui` deliberately has no font dependency — "a layout module that included a font server
/// could not be tested without one" — so a consumer supplies this. The slice's is the honest
/// minimum: `cy::text::next_grapheme` counts what a reader would call characters (which is not what
/// a byte count answers, and not what a codepoint count answers either), and a fixed advance turns
/// that into a width. A project with a font atlas asks `cy::servers-text` instead; the INTERFACE it
/// implements is this one.
class GraphemeMeasurer final : public cy::ui::ContentMeasurer {
public:
    void set(ElementId element, std::string_view label) noexcept {
        if (count_ < kMaxLabels) {
            labels_[count_] = {element, label};
            ++count_;
        }
    }

    cy::Vec2 measure_content(ElementId element, cy::Vec2 available) noexcept override {
        for (cy::u32 index = 0; index < count_; ++index) {
            if (!(labels_[index].element == element)) {
                continue;
            }
            const std::string_view text = labels_[index].text;
            cy::u32 graphemes = 0;
            cy::usize cursor = 0;
            while (cursor < text.size()) {
                cursor = cy::text::next_grapheme(text, cursor);
                ++graphemes;
            }
            const f32 width = static_cast<f32>(graphemes) * 9.0F;
            return cy::Vec2{available.x < 0.0F || width < available.x ? width : available.x, 20.0F};
        }
        return cy::Vec2{0.0F, 0.0F};
    }

private:
    static constexpr cy::u32 kMaxLabels = 16;
    struct Label {
        ElementId element;
        std::string_view text;
    };
    Label labels_[kMaxLabels]{};
    cy::u32 count_ = 0;
};

GraphemeMeasurer& measurer() noexcept {
    static GraphemeMeasurer instance;
    return instance;
}

[[nodiscard]] cy::render::LightDescription point_light(Vec3 at, u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Point;
    light.transform = Transform::from_translation(at);
    light.intensity = 4000.0F;
    light.range = 30.0F;
    light.stable_id = id;
    return light;
}

[[nodiscard]] cy::render::LightDescription sun(u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Directional;
    light.transform = Transform::identity();
    light.intensity = 90000.0F;
    light.stable_id = id;
    return light;
}

/// Where one visible instance's surfaces come from: the mesh and material the snapshot published
/// for its spatial slot.
///
/// The returned span is borrowed only until the next call, which is exactly what `SurfaceQueryFn`
/// promises — `build_draw_list` copies out of it before asking for the next instance's — so one
/// thread-local record is the whole of the storage this needs.
Span<const cy::rendering::DrawSurface> surface_of_instance(
    const cy::rendering::VisibleInstance& instance, void* user) {
    static thread_local cy::rendering::DrawSurface surface;
    const auto& index = *static_cast<const cy::rendering::assembly::SceneIndex*>(user);
    const cy::rendering::assembly::SceneIndex::Surface published = index.surface_of(instance.slot);
    surface = cy::rendering::DrawSurface{};
    surface.mesh = published.mesh.index();
    surface.material = published.material.index();
    surface.pipeline = 0;
    surface.blend = cy::render::BlendMode::Opaque;
    return {&surface, 1};
}

}  // namespace

Presentation::Presentation(Allocator& allocator) noexcept
    : allocator_(&allocator),
      store_(allocator),
      interaction_(allocator),
      buffer_(allocator),
      nodes_(allocator),
      findings_(allocator),
      menu_draws_(allocator),
      menu_layers_(allocator),
      menu_order_(allocator),
      menu_instances_(allocator),
      menu_tints_(allocator),
      menu_batches_(allocator),
      sources_(allocator),
      assignments_(allocator),
      layers_(allocator),
      lights_(allocator),
      drawn_(allocator) {}

Presentation::~Presentation() {
    delete graph_;
    delete index_;
    delete assembly_;
}

Status Presentation::build(const Options& options, Report& report) noexcept {
    if (Status made = build_interface(); !made) {
        return made;
    }
    if (Status made = build_menu(); !made) {
        return made;
    }
    if (Status made = build_audio(options.agents); !made) {
        return made;
    }
    report.ui_elements = static_cast<u32>(store_.size());
    report.audio_sources = static_cast<u32>(sources_.size());
    if (!options.render) {
        return cy::ok();
    }
    return build_frame();
}

// --- The interface -----------------------------------------------------------------------------

namespace {

/// One element, with the fields a heads-up display sets on every one of them.
[[nodiscard]] Expected<ElementId, Error> panel(cy::ui::ElementStore& store, ElementId parent,
                                               const char* type, cy::Vec2 preferred,
                                               cy::u32 background) noexcept {
    Expected<ElementId, Error> created = store.create(parent, Name::intern(type));
    if (!created) {
        return created;
    }
    cy::ui::LayoutInput* input = store.layout_input(*created);
    input->preferred = preferred;
    input->margin = cy::ui::Insets{6.0F, 6.0F, 6.0F, 6.0F};
    cy::ui::PaintData* paint = store.paint(*created);
    paint->background = background;
    paint->tint = 0xFFFFFFFFU;
    paint->corner_radius = 3.0F;
    if (Status flagged = store.set_flags(*created, ElementFlags::Visible); !flagged) {
        return cy::make_unexpected(flagged.error());
    }
    return created;
}

/// Place one element by anchor: a fraction of the parent, an offset in reference units, and a
/// size. Both anchors are the same fraction, which is what pins a corner rather than stretching an
/// edge — see `arrange_absolute`'s "a degenerate anchor pair means use the desired size".
void anchor(cy::ui::ElementStore& store, ElementId element, cy::Vec2 at, cy::Vec2 offset,
            cy::Vec2 size) noexcept {
    cy::ui::LayoutInput* input = store.layout_input(element);
    input->anchor_min = at;
    input->anchor_max = at;
    input->offset_min = offset;
    input->offset_max = cy::Vec2{offset.x + size.x, offset.y + size.y};
    input->margin = cy::ui::Insets{};
}

}  // namespace

Status Presentation::build_interface() noexcept {
    // THE ROOT PLACES ITS CHILDREN BY ANCHOR, NOT BY FLOW. A heads-up display is three things in
    // three corners of the screen and a reticle in the middle of it, and anchors say that in the
    // terms `ui-system` gives for it — fractions of the parent, so the arrangement survives a
    // resize and an aspect change. Stacking them in a column instead put the bars wherever the
    // flow left room and stretched each to the full width of the frame.
    Expected<ElementId, Error> root =
        panel(store_, kNoElement, "hud", cy::Vec2{1920.0F, 1080.0F}, 0x00000000U);
    if (!root) {
        return Status{cy::make_unexpected(root.error())};
    }
    store_.layout_input(*root)->model = cy::ui::LayoutModel::Absolute;
    if (Status mode = store_.set_hit_test_mode(*root, cy::ui::HitTestMode::Pass); !mode) {
        return mode;
    }

    Expected<ElementId, Error> bars =
        panel(store_, *root, "vitals", cy::Vec2{kBarWidth + 24.0F, 90.0F}, 0xC0101418U);
    if (!bars) {
        return Status{cy::make_unexpected(bars.error())};
    }
    store_.layout_input(*bars)->direction = cy::ui::FlexDirection::Column;
    store_.layout_input(*bars)->align = cy::ui::Align::Start;
    store_.layout_input(*bars)->padding = cy::ui::Insets{10.0F, 10.0F, 10.0F, 10.0F};
    store_.layout_input(*bars)->gap = 8.0F;
    anchor(store_, *bars, cy::Vec2{0.0F, 1.0F}, cy::Vec2{40.0F, -130.0F},
           cy::Vec2{kBarWidth + 24.0F, 90.0F});

    Expected<ElementId, Error> health =
        panel(store_, *bars, "bar", cy::Vec2{kBarWidth, kBarHeight}, 0xFF2E7D32U);
    Expected<ElementId, Error> energy =
        panel(store_, *bars, "bar", cy::Vec2{kBarWidth, kBarHeight}, 0xFF1565C0U);
    Expected<ElementId, Error> reticle =
        panel(store_, *root, "reticle", cy::Vec2{18.0F, 18.0F}, 0xB0FFFFFFU);
    Expected<ElementId, Error> fire =
        panel(store_, *root, "button", cy::Vec2{180.0F, 48.0F}, 0xFF101010U);
    if (!health || !energy || !reticle || !fire) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the interface could not be built");
    }
    // The two bars keep their own width: the health bar's width IS the health, and an element
    // stretched to its parent's width could not say so. A MINIMUM keeps an empty bar a bar — at
    // zero width `flatten` culls it against the viewport and the interface silently loses an
    // element, which this artefact's own driver caught by counting primitives against elements.
    for (const ElementId bar : {*health, *energy}) {
        store_.layout_input(bar)->self_align = cy::ui::Align::Start;
        store_.layout_input(bar)->minimum = cy::Vec2{6.0F, kBarHeight};
    }
    anchor(store_, *reticle, cy::Vec2{0.5F, 0.5F}, cy::Vec2{-9.0F, -9.0F}, cy::Vec2{18.0F, 18.0F});
    anchor(store_, *fire, cy::Vec2{1.0F, 1.0F}, cy::Vec2{-220.0F, -100.0F},
           cy::Vec2{180.0F, 48.0F});
    health_bar_ = *health;
    energy_bar_ = *energy;
    reticle_ = *reticle;
    fire_button_ = *fire;

    // The one interactive element, and everything the accessibility audit asks of one: focusable,
    // labelled, big enough to hit, and legible against its own background.
    if (Status flagged =
            store_.set_flags(fire_button_, ElementFlags::Visible | ElementFlags::Focusable);
        !flagged) {
        return flagged;
    }
    store_.paint(fire_button_)->tint = 0xFFFFFFFFU;
    measurer().set(fire_button_, std::string_view("Volley"));
    measurer().set(health_bar_, std::string_view("Health"));

    cy::ui::AccessibilityNode node;
    node.element = fire_button_;
    node.role = cy::ui::Role::Button;
    node.label = Name::intern("Volley");
    node.value = Name::intern("ready");
    node.focusable = true;
    if (Status pushed = nodes_.push_back(node); !pushed) {
        return pushed;
    }
    cy::ui::AccessibilityNode meter;
    meter.element = health_bar_;
    meter.role = cy::ui::Role::ProgressBar;
    meter.label = Name::intern("Health");
    meter.value = Name::intern("100%");
    if (Status pushed = nodes_.push_back(meter); !pushed) {
        return pushed;
    }

    cy::ui::Layer layer;
    layer.name = Name::intern("hud");
    layer.kind = cy::ui::LayerKind::Hud;
    layer.behaviour = cy::ui::default_behaviour(cy::ui::LayerKind::Hud);
    layer.root = *root;
    if (Status pushed = interaction_.push_layer(layer); !pushed) {
        return pushed;
    }
    return interaction_.set_focus(store_, fire_button_);
}

Status Presentation::update_interface(const HudState& state, Report& report,
                                      f64& microseconds) noexcept {
    const auto started = std::chrono::steady_clock::now();

    cy::ui::LayoutInput* health = store_.layout_input(health_bar_);
    cy::ui::LayoutInput* energy = store_.layout_input(energy_bar_);
    if (health == nullptr || energy == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "the interface lost an element");
    }
    health->preferred.x = kBarWidth * (state.health < 0.0F ? 0.0F : state.health);
    energy->preferred.x = kBarWidth * (state.energy < 0.0F ? 0.0F : state.energy);
    store_.mark(health_bar_, Dirty::Measure | Dirty::Paint);
    store_.mark(energy_bar_, Dirty::Measure | Dirty::Paint);
    store_.paint(reticle_)->background = state.contacts > 0U ? 0xFFE53935U : 0xB0FFFFFFU;
    store_.mark(reticle_, Dirty::Paint);

    // LAYOUT HAPPENS IN REFERENCE UNITS, NOT IN PIXELS, and the flatten viewport has to be the
    // same space or the pass culls what layout placed. `cy::ui::layout` divides the viewport by the
    // resolved scale and arranges inside THAT — a 1600x900 window against a 1920x1080 reference
    // lays out at 1920x1080 and the consumer multiplies by 0.833 when it draws. Handing this
    // function the pixel rectangle instead cost the artefact four of its six elements, silently:
    // they were placed below y=900 in reference units, `flatten` intersected them with a
    // 900-tall viewport, and they came back as `culled` rather than as an error.
    cy::ui::ScaleSettings scale;
    scale.reference = cy::Vec2{1920.0F, 1080.0F};
    const cy::Vec2 viewport{static_cast<f32>(width_), static_cast<f32>(height_)};
    scale_ = cy::ui::resolve_scale(scale, viewport);
    const cy::Vec2 reference_space{scale_ > 0.0F ? viewport.x / scale_ : viewport.x,
                                   scale_ > 0.0F ? viewport.y / scale_ : viewport.y};
    cy::ui::LayoutReport laid;
    if (Status done = cy::ui::layout(store_, scale, viewport, &measurer(), laid); !done) {
        return done;
    }

    cy::ui::FlattenReport flattened;
    if (Status done = cy::ui::flatten(
            store_, Rect{0.0F, 0.0F, reference_space.x, reference_space.y}, buffer_, flattened);
        !done) {
        return done;
    }
    report.ui_primitives = flattened.emitted + flattened.reused;
    report.ui_batches = flattened.batches;

    const f64 spent =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    cy::ui::UiBudget budget;
    cy::ui::BudgetReport spend;
    (void)cy::ui::apply_budget(budget, flattened, static_cast<f32>(spent), 1U, spend);

    cy::ui::AccessibilityReport audited;
    if (Status done =
            cy::ui::audit_accessibility(store_, nodes_.span(), interaction_, findings_, audited);
        !done) {
        return done;
    }
    report.ui_accessible = audited.passed;
    report.ui_findings = static_cast<u32>(findings_.size());

    microseconds =
        std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - started).count();
    return cy::ok();
}

Span<const cy::ui::Primitive> Presentation::primitives() const noexcept {
    return buffer_.primitives();
}

// --- The 2D menu --------------------------------------------------------------------------------

Status Presentation::build_menu() noexcept {
    cy::rendering2d::Layer2D layer;
    layer.index = 0;
    layer.y_sorted = true;
    if (Status pushed = menu_layers_.push_back(layer); !pushed) {
        return pushed;
    }

    // A panel and four entries, in the order a menu is read. The tints are the sprite's own, which
    // is what the picture draws.
    // Left of centre, deliberately: a menu drawn over the middle of the frame hides the game
    // behind it, and the artefact's picture has to show both.
    const f32 rows[5][4] = {{96.0F, 210.0F, 430.0F, 400.0F},
                            {128.0F, 262.0F, 366.0F, 54.0F},
                            {128.0F, 332.0F, 366.0F, 54.0F},
                            {128.0F, 402.0F, 366.0F, 54.0F},
                            {128.0F, 472.0F, 366.0F, 54.0F}};
    const cy::u32 tints[5] = {0xE0141A20U, 0xFF2E7D32U, 0xFF1565C0U, 0xFF6A4C93U, 0xFF8D3B3BU};
    for (cy::u32 index = 0; index < 5U; ++index) {
        cy::rendering2d::Draw2D draw;
        draw.kind = cy::rendering2d::PrimitiveKind::Sprite;
        draw.sort.layer = 0;
        draw.sort.y_sort = rows[index][1];
        draw.sort.tiebreak = index + 1U;
        draw.material = index == 0U ? cy::u16{0} : cy::u16{1};
        draw.texture_set = 0;
        draw.destination =
            cy::rendering2d::Rect2D{rows[index][0], rows[index][1], rows[index][2], rows[index][3]};
        draw.source = cy::rendering2d::Rect2D{0.0F, 0.0F, 1.0F, 1.0F};
        draw.tint = tints[index];
        if (Status pushed = menu_draws_.push_back(draw); !pushed) {
            return pushed;
        }
        if (Status pushed = menu_tints_.push_back(tints[index]); !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

Status Presentation::update_menu(bool open, Report& report) noexcept {
    if (!open) {
        menu_instances_.clear();
        return cy::ok();
    }
    cy::rendering2d::BatchReport batched;
    if (Status built =
            cy::rendering2d::build_batches(menu_draws_.span(), menu_layers_.span(), menu_order_,
                                           menu_instances_, menu_batches_, batched);
        !built) {
        return built;
    }
    // THE HIGH-WATER MARK, NOT THE LAST TICK'S. A menu is open some ticks and shut on others, and
    // a run whose last tick happened to have it shut would report that the menu draws nothing —
    // which is what this artefact's own driver caught the first time it ran.
    const auto drawn_now = static_cast<u32>(menu_draws_.size());
    const auto batched_now = static_cast<u32>(menu_batches_.size());
    report.menu_draws = drawn_now > report.menu_draws ? drawn_now : report.menu_draws;
    report.menu_batches = batched_now > report.menu_batches ? batched_now : report.menu_batches;
    return cy::ok();
}

// --- The sound
// ------------------------------------------------------------------------------------

Status Presentation::build_audio(u32 characters) noexcept {
    // One footstep source per character, capped, plus the level's ambience. `audio`'s importance
    // tiers are what make that affordable, and the whole point is that the COUNT is the content's
    // and the COST is the configuration's.
    const u32 count = (characters < 512U ? characters : 512U) + 8U;
    for (u32 index = 0; index < count; ++index) {
        cy::audio::SourceScoring source;
        source.source = index + 1U;
        const f32 angle = static_cast<f32>(index) * 0.37F;
        const f32 ring = 2.0F + (static_cast<f32>(index % 40U) * 1.1F);
        source.position = Vec3{ring * std::cos(angle), 0.0F, ring * std::sin(angle)};
        source.volume = 0.7F;
        source.priority = index < 8U ? 4.0F : 1.0F;
        source.gameplay_important = index < 4U;
        if (Status pushed = sources_.push_back(source); !pushed) {
            return pushed;
        }
    }

    music_.sample_rate = 48000;
    music_.tempo_bpm = 120.0F;
    music_.beats_per_bar = 4;
    music_.length_samples = 48000ULL * 16ULL;
    // The two layers respond to one intensity parameter in opposite directions, which is what
    // `MusicLayer`'s silent/full pair expresses: combat rises with it, exploration falls away.
    cy::audio::MusicLayer combat;
    combat.silent_at = 0.3F;
    combat.full_at = 0.9F;
    combat.fade_seconds = 1.0F;
    if (Status pushed = layers_.push_back(combat); !pushed) {
        return pushed;
    }
    cy::audio::MusicLayer explore;
    explore.silent_at = 0.9F;
    explore.full_at = 0.3F;
    explore.fade_seconds = 1.0F;
    return layers_.push_back(explore);
}

Status Presentation::update_audio(Vec3 listener, u32 tick, Report& report) noexcept {
    cy::audio::ScoringListener ears;
    ears.position = listener;
    cy::audio::TierBudgets budgets;
    budgets.full_acoustic = 8;
    budgets.spatialised = 48;
    budgets.simple = 128;
    cy::audio::TierReport tiers;
    if (Status assigned = cy::audio::assign_tiers(sources_.span(), ears, budgets, 1.0F / 60.0F,
                                                  assignments_, tiers);
        !assigned) {
        return assigned;
    }
    report.audio_full_acoustic =
        tiers.counts[static_cast<cy::usize>(cy::audio::SimulationTier::FullAcoustic)];
    report.audio_virtual = tiers.counts[static_cast<cy::usize>(cy::audio::SimulationTier::Virtual)];

    // The music follows the fight: one transition scheduled on a bar boundary when the intensity
    // crosses the layer's threshold, which is what "interactive audio" means here.
    const f32 intensity = (tick % 240U) < 120U ? 0.8F : 0.2F;
    cy::audio::update_layers(layers_.span(), intensity, 1.0F / 60.0F);
    playhead_ += 800U;
    if ((tick % 120U) == 0U) {
        cy::audio::MusicTransition transition;
        transition.point = cy::audio::TransitionPoint::NextBar;
        if (Status scheduled = cy::audio::schedule_transition(music_, playhead_, transition);
            scheduled) {
            transition_ = transition;
            ++report.music_transitions;
        }
    }
    return cy::ok();
}

// --- The frame
// ---------------------------------------------------------------------------------------

Status Presentation::build_frame() noexcept {
    assembly_ = new (std::nothrow) cy::rendering::assembly::FrameAssembly(*allocator_);
    index_ = new (std::nothrow) cy::rendering::assembly::SceneIndex(*allocator_);
    graph_ = new (std::nothrow) cy::rendering::RenderGraph(*allocator_);
    if (assembly_ == nullptr || index_ == nullptr || graph_ == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the frame's workspaces did not allocate");
    }

    cy::rendering::assembly::AssemblyDescription description;
    description.width = width_;
    description.height = height_;
    description.near_plane = 0.1F;
    description.far_plane = 400.0F;
    description.clusters = cy::rendering::ClusterGridConfig{32, 16, 32};
    description.post.ambient_occlusion = true;
    description.post.temporal_antialiasing = true;
    description.post.bloom = true;
    description.post.colour_grading = true;
    description.shadows.slots = 64;
    description.material_capacity = 64;
    description.max_draws = 16384;
    description.max_instances = 16384;
    // No device is attached: this sample assembles a frame and records no pass. See the header.
    description.gpu_culling = false;
    if (Status ready = assembly_->initialize(description); !ready) {
        return ready;
    }

    if (Status pushed = lights_.push_back(sun(1)); !pushed) {
        return pushed;
    }
    for (u32 index = 0; index < 4U; ++index) {
        const f32 angle = static_cast<f32>(index) * 1.5707963F;
        if (Status pushed = lights_.push_back(point_light(
                Vec3{14.0F * std::cos(angle), 5.0F, 14.0F * std::sin(angle)}, 2ULL + index));
            !pushed) {
            return pushed;
        }
    }
    frame_ready_ = true;
    return cy::ok();
}

Status Presentation::update_frame(const cy::render::RenderSnapshot& snapshot, const ViewState& view,
                                  Report& report, f64& microseconds) noexcept {
    if (!frame_ready_) {
        return cy::ok();
    }
    const auto started = std::chrono::steady_clock::now();

    cy::rendering::assembly::SceneIndexReport applied;
    if (Status stepped = index_->apply(snapshot, 1.0F, applied); !stepped) {
        return stepped;
    }

    const f32 aspect = static_cast<f32>(width_) / static_cast<f32>(height_);
    const f32 fov = 1.0471975512F;
    projection_ = cy::perspective_reversed_z(fov, aspect, 0.1F, 400.0F);
    view_matrix_ = cy::look_at(view.eye, view.target, Vec3{0.0F, 1.0F, 0.0F});

    cy::rendering::assembly::AssemblyView assembly_view;
    assembly_view.fov_y_radians = fov;
    assembly_view.projection = projection_;
    assembly_view.view = view_matrix_;
    assembly_view.cull.frustum = cy::Frustum::from_view_projection(projection_ * view_matrix_);
    assembly_view.cull.camera_position = view.eye;
    const Vec3 forward{view.target.x - view.eye.x, view.target.y - view.eye.y,
                       view.target.z - view.eye.z};
    const f32 length =
        std::sqrt((forward.x * forward.x) + (forward.y * forward.y) + (forward.z * forward.z));
    assembly_view.cull.camera_forward =
        length > 1e-4F ? Vec3{forward.x / length, forward.y / length, forward.z / length}
                       : Vec3{0.0F, 0.0F, -1.0F};
    assembly_view.cull.fov_y_radians = fov;
    assembly_view.lights = lights_.span();
    assembly_view.sun_direction = view.sun;

    graph_->reset();
    // THE SURFACE QUERY IS WHY THE FRAME SHADES WITH WHAT WAS AUTHORED. Left null, the assembly
    // falls back to one opaque surface per instance whose material index is the instance's SPATIAL
    // SLOT — enough to sort a frame, not enough to shade one, and past the end of a 64-slot table
    // the moment there are more than 64 instances. This one answers from `SceneIndex::surface_of`,
    // which is the mesh and material handle the snapshot published, which is what
    // `bind_render_assets` resolved the authored reference to. Every link of task 11.3's chain is
    // in that sentence.
    cy::rendering::assembly::FrameSinks sinks;
    sinks.surfaces = &surface_of_instance;
    sinks.surfaces_user = index_;
    cy::rendering::assembly::AssemblyReport assembled;
    if (Status made =
            assembly_->assemble(index_->index(), assembly_view, sinks, *graph_, assembled);
        !made) {
        return made;
    }

    report.frame_assembled = true;
    report.frame_draws = assembled.draws;
    report.frame_visible = assembled.cull.visible;
    report.frame_lights = assembled.lights;
    report.frame_passes = assembled.passes_declared;
    report.frame_material_slots = assembled.material_slots;
    report.frame_draws_without_material = assembled.draws_without_material;
    report.frame_clusters = assembled.clusters.clusters;
    report.frame_shadow_pages = assembled.shadow_pages_requested;

    drawn_.clear();
    // AND THE MESHES THE FRAME ACTUALLY NAMED, counted here rather than trusted. "Every reference
    // resolved to a handle" is satisfied by a resolver that answers the same handle to everything,
    // which is exactly M8.a's defect one link further along, so the artefact counts the DISTINCT
    // handles and slice.py requires as many as the level interned.
    cy::Array<cy::u64> meshes_seen(*allocator_);
    for (const cy::render::DrawItem& item : assembly_->layer(cy::render::SortLayer::Opaque)) {
        if (Status pushed = drawn_.push_back(item.stable_id); !pushed) {
            return pushed;
        }
        const cy::u32 slot = index_->slot_of(item.stable_id);
        if (slot == cy::rendering::assembly::SceneIndex::kNoSlot) {
            continue;
        }
        const cy::u64 mesh = index_->surface_of(slot).mesh.bits();
        bool known = false;
        for (const cy::u64 seen : meshes_seen) {
            known = known || seen == mesh;
        }
        if (!known) {
            if (Status pushed = meshes_seen.push_back(mesh); !pushed) {
                return pushed;
            }
        }
    }
    report.frame_distinct_meshes = static_cast<cy::u32>(meshes_seen.size());

    microseconds =
        std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - started).count();
    return cy::ok();
}

}  // namespace cy::sample::slice
