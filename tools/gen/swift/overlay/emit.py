"""The Swift the generator writes. Task 3.1, `swift-scripting` "Generated overlay".

Every function here takes the ABI description and returns one file's text. They share three rules:

  * NO TIMESTAMP, NO PATH, NO HOST. `build-system-and-platforms` requires generation to be
    deterministic — identical inputs, byte-identical outputs — and `just generate-swift --check`
    compares the committed files against a fresh run. A generator that stamped the time would fail
    its own currency check once a second.
  * EVERY FILE CARRIES THE SAME BANNER, naming the generator and the header. A reader who opens a
    generated file and edits it anyway has been told; a reviewer who sees the banner in a diff knows
    the change belongs in the generator or in the ABI.
  * NOTHING IS EMITTED THAT THE DESCRIPTION DOES NOT CONTAIN. Where the C header states something in
    prose that no parser could recover — the bit packing inside `CyEntity`, for instance — the Swift
    for it is hand-written in `CyberdyneKit` and says where the rule comes from. A generator that
    guessed would be a second, unchecked copy of the ABI.
"""

from __future__ import annotations

from . import cnames, entries, swifttypes

BANNER = """\
// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.
"""

# The four vector kinds the ABI's `CyVarPayload` carries in its `as_f32x4` slot, and how many of its
# four floats each uses. Derived from the CyVarType enum below rather than listed by hand: the
# generator refuses to emit a math type whose CY_VAR_* constant is not in the description.
VECTOR_LANES = {"CY_VAR_VEC2": ("Vec2", ("x", "y")),
                "CY_VAR_VEC3": ("Vec3", ("x", "y", "z")),
                "CY_VAR_VEC4": ("Vec4", ("x", "y", "z", "w")),
                "CY_VAR_QUAT": ("Quat", ("x", "y", "z", "w"))}


def _find(description: dict, kind: str, name: str) -> dict:
    for entry in description["types"]:
        if entry["kind"] == kind and entry["name"] == name:
            return entry
    raise entries.EntryError(f"the ABI description has no {kind} named {name!r}")


# The ABI's enums, and the two things about each that the description cannot carry: the prefix its
# constants share (`CyVarType`'s constants say `CY_VAR_`, not `CY_VAR_TYPE_`) and the Swift name.
#
# `CyResult` becomes `Status` rather than `Result`, which is the one place a mechanical `Cy`-strip
# would have produced a name that shadows a standard-library type inside every file that imports
# this module. An enum in the description with no row here is a generation error, so adding one to
# the C header stops the build until somebody chooses its Swift spelling.
ENUM_SPECS = {
    "CyResult": ("Status", "CY_RESULT_", "Int32"),
    "CyVarType": ("VarType", "CY_VAR_", "UInt32"),
    "CyInitLevel": ("InitLevel", "CY_INIT_LEVEL_", "UInt32"),
}


def enum_spec(name: str) -> tuple[str, str, str]:
    """The Swift name, constant prefix and raw type for one ABI enum, or an error naming it."""
    spec = ENUM_SPECS.get(name)
    if spec is None:
        raise entries.EntryError(
            f"the ABI declares enum {name!r}, which tools/gen/swift/overlay/emit.py has no Swift "
            f"name for. Add a row to ENUM_SPECS: the Swift spelling, the constants' shared prefix "
            f"and the raw type.")
    return spec


def check_enum_coverage(description: dict) -> None:
    """Every enum in the description has a row above. Called before anything is emitted."""
    for declaration in description["types"]:
        if declaration["kind"] == "enum":
            enum_spec(declaration["name"])


# --- versions and enums ---------------------------------------------------------------------------


def abi_version(description: dict) -> str:
    version = description["abi"]
    table = description["table"]
    return f"""{BANNER}
/// The ABI version this overlay was generated against.
///
/// A module built on this overlay is compiled against exactly this table. The check that matters at
/// run time is the module's own, in `CyberdyneKit`'s module entry point: the engine's
/// `header.table_size` must be at least the size recorded here, and `abiMajor` must be equal. That
/// is `native-abi`'s "Older engine, newer module", and refusing there is how it is reported without
/// aborting engine startup.
public enum ABI {{
    public static let major: UInt32 = {version["major"]}
    public static let minor: UInt32 = {version["minor"]}
    public static let patch: UInt32 = {version["patch"]}

    /// `sizeof(CyInterface)` as this overlay was generated. The engine may export a larger table —
    /// that is what append-only growth looks like from here — and may never export a smaller one.
    public static let interfaceTableSize: UInt32 = {table["size"]}

    /// The entries this overlay knows, in the table's order. Written down so that a diagnostic can
    /// say *which* entry a mismatched table stops at rather than only that the sizes differ.
    public static let entryNames: [String] = [
{chr(10).join(f'        "{entry["name"]}",' for entry in entries.function_entries(description))}
    ]
}}
"""


