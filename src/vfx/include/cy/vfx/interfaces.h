#pragma once
// DATA INTERFACES: declared, versioned, extensible sources a graph reads engine data through.
// M8.c task 2.5.
//
// ================================================================================================
// EXTENSIBLE MEANS THE COMPILER DOES NOT KNOW THE LIST
// ================================================================================================
//
// `vfx-system`: "Data interfaces SHALL be extensible: modules and projects SHALL be able to
// register their own WITHOUT MODIFYING THE COMPILER", and its scenario is explicit that sampling
// the scene signed distance field must work "with no bespoke compiler support for that specific
// case".
//
// So `compile.cpp` contains no interface names. It resolves a `vfx.sample` node's `interface` and
// `field` properties against whatever registry it was handed, types the read from the field's own
// declaration, and emits `cy_vfx_sample_<interface>_<field>(...)`. The built-in list below is
// registered through the same public call a project uses, which is what makes that claim
// checkable: `register_builtin_interfaces` is not a friend and has no privileges.
//
// ================================================================================================
// TWO DECLARATIONS THAT ARE COOK-TIME GATES, NOT DOCUMENTATION
// ================================================================================================
//
// "Each data interface SHALL declare its COST CLASS and WHETHER IT IS AVAILABLE ON THE CPU PATH, so
// the compiler can reject or warn about use in effects that require CPU simulation", and its
// scenario: "WHEN an effect declares CPU simulation and uses a GPU-only data interface THEN
// COOKING SHALL FAIL with a diagnostic naming the interface."
//
// `cpu_available` is that gate and `compile_system` fails on it. The cost class reaches the cook
// report, so an author sees what a sample cost them before a frame does.
//
// ================================================================================================
// WIND IS SHARED AND IS NOT VFX'S
// ================================================================================================
//
// "Wind SHALL be read from the shared wind field rather than a VFX-owned wind model, so that
// particles, foliage, cloth, and water agree about the wind in the same frame. Force fields local
// to an effect remain VFX-owned; the ambient wind does not."
//
// `wind_field` is therefore in the built-in list as an interface like any other, and there is no
// wind model in `src/vfx/`. A change that added one would be adding a second answer to a question
// `weather-and-wind` already owns.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/vfx/ir.h>

namespace cy::vfx {

/// What sampling this interface costs, relative to arithmetic. Reported per effect so a cook report
/// says where an effect's cost went rather than only that it has some.
enum class InterfaceCost : u8 {
    /// A uniform read: camera state, a parameter block.
    Trivial = 0,
    /// A buffer load with good locality.
    Cheap,
    /// A texture or volume sample.
    Moderate,
    /// A traversal or a query: the scene SDF, a physics cast.
    Expensive,
};

[[nodiscard]] const char* interface_cost_name(InterfaceCost cost) noexcept;
/// A relative weight the cook report multiplies by, so "estimated cost at a reference population"
/// is a number rather than an adjective.
[[nodiscard]] u32 interface_cost_weight(InterfaceCost cost) noexcept;

/// One readable field of an interface.
struct InterfaceField {
    const char* name = "?";
    TypeId type = Float;
};

/// An interface as its owner declares it. The arrays are borrowed; the registry copies them.
struct DataInterfaceDesc {
    const char* name = "?";
    /// VERSIONED, because a graph cooked against version 1 of an interface must not silently bind
    /// to version 2's fields. The cook key closes over it.
    u32 version = 1;
    Span<const InterfaceField> fields;
    InterfaceCost cost = InterfaceCost::Moderate;
    /// False makes an effect that declares `SimulationPath::CpuRequired` fail to cook, naming this
    /// interface. See the note at the top of this file.
    bool cpu_available = false;
    bool gpu_available = true;
};

/// A registered interface. Owns its field table.
class DataInterface {
public:
    DataInterface(Allocator& allocator, const DataInterfaceDesc& desc) noexcept;

    DataInterface(const DataInterface&) = delete;
    DataInterface& operator=(const DataInterface&) = delete;
    DataInterface(DataInterface&&) noexcept = default;
    DataInterface& operator=(DataInterface&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] u32 version() const noexcept { return version_; }
    [[nodiscard]] InterfaceCost cost() const noexcept { return cost_; }
    [[nodiscard]] bool cpu_available() const noexcept { return cpu_available_; }
    [[nodiscard]] bool gpu_available() const noexcept { return gpu_available_; }
    [[nodiscard]] Span<const InterfaceField> fields() const noexcept { return fields_.span(); }
    [[nodiscard]] const InterfaceField* find_field(Name field) const noexcept;

private:
    Name name_;
    Array<InterfaceField> fields_;
    u32 version_ = 1;
    InterfaceCost cost_ = InterfaceCost::Moderate;
    bool cpu_available_ = false;
    bool gpu_available_ = true;
};

/// The interfaces one cook knows about.
class DataInterfaceRegistry {
public:
    explicit DataInterfaceRegistry(Allocator& allocator) noexcept : interfaces_(allocator) {}

    DataInterfaceRegistry(const DataInterfaceRegistry&) = delete;
    DataInterfaceRegistry& operator=(const DataInterfaceRegistry&) = delete;

    [[nodiscard]] Status register_interface(const DataInterfaceDesc& desc) noexcept;
    [[nodiscard]] const DataInterface* find(Name interface_name) const noexcept;
    [[nodiscard]] Span<const DataInterface> all() const noexcept { return interfaces_.span(); }
    [[nodiscard]] usize size() const noexcept { return interfaces_.size(); }

    /// A digest over every registered interface's name and version, in registration order. Part of
    /// the cook key, so a project that bumps its own interface's version invalidates the effects
    /// that read it.
    [[nodiscard]] u64 digest() const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return interfaces_.allocator(); }

private:
    Array<DataInterface> interfaces_;
};

/// The list `vfx-system` requires the engine to provide, registered through the public call.
///
/// "The engine SHALL provide at least: scene depth, scene normals, the scene signed distance field,
/// the GPU scene, physics collision queries, terrain height and material, static and skeletal mesh
/// sampling, texture and curve sampling, camera state, environment fields, audio spectrum and
/// level, ECS query results, and a generic structured buffer" — plus the shared wind field, which
/// the same requirement puts here rather than in a VFX-owned model.
[[nodiscard]] Status register_builtin_interfaces(DataInterfaceRegistry& registry) noexcept;

/// The GRID DATA INTERFACE CONTRACT, one of the three seams `vfx-system` reserves for fluids:
/// "a grid data interface contract for graphs to read and write volumetric data". It is registered
/// by `register_builtin_interfaces` and read by nothing in this milestone, which is exactly what
/// "the seams are checked, not filled" means — a proposal that removed it would be flagged against
/// the "Fluids are deferred with reserved seams" requirement, and `test_vfx_compiler.cpp` is what
/// notices.
inline constexpr const char* kGridInterfaceName = "grid";

}  // namespace cy::vfx
