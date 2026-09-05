#pragma once
// Bindings, composites, processors, modifiers, and the mapping contexts that hold them.
// Tasks 4.1.2 and 4.1.3.
//
// --- PROCESSORS ARE NUMERICAL, MODIFIERS ARE CONTEXTUAL ------------------------------------------
//
// `input-and-actions` states the split as a requirement and then states why: "Keeping processors
// numerical and modifiers contextual SHALL be maintained, so that a processor chain can be
// evaluated with no knowledge of the world."
//
// That is not a taxonomy for its own sake. A processor chain with no world in it can be evaluated
// in a test, in a cook, in an editor preview and on a dedicated server, and it can be flattened
// into a table of POD structs — which is what makes "Processor evaluation SHALL NOT allocate per
// frame" a property of the type rather than a discipline. The moment a dead zone could ask the
// camera for its forward vector, all four of those stop being true.
//
// So: `Processor` is `{kind, a, b}` and evaluates `Vec3 -> Vec3`. `Modifier` is the one that may
// consult a reference frame or a player setting, it is a single field on the binding rather than a
// chain, and the frame is **supplied to the user** by whoever owns the camera. The input system
// never reaches into the renderer; the requirement's scenario says so explicitly and the header
// makes it structural — there is no renderer type in this file's includes and none is reachable
// from layer 2.
//
// --- COMPOSITES ARE BINDINGS, NOT GAMEPLAY CODE
// ---------------------------------------------------
//
// "Composite bindings SHALL be supported: one-dimensional and two-dimensional axes assembled from
// discrete controls, radial composites, chords, and sequences — so that four keys produce one
// two-dimensional action without gameplay knowing." A `Binding` therefore holds up to four
// components with per-axis weights, and WASD is one binding of kind `Axis2D` whose four components
// weight (-1,0), (+1,0), (0,-1), (0,+1). Gameplay reads one `Vec2`.
//
// --- WHY BINDINGS ARE IMMUTABLE AT RUNTIME
// --------------------------------------------------------
//
// "Authored binding assets SHALL be immutable at runtime. Player changes SHALL be stored as
// overrides in a profile, applied over the defaults." A `MappingContext` is therefore const once
// registered, and `profile.h` holds the overrides. A shipped content update replaces the context
// and the player's overrides still apply over it — which is the scenario, and it only works if
// nothing ever writes into the context.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/servers/input/action.h>
#include <cy/servers/input/types.h>