def _enum(description: dict, name: str, doc: str) -> str:
    declaration = _find(description, "enum", name)
    swift, prefix, raw = enum_spec(name)
    lines = [f"    case {cnames.enum_case(value['name'], prefix)} = {value['value']}"
             for value in declaration["values"]]
    return (f"{doc}\npublic enum {swift}: {raw}, Sendable, CaseIterable {{\n"
            + "\n".join(lines) + "\n}\n")


def enums(description: dict) -> str:
    check_enum_coverage(description)
    status = _enum(description, "CyResult", """\
/// `CyResult`, as a Swift enum. `swift-scripting`: "Swift `enum`s for ABI enums".
///
/// The raw values are the C enumerators' own, so `Status(rawValue:)` over a `CyResult` cannot
/// reorder them. The first fifteen are `cy::ErrorCode`'s, in its order — see cy_abi.h, which
/// explains why that is a cast in the engine rather than a switch.""")
    var_type = _enum(description, "CyVarType", """\
/// `CyVarType`: the kinds a value may carry across the boundary.""")
    init_level = _enum(description, "CyInitLevel", """\
/// `CyInitLevel`: when a module registers what. Types are registered at `.scene`.""")
    return f"""{BANNER}
{status}
{var_type}
{init_level}
/// The error every throwing overlay call raises.
///
/// `swift-scripting`: "the overlay SHALL throw a typed `CyberdyneError` carrying the status and the
/// engine's last-error message", and, separately, "accessing it SHALL return `nil` or throw a
/// `CyberdyneError.invalidHandle`". Both spellings are cases of one enum so that a single
/// `catch` covers the boundary.
public enum CyberdyneError: Error, Sendable, Equatable {{
    /// An ABI call returned a failure status. The message is the engine's `get_last_error` at the
    /// moment of the failure, copied — the C pointer is only valid until this thread's next one.
    case status(Status, message: String)
    /// A handle whose target no longer exists, or was never valid.
    case invalidHandle
    /// A value that cannot cross the boundary in a `CyVar`: `swift-scripting`'s "non-representable
    /// exported types". Carries the Swift type's name.
    case notRepresentable(String)
}}

extension CyberdyneError: CustomStringConvertible {{
    public var description: String {{
        switch self {{
        case let .status(status, message):
            return message.isEmpty ? "\\(status)" : "\\(status): \\(message)"
        case .invalidHandle:
            return "invalid handle"
        case let .notRepresentable(type):
            return "\\(type) is not representable across the ABI"
        }}
    }}
}}
"""


# --- math ------------------------------------------------------------------------------------------


def math(description: dict) -> str:
    declaration = _find(description, "enum", "CyVarType")
    present = {value["name"] for value in declaration["values"]}
    missing = sorted(name for name in VECTOR_LANES if name not in present)
    if missing:
        raise entries.EntryError(
            "the ABI description's CyVarType is missing " + ", ".join(missing)
            + ". The math types are generated from it, so a removed vector kind must be a removed "
              "math type and not a silently skipped one.")

    blocks = [
        _vector(name, lanes)
        for constant, (name, lanes) in sorted(VECTOR_LANES.items(), key=lambda item: item[1][0])
        if constant in present
    ]
    return f"""{BANNER}
// The math types, generated from `CyVarType`'s vector kinds.
//
// They have no C struct to mirror: the ABI carries a vector in `CyVarPayload.as_f32x4`, four floats
// with the used lanes at the front. So the layout claim these make is against that array, and
// `Tests/CyberdyneCoreTests/Generated/LayoutTests.swift` asserts it — a `Vec3` must be three
// contiguous floats or `withUnsafeBytes` into the payload writes the wrong thing.
//
// `@frozen` because the layout IS the ABI: a future field would change what crosses the boundary,
// which is a change to `CyVarPayload` and not to this file.

{chr(10).join(blocks)}"""


