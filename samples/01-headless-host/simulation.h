// The world and the stage that runs over it.
//
// Two things are demonstrated here and nothing else:
//
//   THE CONTROL-PLANE RULE. A field is resolved to a TypedAccessor ONCE, at setup, through a
//   reflected lookup by FieldId (`Bindings::resolve`). Everything per entity thereafter is an
//   offset. `core-type-system` — "field iteration, offset arithmetic, and dynamic dispatch SHALL
//   NOT appear in per-entity hot paths" — and each system body declares CY_REFLECT_HOT_REGION, so
//   a lookup that crept back in would be counted rather than merely disapproved of.
//
//   DERIVED PARALLELISM. The three systems declare what they touch; nobody declares which may run
//   together. `SystemSchedule` derives that: `decay` and `drift` write different components and
//   land in one batch, `summarise` reads both and is ordered after them. Changing a declaration
//   changes the plan, and no scheduling code has to be edited to keep up.

#ifndef CY_SAMPLE_HEADLESS_HOST_SIMULATION_H
#define CY_SAMPLE_HEADLESS_HOST_SIMULATION_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/schedule.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/demo/types.h>
#include <cy/core/reflect/registry.h>
#include <cy/core/reflect/type_info.h>

namespace sample {

/// The component data, structure-of-arrays, the way the ECS will hold it at M2.
///
/// The allocator is taken rather than assumed, because the domain an allocation is attributed to is
/// the whole point of the budget report: these arrays are `MemoryDomain::Ecs` and the report says
/// so without anyone having to tag them a second time.
struct World {
    explicit World(cy::Allocator& allocator) noexcept : health(allocator), placement(allocator) {}

    cy::Array<cy::demo::Health> health;
    cy::Array<cy::demo::Placement> placement;

    [[nodiscard]] cy::u32 count() const noexcept { return static_cast<cy::u32>(health.size()); }

    /// Reserve `count` entities, default-constructed.
    [[nodiscard]] cy::Status resize(cy::u32 count) noexcept;
};

/// The five field bindings the stage uses, resolved once.
///
/// The identifiers are the manifest's (identity/manifest.toml), not the field names: a rename is a
/// manifest edit and must not reach this file. That is the same contract a serialized record is
/// written under, which is why the package this data arrives in is still readable after one.
struct Bindings {
    cy::reflect::TypedAccessor<cy::f32> maximum;    // cy::demo::Health field 1
    cy::reflect::TypedAccessor<cy::f32> current;    // cy::demo::Health field 2
    cy::reflect::TypedAccessor<cy::f32> displayed;  // cy::demo::Health field 3
    cy::reflect::TypedAccessor<cy::f32> x;          // cy::demo::Placement field 1
    cy::reflect::TypedAccessor<cy::f32> y;          // cy::demo::Placement field 2
    cy::reflect::TypedAccessor<cy::f32> rotation;   // cy::demo::Placement field 3

    /// One reflected lookup per field, against the registry, at setup. Fails naming the field when
    /// the type it resolves against no longer holds it.
    [[nodiscard]] static cy::Expected<Bindings, cy::Error> resolve(
        const cy::reflect::TypeRegistry& registry) noexcept;
};

/// What one frame of the stage did. Every figure is a function of the data and the frame index
/// alone — no worker count, no thread identity — so two runs report the same numbers.
struct FrameResult {
    cy::u64 commands_applied = 0;
    cy::u64 retired = 0;  ///< entities whose health reached zero this frame
};

/// The three systems, their access declarations, and the plan derived from them.
class Stage {
public:
    Stage() noexcept = default;

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    /// Register the systems and build the plan. `world` and `bindings` must outlive the stage.
    [[nodiscard]] cy::Status build(World& world, const Bindings& bindings) noexcept;

    /// Run one frame: every batch in order, the systems within a batch in parallel, then the
    /// deferred structural changes applied in commit order.
    [[nodiscard]] cy::Expected<FrameResult, cy::Error> run(cy::jobs::JobSystem& jobs) noexcept;

    /// The plan, as one line: "[decay drift] [summarise]". This is the derived parallelism made
    /// visible, and it is what changes when a declaration does.
    void format_plan(char* buffer, cy::usize capacity) const noexcept;

    [[nodiscard]] cy::u32 system_count() const noexcept { return schedule_.system_count(); }
    [[nodiscard]] cy::u32 batch_count() const noexcept { return schedule_.batch_count(); }

    /// A checksum over every field the stage writes. The determinism claim, in one number.
    [[nodiscard]] cy::u64 checksum() const noexcept;

private:
    /// What a system body is handed. One instance, shared by all three: they touch different
    /// components, which is exactly what their declarations say.
    struct State {
        World* world = nullptr;
        const Bindings* bindings = nullptr;
        cy::u64 frame = 0;
    };

    static void decay(const cy::jobs::SystemContext& context, void* user) noexcept;
    static void drift(const cy::jobs::SystemContext& context, void* user) noexcept;
    static void summarise(const cy::jobs::SystemContext& context, void* user) noexcept;
    static void apply_command(const cy::jobs::StructuralCommand& command, void* user) noexcept;

    State state_;
    cy::jobs::SystemSchedule schedule_;
    cy::jobs::DeferredCommands commands_;
    cy::u64 retired_ = 0;
};

}  // namespace sample

#endif  // CY_SAMPLE_HEADLESS_HOST_SIMULATION_H