namespace cy::input {

// --- Processors ----------------------------------------------------------------------------------

/// A numerical transformation. Stateless unless the kind says otherwise, and the stateful ones keep
/// their state in **per-binding storage on the user**, never in an allocated object — see
/// `input-and-actions`: "those requiring state SHALL store it in per-binding storage rather than as
/// allocated objects".
enum class ProcessorKind : u8 {
    None = 0,
    /// Per-axis dead zone: below `a` the axis is zero, and the remainder is rescaled so the value
    /// still reaches 1 at full deflection. Rescaling is the part people forget, and without it a
    /// stick with a dead zone can never reach full speed.
    DeadZone,
    /// Radial dead zone over the whole vector, which is what a stick actually needs: an axis-wise
    /// dead zone leaves a square hole and makes diagonals reachable that the cardinals are not.
    RadialDeadZone,
    /// Scale the vector to unit length when it exceeds it.
    Normalize,
    /// Multiply by `a` (and by `b` on Y when `b` is non-zero).
    Scale,
    Invert,
    /// A player-facing sensitivity multiplier. Distinct from `Scale` so the inspector can say which
    /// factor came from the binding and which from a setting.
    Sensitivity,
    /// `sign(v) * pow(|v|, a)`. `a` above 1 gives fine control near centre.
    ResponseCurve,
    /// Clamp each component to [`a`, `b`].
    Clamp,
    /// Reorder or negate components: `a` encodes the destination of each source axis. Cooked, never
    /// parsed.
    Swizzle,
    /// Exponential smoothing with time constant `a` seconds. **Stateful.**
    Smooth,
    Count,
};

const char* processor_kind_name(ProcessorKind kind) noexcept;

struct Processor {
    ProcessorKind kind = ProcessorKind::None;
    f32 a = 0.0F;
    f32 b = 0.0F;
};

/// State for the processors in one binding's chain that need it. One float per slot is enough for
/// every stateful kind declared above; a kind that needs more takes a second slot rather than this
/// becoming a variant.
inline constexpr u8 kMaxProcessors = 6;

struct ProcessorState {
    Vec3 smoothed[kMaxProcessors];
    bool primed[kMaxProcessors] = {};
};

/// Evaluate a chain. Pure apart from `state`, which is the caller's per-binding storage.
///
/// `seconds` is the time since the previous evaluation and is used **only** by `Smooth`, whose time
/// constant is a smoothing rate rather than a value. It is not a licence to scale the value: that
/// decision belongs to `Interpretation` and lives in `apply_time_step()`.
[[nodiscard]] Vec3 evaluate_processors(const Processor* processors, u8 count, Vec3 value,
                                       ProcessorState& state, f32 seconds) noexcept;

// --- Modifiers -----------------------------------------------------------------------------------

/// A semantic frame, supplied by whoever owns one — a camera, the controlled entity, a vehicle.
///
/// Supplied *to* the input system. `input-and-actions` requires that a reference-frame modifier
/// "SHALL NOT depend on renderer internals", and the way to guarantee that is for input to hold
/// three vectors it was handed rather than a pointer to something that could produce them.
struct ReferenceFrame {
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
};

enum class ModifierKind : u8 {
    None = 0,
    /// Reinterpret a 2D value in the user's reference frame: X along `right`, Y along `forward`.
    /// This is what makes a stick drive camera-relative movement without gameplay doing the maths
    /// and without input naming a camera type.
    ReferenceFrame,
    /// Multiply by a named player setting. The setting is a number the user holds; the modifier
    /// exists so that changing sensitivity does not rewrite bindings.
    SettingScale,
    Negate,
    /// Active only while the user is in a declared state — a gameplay tag the user carries. Absent
    /// state means inactive, so a modifier whose state was never set does nothing rather than
    /// silently applying.
    ActiveInState,
    Count,
};

const char* modifier_kind_name(ModifierKind kind) noexcept;

struct Modifier {
    ModifierKind kind = ModifierKind::None;
    /// The setting or state this modifier names. Resolved to an index on the user at registration.
    Name key;
    f32 fallback = 1.0F;
};

// --- Bindings ------------------------------------------------------------------------------------

enum class BindingKind : u8 {
    /// One control drives the action directly.
    Simple = 0,
    /// Two controls form one axis: negative and positive.
    Axis1D,
    /// Four controls form two axes. WASD.
    Axis2D,
    /// Two axes read as a vector and clamped to the unit disc.
    Radial,
    /// Every component must be actuated at once.
    Chord,
    /// The components must be actuated in order, within the trigger's duration.
    Sequence,
    Count,
};

const char* binding_kind_name(BindingKind kind) noexcept;

inline constexpr u8 kMaxComponents = 4;

/// One control's contribution to a binding's value.
struct BindingComponent {
    Control control;
    /// What one unit of this control adds to each axis. WASD's 'A' is `{-1, 0, 0}`.
    Vec3 weight{1.0F, 0.0F, 0.0F};
};

/// One binding, flat and copyable, with everything resolved.
///
/// No pointer, no string, no allocation. A cooked context is an array of these and a `memcpy` is a
/// valid copy of one, which is what lets the cook write them and the runtime read them back with no
/// fix-up pass.
struct Binding {
    ActionId action = kInvalidAction;
    BindingKind kind = BindingKind::Simple;
    Interpretation interpretation = Interpretation::Absolute;
    /// Which schemes this binding belongs to, so the interface can present the right prompts and
    /// rebinding can keep gamepad and keyboard overrides apart.
    u8 schemes = kAllSchemes;
    /// `input-and-actions` — a context may **consume** an action so lower contexts do not observe
    /// it. Declared per binding rather than per context, because a modal that swallows `Confirm`
    /// while passing `Look` through is the ordinary case.
    bool consume = true;
    u8 component_count = 0;
    u8 processor_count = 0;
    BindingComponent components[kMaxComponents];
    Processor processors[kMaxProcessors];
    Modifier modifier;
    TriggerSpec trigger;
};

// --- Mapping contexts ----------------------------------------------------------------------------

CY_HANDLE_TAG(MappingContext);
/// A context is pushed and popped **by handle**, never by stack position.
///
/// `input-and-actions`: "Contexts SHALL be pushed and popped by handle, not by assuming stack
/// positions, so that overlapping activations unwind correctly." A modal opened over an inventory
/// and closed after it is the case that breaks an index-based stack, and it is not rare.
using ContextHandle = Handle<MappingContextTag>;

/// A set of bindings with a priority. Immutable once registered — see the header comment.
class MappingContext {
public:
    explicit MappingContext(Allocator& allocator) noexcept : bindings_(allocator) {}

    MappingContext(const MappingContext&) = delete;
    MappingContext& operator=(const MappingContext&) = delete;
    MappingContext(MappingContext&&) noexcept = default;

    [[nodiscard]] Status add(const Binding& binding) noexcept;

    [[nodiscard]] Name name() const noexcept { return name_; }
    void set_name(Name name) noexcept { name_ = name; }

    [[nodiscard]] u32 binding_count() const noexcept { return static_cast<u32>(bindings_.size()); }
    [[nodiscard]] const Binding& binding(u32 index) const noexcept { return bindings_[index]; }

private:
    Name name_;
    Array<Binding> bindings_;
};

/// One entry on a user's context stack.
struct ContextStackEntry {
    ContextHandle context;
    /// Higher wins. Two contexts at one priority resolve by push order, most recent first, so the
    /// arrangement is total rather than arbitrary.
    i32 priority = 0;
    /// Push order, the tiebreaker. Assigned by the stack.
    u32 sequence = 0;
    bool active = true;
};

}  // namespace cy::input