def _vector(name: str, lanes: tuple[str, ...]) -> str:
    parameters = ", ".join(f"{lane}: Float = 0" for lane in lanes)
    assignments = "\n".join(f"        self.{lane} = {lane}" for lane in lanes)
    stored = "\n".join(f"    public var {lane}: Float" for lane in lanes)
    zero = ", ".join(f"{lane}: 0" for lane in lanes)
    return f"""@frozen
public struct {name}: Equatable, Sendable {{
{stored}

    @inlinable
    public init({parameters}) {{
{assignments}
    }}

    public static let zero = {name}({zero})

    /// The lane count, so that generic code over the math types does not repeat the number.
    public static var lanes: Int {{ {len(lanes)} }}
}}
"""


# --- the interface table ---------------------------------------------------------------------------


def _handle_names(description: dict) -> set[str]:
    return {item["name"] for item in description["types"] if item["kind"] == "handle"}


def _nullable_return(returns: str, handles: set[str]) -> bool:
    """A returned pointer is Optional in Swift, always.

    C has no nullability annotations, so the alternative is to trust a header comment; cy_abi.h says
    `get_last_error` is "never null", and an overlay that encoded that would trap rather than throw
    on the day it stopped being true. The engine's own tests assert the guarantee; this file does
    not have to bet on it.
    """
    return returns.endswith("*") or returns in handles


def _method(entry: dict, handles: set[str], *, receiver: str | None = None) -> str:
    """One Swift method for one table entry.

    `receiver` names the wrapper the method is being emitted into, for `Handles.swift`: the first
    parameter is then hidden and the wrapper's own `raw` is passed for it, so `world_create_entity`
    reads `world.createEntity()` rather than `world.createEntity(world: world.raw)`.
    """
    record = entries.ENTRIES[entry["name"]]
    returns, parameters = entries.signature(entry)
    labels = list(record.labels)
    # A POINTER PARAMETER IS OPTIONAL AND A HANDLE PARAMETER IS NOT, which is not a style choice.
    # C says nothing about nullability, but the header does say what each argument means: every
    # pointer parameter in this table has a documented null case (`initial` may be null; a null
    # `buffer` is the size query), and no entry accepts a null handle — passing one is the
    # programmer error the distinct handle typedefs exist to catch. A caller passing a non-optional
    # value into an Optional parameter costs nothing; the reverse costs a force-unwrap at every
    # call site that legitimately means "none".
    types = [swifttypes.imported(spelling, optional=spelling.endswith("*"))
             for spelling in parameters]

    hidden = 1 if receiver is not None else 0
    declared = ", ".join(f"{label}: {swift}" for label, swift in zip(labels[hidden:], types[hidden:]))
    values = (["raw"] if receiver is not None else []) + labels[hidden:]

    name = _method_name(entry["name"], receiver)
    call = _call(entry["name"], labels, values, receiver)

    if record.result == "throwing":
        body = f"try check({call})" if receiver is None else f"try {call}"
        return _function(name, declared, "throws", body)
    if returns == "void":
        return _function(name, declared, "", call)
    swift_return = swifttypes.imported(returns, optional=_nullable_return(returns, handles))
    return _function(name, declared, f"-> {swift_return}", call)


def _function(name: str, declared: str, effect: str, body: str) -> str:
    signature = f"    public func {name}({declared})"
    if effect:
        signature += f" {effect}"
    return f"    @inlinable\n{signature} {{\n        {body}\n    }}\n"


def _call(c_name: str, labels: list[str], values: list[str], receiver: str | None) -> str:
    """The call this method's body makes: the raw C entry, or the `Interface` method wrapping it."""
    if receiver is None:
        return f"table.pointee.{c_name}({', '.join(values)})"
    arguments = ", ".join(f"{label}: {value}" for label, value in zip(labels, values))
    return f"interface.{cnames.entry_method(c_name)}({arguments})"


# A handle wrapper's method name drops the leading word that names the handle it is called on:
# `world_create_entity` on a `World` reads `createEntity`, `behaviour_generation` on a
# `BehaviourType` reads `generation`. Everything else keeps its name, so `component_get_f32` stays
# `componentGetF32` on `World` and `register_behaviour` stays `registerBehaviour` on `Engine`.
_RECEIVER_WORDS = {"World": "world_", "Engine": "engine_", "BehaviourType": "behaviour_"}


def _method_name(c_name: str, receiver: str | None) -> str:
    if receiver is not None:
        word = _RECEIVER_WORDS[receiver]
        if c_name.startswith(word) and len(c_name) > len(word):
            return cnames.entry_method(c_name[len(word):])
    return cnames.entry_method(c_name)


def interface(description: dict) -> str:
    entries.validate(description)
    handles = _handle_names(description)
    methods = "\n".join(_method(entry, handles)
                        for entry in entries.function_entries(description))
    return f"""{BANNER}
import CyberdyneABI

/// The engine's exported interface table, with one Swift method per entry.
///
/// It is deliberately thin: every method is the imported C call and nothing else, so there is no
/// second model of the ABI anywhere in this package. The ergonomics — Swift `String`s, `Vec3`,
/// component accessors that return rather than fill an out-parameter — are `CyberdyneKit`'s, which
/// is hand-written on purpose (`swift-scripting` names it "the hand-written ergonomic layer").
///
/// `@unchecked Sendable` because it is a pointer to memory the engine owns for the process
/// lifetime and never mutates after `cy_get_interface` returns. The rule that actually governs
/// which thread may call these is in `CyberdyneKit/Concurrency.swift`, and it is stronger than
/// `Sendable`: engine mutation is confined to `@GameActor`.
public struct Interface: @unchecked Sendable {{
    public let table: UnsafePointer<CyInterface>

    @inlinable
    public init(_ table: UnsafePointer<CyInterface>) {{
        self.table = table
    }}

    /// The table the engine actually exported. `native-abi`'s additive growth rule from this side:
    /// the engine may export MORE entries than this overlay knows, never fewer.
    @inlinable public var abiMajor: UInt32 {{ table.pointee.header.abi_major }}
    @inlinable public var abiMinor: UInt32 {{ table.pointee.header.abi_minor }}
    @inlinable public var abiPatch: UInt32 {{ table.pointee.header.abi_patch }}
    @inlinable public var tableSize: UInt32 {{ table.pointee.header.table_size }}

    /// The module's half of the version handshake, `native-abi`'s "Older engine, newer module".
    ///
    /// True when this overlay may call every entry it knows about. A module that gets `false` must
    /// return `false` from `cy_module_entry` — which the loader reports and survives — rather than
    /// calling into a table that stops short of what it was compiled against.
    @inlinable
    public var isCompatible: Bool {{
        abiMajor == ABI.major && tableSize >= ABI.interfaceTableSize
    }}

    /// Turn a failing `CyResult` into a thrown `CyberdyneError`, carrying the engine's message.
    ///
    /// The message is copied here rather than held: `get_last_error` documents its pointer as valid
    /// only until this thread's next failing call, so a `String` made later would read whatever
    /// failed since.
    @inlinable
    public func check(_ result: CyResult) throws {{
        if result == CY_RESULT_OK {{ return }}
        let status = Status(rawValue: Int32(result.rawValue)) ?? .unknown
        var message = ""
        if let text = table.pointee.get_last_error() {{
            message = String(cString: text)
        }}
        throw CyberdyneError.status(status, message: message)
    }}

{methods}}}
"""


# --- the handle wrappers ---------------------------------------------------------------------------


_HANDLE_WRAPPERS = {"CyEngine": "Engine", "CyWorld": "World", "CyBehaviourType": "BehaviourType"}


def _wrapper_groups(description: dict) -> dict[str, list[dict]]:
    """Group the table's entries by the handle type of their first parameter.

    The grouping is derived, not listed: an entry whose first parameter is a `CyWorld` becomes a
    method on `World`. An entry whose first parameter is not a handle — `var_clone`, `var_release`,
    `get_last_error` — belongs to no wrapper and stays on `Interface`, which is correct: those are
    the entries that do not act on anything the engine owns.
    """
    groups: dict[str, list[dict]] = {name: [] for name in sorted(set(_HANDLE_WRAPPERS.values()))}
    for entry in entries.function_entries(description):
        _, parameters = entries.signature(entry)
        if parameters and parameters[0] in _HANDLE_WRAPPERS:
            groups[_HANDLE_WRAPPERS[parameters[0]]].append(entry)
    return groups


def _check_no_collisions(wrapper: str, group: list[dict]) -> None:
    seen: dict[str, str] = {}
    for entry in group:
        name = _method_name(entry["name"], wrapper)
        if name in seen:
            raise entries.EntryError(
                f"{wrapper}: {entry['name']!r} and {seen[name]!r} both become {name!r}. The rule "
                f"that strips the receiver's word from a method name has produced a collision; "
                f"either the ABI entry is misnamed or _RECEIVER_WORDS needs the case spelled out.")
        seen[name] = entry["name"]


def handles(description: dict) -> str:
    handle_names = _handle_names(description)
    groups = _wrapper_groups(description)
    blocks = []
    for wrapper, group in groups.items():
        _check_no_collisions(wrapper, group)
        methods = "\n".join(_method(entry, handle_names, receiver=wrapper) for entry in group)
        blocks.append(f"""public struct {wrapper}: @unchecked Sendable {{
    public let raw: Cy{wrapper}
    public let interface: Interface

    @inlinable
    public init(_ raw: Cy{wrapper}, _ interface: Interface) {{
        self.raw = raw
        self.interface = interface
    }}

{methods}}}
""")
    return f"""{BANNER}
import CyberdyneABI

// The typed wrappers over the ABI's opaque handles.
//
// Which entries land on which wrapper is DERIVED rather than listed: an entry whose first parameter
// is a `CyWorld` is a method on `World`, and one whose first parameter is not a handle at all stays
// on `Interface`. So appending a `world_*` entry to `CyInterface` puts a method on `World` with no
// edit here, and appending one that takes no handle does not.
//
// A wrapper is a handle plus the table to reach it through; it owns nothing and keeps nothing
// alive. `swift-scripting`: "A `Node` or entity wrapper is a **handle**, not an owning reference;
// holding one does not keep the entity alive."

{chr(10).join(blocks)}
/// An entity, as the ABI carries it. `native-abi` fixes the encoding: the 32-bit index low and the
/// 32-bit generation high, with zero reserved for the null entity because a generation of zero is
/// never issued.
@frozen
public struct Entity: Hashable, Sendable {{
    public var bits: CyEntity

    @inlinable
    public init(bits: CyEntity) {{
        self.bits = bits
    }}

    /// `CY_ENTITY_NULL`, read from the C header rather than written as a literal here.
    public static let null = Entity(bits: CY_ENTITY_NULL)

    @inlinable public var isNull: Bool {{ bits == CY_ENTITY_NULL }}
}}

/// A component type's id within one world. Registration order is id order, so an id is meaningful
/// only against the world that issued it.
@frozen
public struct ComponentType: Hashable, Sendable {{
    public var id: CyComponentTypeId

    @inlinable
    public init(id: CyComponentTypeId) {{
        self.id = id
    }}

    public static let invalid = ComponentType(id: CY_COMPONENT_TYPE_INVALID)

    @inlinable public var isValid: Bool {{ id != CY_COMPONENT_TYPE_INVALID }}
}}
"""


# --- the layout assertions ---------------------------------------------------------------------------


def _layout_assertions(declaration: dict) -> list[str]:
    """Size, alignment and — for a struct — every member's offset, as XCTest lines."""
    name = declaration["name"]
    lines = [
        f'        XCTAssertEqual(MemoryLayout<{name}>.size, {declaration["size"]}, '
        f'"{name} size")',
        f'        XCTAssertEqual(MemoryLayout<{name}>.alignment, {declaration["alignment"]}, '
        f'"{name} alignment")',
    ]
    if declaration["kind"] != "struct":
        # A C union is imported as a Swift struct whose members are COMPUTED properties over one
        # stored payload, so `offset(of:)` answers nil for every one of them. Asserting the size and
        # the alignment is the whole of what can be checked here, and it is what matters: every
        # member of CyVarPayload is at offset zero by definition.
        return lines
    for member in declaration["members"]:
        key = cnames.member(member["name"]) if member["name"] in cnames.SWIFT_KEYWORDS \
            else member["name"]
        lines.append(
            f'        XCTAssertEqual(MemoryLayout<{name}>.offset(of: \\{name}.{key}), '
            f'{member["offset"]}, "{name}.{member["name"]} offset")')
    return lines


def layout_tests(description: dict) -> str:
    """The generated layout suite. `swift-scripting`: "Layout compatibility is verified".

    WHY THIS EXISTS AS A TEST RATHER THAN A `static_assert` IN THE OVERLAY. The numbers it checks are
    *computed* by `tools/abi/abi_describe.py` from the declaration, under the layout model the C ABI
    fixes — they are not measured from a compiler. `src/abi/tests/test_layout.cpp` already asserts
    the C++ compiler agrees with those numbers; this asserts that Swift's C importer does too, on
    whatever platform the package is built for, which is the half no C++ test can reach.
    """
    declarations = [item for item in description["types"] if item["kind"] in ("struct", "union")]
    blocks = []
    for declaration in declarations:
        blocks.append(f"""    func test{cnames.type_name(declaration["name"])}Layout() {{
{chr(10).join(_layout_assertions(declaration))}
    }}
""")

    vectors = "\n".join(
        f'        XCTAssertEqual(MemoryLayout<{name}>.size, {4 * len(lanes)}, "{name} size")\n'
        f'        XCTAssertEqual(MemoryLayout<{name}>.alignment, 4, "{name} alignment")\n'
        + "\n".join(
            f'        XCTAssertEqual(MemoryLayout<{name}>.offset(of: \\{name}.{lane}), '
            f'{4 * index}, "{name}.{lane} offset")'
            for index, lane in enumerate(lanes))
        for name, lanes in sorted(VECTOR_LANES.values()))

    return f"""{BANNER}
import CyberdyneABI
import XCTest

@testable import CyberdyneCore

/// Every ABI struct's size, alignment and member offsets, as Swift's C importer sees them.
///
/// The expected numbers come from the ABI description, which computes them from the declaration
/// rather than measuring them on this machine — see tools/abi/abi_describe.py for why. So a failure
/// here means one of two things, and both are worth stopping for: Swift's importer disagrees with
/// the layout model on this platform, or the header changed and the overlay was not regenerated.
final class GeneratedLayoutTests: XCTestCase {{
{chr(10).join(blocks)}
    /// The table itself. `Interface` reads entries by name through the imported struct, so if Swift
    /// laid `CyInterface` out differently from the engine, every call would go to the wrong entry.
    func testInterfaceTableSize() {{
        XCTAssertEqual(MemoryLayout<CyInterface>.size, {description["table"]["size"]},
                       "CyInterface size")
        XCTAssertEqual(Int(ABI.interfaceTableSize), MemoryLayout<CyInterface>.size,
                       "the generated table size and the imported one")
    }}

    /// The math types, which have no C struct to mirror: the ABI carries them as the leading lanes
    /// of `CyVarPayload.as_f32x4`, so each must be exactly its lane count of contiguous floats.
    func testVectorLayout() {{
{vectors}
        XCTAssertEqual(MemoryLayout<CyVarPayload>.size, 16, "the payload the vectors live in")
    }}

    /// The overlay's version constants against the header's own macros, which the C importer brings
    /// across independently of the description. Two routes to the same three numbers; if they ever
    /// disagree, the overlay was generated from a different header than the one being compiled.
    func testVersionConstantsAgreeWithTheHeader() {{
        XCTAssertEqual(ABI.major, CY_ABI_MAJOR)
        XCTAssertEqual(ABI.minor, CY_ABI_MINOR)
        XCTAssertEqual(ABI.patch, CY_ABI_PATCH)
    }}

    /// The entry names, in order. A reorder in `CyInterface` is what `just quality-abi` refuses;
    /// this is the same claim from Swift's side, and it is what makes `ABI.entryNames` — which a
    /// diagnostic uses to say *which* entry a short table stops at — worth trusting.
    func testEntryNameCount() {{
        XCTAssertEqual(ABI.entryNames.count, {len(entries.ENTRIES)})
        XCTAssertEqual(ABI.entryNames.first, "{next(iter(entries.ENTRIES))}")
        XCTAssertEqual(ABI.entryNames.last, "{list(entries.ENTRIES)[-1]}")
    }}
}}
"""
