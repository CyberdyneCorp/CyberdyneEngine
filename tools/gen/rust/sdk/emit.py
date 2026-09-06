"""The Rust the generator writes. Task 2.3, `editor-rust-application` "Editor SDK boundary".

Every function here takes the ABI description and returns one file's text. They share the three
rules `tools/gen/swift/overlay/emit.py` states, for the same reasons:

  * NO TIMESTAMP, NO PATH, NO HOST. Generation is deterministic — identical inputs, byte-identical
    outputs — and `sdk_gen.py --check` compares the committed files against a fresh run.
  * EVERY FILE CARRIES THE SAME BANNER, naming the generator and the header.
  * NOTHING IS EMITTED THAT THE DESCRIPTION DOES NOT CONTAIN. Where the C header states a rule in
    prose that no parser could recover — the bit packing inside `CyEntity`, the two-call sizing
    protocol of `world_chunks` — the Rust for it is hand-written in the SDK crate and says where the
    rule comes from. A generator that guessed would be a second, unchecked copy of the ABI.

WHAT IS GENERATED AND WHAT IS DELIBERATELY NOT. Generated: the version constants, the enums, the
`#[repr(C)]` mirrors of every ABI struct and union, the interface table, one typed raw call per
table entry, and the layout assertions. **Not generated: the safe API.** Deciding that a null
`CyWorld` means "no world is bound", that a `const char*` is UTF-8 with the engine's lifetime, or
that a `CyBorrow` must be re-checked against the epoch before every read are semantic decisions the
C declarations do not contain. They are hand-written in `cy-editor-sdk`, in one audited module, and
they say which sentence of `cy_abi.h` they implement.
"""

from __future__ import annotations

from . import entries, rusttypes

BANNER = """\
// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.
"""

# The ABI's enums, and the three things about each the description cannot carry: the prefix its
# constants share (`CyVarType`'s constants say `CY_VAR_`, not `CY_VAR_TYPE_`), the Rust name, and
# the integer type the ABI actually stores it in.
#
# `CyResult` becomes `Status` rather than `Result`, which is the one place a mechanical `Cy`-strip
# would produce a name shadowing a standard-library type in every file that imports this module.
#
# The repr is the ABI's STORAGE, not C's enum width: `CyVarType` is read out of `CyVar.type`, which
# is a `uint32_t`, so a `VarType` whose repr were `i32` would need a cast at every use and would
# eventually get one that was wrong. An enum in the description with no row here is a generation
# error, so adding one to the C header stops the build until somebody chooses its Rust spelling.
ENUM_SPECS = {
    "CyResult": ("Status", "CY_RESULT_", "i32"),
    "CySeverity": ("Severity", "CY_SEVERITY_", "u32"),
    "CyVarType": ("VarType", "CY_VAR_", "u32"),
    "CyInitLevel": ("InitLevel", "CY_INIT_LEVEL_", "u32"),
    "CyStage": ("Stage", "CY_STAGE_", "u32"),
}

# What each `CyResult` means to a caller who has to act on it. `native-abi` requires a failure to be
# "a typed error with a reason, not a null or a code to interpret" (`editor-rust-application`), and
# a reason nobody wrote is a code with extra steps. An enumerator with no row here is a generation
# error, so appending one to the C header stops the build until somebody says what it means.
STATUS_MESSAGES = {
    "CY_RESULT_OK": "the call succeeded",
    "CY_RESULT_UNKNOWN": "the engine reported a failure it did not classify",
    "CY_RESULT_INVALID_ARGUMENT": "an argument was not valid for this call",
    "CY_RESULT_OUT_OF_RANGE": "a value was outside the range this call accepts",
    "CY_RESULT_NOT_FOUND": "the named thing does not exist",
    "CY_RESULT_ALREADY_EXISTS": "the thing being created is already there",
    "CY_RESULT_PERMISSION_DENIED": "the caller is not allowed to do this",
    "CY_RESULT_UNSUPPORTED": "this build of the engine does not support the operation",
    "CY_RESULT_NOT_IMPLEMENTED": "the operation is declared but not implemented yet",
    "CY_RESULT_UNAVAILABLE": "the subsystem is not available right now",
    "CY_RESULT_TIMEOUT": "the operation did not complete in the time allowed",
    "CY_RESULT_OUT_OF_MEMORY": "an allocation failed",
    "CY_RESULT_BUFFER_TOO_SMALL": "the buffer supplied was too small; ask for the size first",
    "CY_RESULT_IO": "an input or output operation failed",
    "CY_RESULT_INTERNAL": "the engine hit an internal invariant failure",
    "CY_RESULT_VERSION_MISMATCH": "the ABI versions of the caller and the engine do not match",
    "CY_RESULT_SCHEMA_TOO_NEW": "the data was written by a newer schema than this build knows",
    "CY_RESULT_SCHEMA_UNMIGRATABLE": "the data's schema cannot be migrated to this build's",
    "CY_RESULT_MODULE_LOAD_FAILED": "a native module could not be loaded",
}


class EmitError(Exception):
    """The description contains something this generator has no rule for."""


# Rust's keywords, which several ABI member names collide with — `CyVar::type` and
# `CyFieldDesc::type` both do. Escaped as raw identifiers rather than renamed, because the FFI
# mirror's field names ARE the C names: a Rust-side `kind` beside a C-side `type` is a second
# vocabulary, and the reader of a `#[repr(C)]` struct is exactly the person who needs the two to
# match. The four that cannot be raw identifiers — `self`, `Self`, `crate`, `super` — would be a
# generation error, which is the right outcome for a C ABI that used one as a member name.
_RUST_KEYWORDS = frozenset("""
as async await box break const continue dyn else enum extern false final fn for gen if impl in let
loop macro match mod move mut override priv pub ref return static struct trait true try type typeof
union unsafe unsized use virtual where while yield
""".split())

_UNRAWABLE = frozenset({"self", "Self", "crate", "super"})


def identifier(name: str) -> str:
    """A C member or parameter name as a Rust identifier, escaped when it is a keyword."""
    if name in _UNRAWABLE:
        raise EmitError(f"{name!r} cannot be spelled as a Rust identifier at all")
    return f"r#{name}" if name in _RUST_KEYWORDS else name


def _find(description: dict, kind: str, name: str) -> dict:
    for entry in description["types"]:
        if entry["kind"] == kind and entry["name"] == name:
            return entry
    raise EmitError(f"the ABI description has no {kind} named {name!r}")


def _enum_names(description: dict) -> frozenset[str]:
    return frozenset(item["name"] for item in description["types"] if item["kind"] == "enum")


def variant(constant: str, prefix: str) -> str:
    """`CY_RESULT_OUT_OF_RANGE` with prefix `CY_RESULT_` -> `OutOfRange`.

    Each underscore-separated word is capitalised and the rest lower-cased, which is what turns
    `VEC2` into `Vec2` and `I64` into `I64` without a table of exceptions.
    """
    if not constant.startswith(prefix):
        raise EmitError(f"{constant!r} does not start with the declared prefix {prefix!r}")
    tail = constant[len(prefix) :]
    if not tail:
        raise EmitError(f"{constant!r} is exactly its prefix, so it has no Rust name")
    return "".join(word[:1].upper() + word[1:].lower() for word in tail.split("_"))


def check_enum_coverage(description: dict) -> None:
    """Refuse a description whose enums this generator has not been taught to name."""
    described = {item["name"] for item in description["types"] if item["kind"] == "enum"}
    missing = sorted(described - set(ENUM_SPECS))
    if missing:
        raise EmitError(
            "the ABI has enums this generator has no Rust name for: " + ", ".join(missing)
            + "\n  Add a row to ENUM_SPECS in tools/gen/rust/sdk/emit.py giving the Rust name, the"
            "\n  constant prefix, and the integer type the ABI stores it in.")
    absent = sorted(set(ENUM_SPECS) - described)
    if absent:
        raise EmitError("ENUM_SPECS names enums the ABI does not have: " + ", ".join(absent))

    status = _find(description, "enum", "CyResult")
    unexplained = [value["name"] for value in status["values"] if value["name"] not in
                   STATUS_MESSAGES]
    if unexplained:
        raise EmitError(
            "CyResult has enumerators with no message: " + ", ".join(unexplained)
            + "\n  Add a row to STATUS_MESSAGES saying what a caller should understand by it. A"
            "\n  reason nobody wrote is a code with extra steps.")


# --- the files --------------------------------------------------------------------------------


def module(description: dict) -> str:
    """`generated/mod.rs`: the one place the generated file set is listed."""
    check_enum_coverage(description)
    return BANNER + """
//! Everything in this directory is produced from the C ABI description. The safe API that sits on
//! top of it is hand-written in the crate root's other modules, because the decisions it makes —
//! what a null handle means, which lifetime a borrowed string has, when a borrow must be
//! re-validated — are not in the C declarations and a generator that invented them would be a
//! second, unchecked copy of the ABI.

pub mod abi;
pub mod enums;
pub mod ffi;
pub mod interface;

#[cfg(test)]
mod layout;
"""


def abi_version(description: dict) -> str:
    """`generated/abi.rs`: the version the SDK was generated against, and the table's shape."""
    version = description["abi"]
    table = description["table"]
    count = len(entries.function_entries(description))
    return BANNER + f"""
//! The ABI this SDK was generated against.
//!
//! `MAJOR` and `MINOR` are what the SDK asks `cy_get_interface` for. The engine's rule, stated in
//! `cy_abi.h`, is that a request for a minor at or below the engine's own succeeds and returns the
//! engine's table — so an SDK generated against 1.{version["minor"]} works against any 1.x engine with
//! x >= {version["minor"]}, and is refused by an older one rather than reading a short table.
//!
//! `TABLE_SIZE` is the byte size of `CyInterface` as this SDK declares it. The engine reports its
//! own in `CyInterfaceHeader::table_size`, and a table SMALLER than this one is a runtime the SDK
//! must refuse: the entries past its end are not there. A LARGER one is fine and expected — that is
//! what an append looks like from the old side of it.

/// The ABI major version. A change here is a break, and the SDK will not talk to a different one.
pub const MAJOR: u32 = {version["major"]};

/// The ABI minor version. Appends increment it; the SDK works against this or any later minor.
pub const MINOR: u32 = {version["minor"]};

/// The ABI patch version, which carries no compatibility meaning and is reported for diagnostics.
pub const PATCH: u32 = {version["patch"]};

/// `size_of::<CyInterface>()` as this SDK declares it. Asserted against the compiler in `layout`.
pub const TABLE_SIZE: u32 = {table["size"]};

/// How many function-pointer entries `CyInterface` has, not counting its header.
pub const TABLE_ENTRY_COUNT: usize = {count};
"""


def _enum(description: dict, c_name: str) -> str:
    rust_name, prefix, repr_type = ENUM_SPECS[c_name]
    declaration = _find(description, "enum", c_name)
    lines = [
        f"/// `{c_name}`, as the ABI declares it.",
        "///",
        f"/// Stored as `{repr_type}` because that is what the ABI carries it in. `from_raw` is the only",
        "/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,",
        "/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an",
        "/// engine one minor version ahead is exactly how one arrives.",
        "#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]",
        f"#[repr({repr_type})]",
        f"pub enum {rust_name} {{",
    ]
    for value in declaration["values"]:
        lines.append(f"    /// `{value['name']}` = {value['value']}.")
        lines.append(f"    {variant(value['name'], prefix)} = {value['value']},")
    lines.append("}")
    lines.append("")
    lines.append(f"impl {rust_name} {{")
    lines.append("    /// Every value, in declaration order. Lets a caller enumerate without a range.")
    lines.append(f"    pub const ALL: [{rust_name}; {len(declaration['values'])}] = [")
    for value in declaration["values"]:
        lines.append(f"        {rust_name}::{variant(value['name'], prefix)},")
    lines.append("    ];")
    lines.append("")
    lines.append("    /// The value the ABI carries, or `None` when this build has no name for it.")
    lines.append("    #[must_use]")
    lines.append(f"    pub const fn from_raw(raw: {repr_type}) -> Option<Self> {{")
    lines.append("        match raw {")
    for value in declaration["values"]:
        lines.append(f"            {value['value']} => Some({rust_name}::{variant(value['name'], prefix)}),")
    lines.append("            _ => None,")
    lines.append("        }")
    lines.append("    }")
    lines.append("")
    lines.append("    /// The integer the ABI carries this value as.")
    lines.append("    #[must_use]")
    lines.append(f"    pub const fn as_raw(self) -> {repr_type} {{")
    lines.append(f"        self as {repr_type}")
    lines.append("    }")
    lines.append("")
    lines.append("    /// The C spelling, for diagnostics that have to be read beside the header.")
    lines.append("    #[must_use]")
    lines.append("    pub const fn c_name(self) -> &'static str {")
    lines.append("        match self {")
    for value in declaration["values"]:
        lines.append(f"            {rust_name}::{variant(value['name'], prefix)} => \"{value['name']}\",")
    lines.append("        }")
    lines.append("    }")
    lines.append("}")
    return "\n".join(lines)


def enums(description: dict) -> str:
    """`generated/enums.rs`: every ABI enum as a Rust enum that cannot hold an unknown value."""
    check_enum_coverage(description)
    parts = [BANNER, """
//! The ABI's enums.
//!
//! Two hand-written copies of these used to exist in the Swift overlay and one of them was wrong —
//! six enumerators against the engine's three, which put every `Log.info` on the wire as an error
//! on a green run. That is the failure this file exists to make impossible: there is one
//! declaration, in `cy_abi.h`, and every language's copy is produced from it.
"""]
    for c_name in ENUM_SPECS:
        parts.append(_enum(description, c_name))

    status = _find(description, "enum", "CyResult")
    lines = ["", "impl Status {",
             "    /// What a caller should understand by this status.",
             "    ///",
             "    /// `editor-rust-application` requires a failure crossing the boundary to arrive as \"a typed",
             "    /// error with a reason, not a null or a code to interpret\". This is the reason.",
             "    #[must_use]",
             "    pub const fn reason(self) -> &'static str {",
             "        match self {"]
    for value in status["values"]:
        name = variant(value["name"], "CY_RESULT_")
        lines.append(f"            Status::{name} => \"{STATUS_MESSAGES[value['name']]}\",")
    lines.extend(["        }", "    }", "",
                  "    /// Whether this status means the call did what it was asked.",
                  "    #[must_use]",
                  "    pub const fn is_ok(self) -> bool {",
                  "        matches!(self, Status::Ok)",
                  "    }",
                  "}", "",
                  "impl ::std::fmt::Display for Status {",
                  "    fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {",
                  "        write!(f, \"{} ({})\", self.reason(), self.c_name())",
                  "    }",
                  "}", "",
                  "impl ::std::error::Error for Status {}"])
    parts.append("\n".join(lines))
    return "\n".join(parts) + "\n"


def _record(declaration: dict, enum_names: frozenset[str],
            union_names: frozenset[str]) -> str:
    keyword = "union" if declaration["kind"] == "union" else "struct"
    # A union's members overlap, so Rust cannot derive Debug for it, and a struct containing one
    # inherits that. Clone and Copy are derivable for both and are all the FFI needs.
    derives = "#[derive(Clone, Copy)]" if _contains_union(declaration, union_names) else \
              "#[derive(Clone, Copy, Debug)]"
    lines = [f"/// `{declaration['name']}` — {declaration['size']} bytes, {declaration['alignment']}-byte aligned.",
             derives, "#[repr(C)]", f"pub {keyword} {declaration['name']} {{"]
    for item in declaration["members"]:
        lines.append(f"    /// `{item['type']}` at byte {item['offset']}.")
        lines.append(f"    pub {identifier(item['name'])}: "
                     f"{rusttypes.member(item['type'], enum_names)},")
    lines.append("}")
    return "\n".join(lines)


def _contains_union(declaration: dict, union_names: frozenset[str]) -> bool:
    """Whether Rust can derive `Debug` for this declaration — it cannot, through a union.

    Derived from the description's union set rather than from a list of names here, so that a second
    union appearing in the ABI is handled rather than producing a struct that does not compile.
    """
    if declaration["kind"] == "union":
        return True
    return any(item["type"].strip() in union_names for item in declaration.get("members", ()))


def ffi(description: dict) -> str:
    """`generated/ffi.rs`: the `#[repr(C)]` mirror of every ABI declaration."""
    enum_names = _enum_names(description)
    union_names = frozenset(item["name"] for item in description["types"]
                            if item["kind"] == "union")

    parts = [BANNER, """
//! The C ABI as Rust sees it: `#[repr(C)]` mirrors, opaque handle tags, and the interface table.
//!
//! Nothing here is safe to use directly and nothing here is meant to be. The crate's `interface`
//! module wraps every entry, and the crate root wraps that in an API with no raw pointers in it.
//!
//! WHY THE MIRRORS ARE GENERATED HERE RATHER THAN BOUND BY bindgen. bindgen would need libclang on
//! every machine that builds the editor, would produce a different file for each host it ran on,
//! and would put a second reader of `cy_abi.h` in the repository. There is exactly one reader —
//! `tools/abi/abi_describe.py` — and the ABI gate, the committed baseline, the Swift overlay and
//! this file all come from its single parse. The layout it computes is asserted against the C
//! compiler by `src/abi/tests/test_layout.cpp` and against `rustc` by this crate's `layout` module.

#![allow(non_camel_case_types)]
"""]

    for declaration in description["types"]:
        kind = declaration["kind"]
        if kind == "handle":
            tag = declaration["tag"]
            parts.append(f"""/// The opaque type behind `{declaration['name']}`.
///
/// A zero-sized member and no constructor: this type exists to give the pointer a name the compiler
/// can distinguish from every other handle's, so that passing a `CyWorld` where a `CyEngine` is
/// wanted does not compile. Nothing in Rust ever holds one of these by value.
#[repr(C)]
pub struct {tag} {{
    _private: [u8; 0],
}}

/// `{declaration['name']}` — an opaque handle. Null is the ABI's "no such thing".
pub type {declaration['name']} = *mut {tag};""")
        elif kind == "alias":
            parts.append(f"/// `{declaration['name']}`, the ABI's alias for `{declaration['underlying']}`.\n"
                         f"pub type {declaration['name']} = {rusttypes.ffi(declaration['underlying'], enum_names)};")
        elif kind in ("struct", "union"):
            parts.append(_record(declaration, enum_names, union_names))
        elif kind == "function_pointer":
            rendered = ", ".join(rusttypes.ffi(item, enum_names)
                                 for item in declaration["parameters"])
            returns = declaration["returns"]
            suffix = "" if returns == "void" else f" -> {rusttypes.ffi(returns, enum_names)}"
            parts.append(f"/// `{declaration['name']}`, an entry point the engine looks up in a module image.\n"
                         f"pub type {declaration['name']} = Option<unsafe extern \"C\" fn({rendered}){suffix}>;")
        elif kind == "enum":
            continue  # enums.rs owns these; the FFI carries their integer.
        else:
            raise EmitError(f"no Rust rule for a description entry of kind {kind!r}")

    parts.append(_empty_table(description))
    return "\n\n".join(parts) + "\n"


def _empty_table(description: dict) -> str:
    """`CyInterface::EMPTY`: every entry absent, for a test host that fills in what it implements.

    Generated rather than hand-written because it has one field per table entry, and a hand-written
    one would be the first thing to go stale on an append — silently, since a missing initialiser
    for a field added later is a compile error only if the struct is not `..Default::default()`-ed.
    """
    header = _find(description, "struct", "CyInterfaceHeader")
    non_scalar = [item["name"] for item in header["members"] if item["type"] != "uint32_t"]
    if non_scalar:
        raise EmitError("CyInterfaceHeader has non-uint32_t members "
                        f"({', '.join(non_scalar)}); CyInterface::EMPTY needs a rule for them")
    table = _find(description, "struct", "CyInterface")
    lines = ["impl CyInterface {",
             "    /// A table with a zeroed header and no entries at all.",
             "    ///",
             "    /// The starting point for a host that implements a subset — a test double, or a",
             "    /// runtime older than this SDK. Every absent entry is a `None`, which the SDK",
             "    /// reports by name rather than jumping to zero.",
             "    pub const EMPTY: CyInterface = CyInterface {",
             "        header: CyInterfaceHeader {"]
    for item in header["members"]:
        lines.append(f"            {identifier(item['name'])}: 0,")
    lines.append("        },")
    for item in table["members"]:
        if item["name"] == "header":
            continue
        lines.append(f"        {identifier(item['name'])}: None,")
    lines.extend(["    };", "}"])
    return "\n".join(lines)


def _call(entry: dict, record: entries.Entry, enum_names: frozenset[str]) -> str:
    returns, parameters = entries.signature(entry)
    typed = [f"{identifier(name)}: {rusttypes.ffi(spelling, enum_names, 'ffi::')}"
             for name, spelling in zip(record.parameters, parameters)]
    arguments = ", ".join(identifier(name) for name in record.parameters)
    signature_text = ", ".join(["&self", *typed])
    name = entry["name"]

    if record.result == "fallible":
        body = (f"        let raw = unsafe {{ entry({arguments}) }};\n"
                "        match Status::from_raw(raw) {\n"
                "            Some(Status::Ok) => Ok(()),\n"
                "            Some(status) => Err(CallError::Failed(status)),\n"
                "            None => Err(CallError::UnknownStatus(raw)),\n"
                "        }")
        result_type = "()"
    elif returns == "void":
        body = f"        unsafe {{ entry({arguments}) }};\n        Ok(())"
        result_type = "()"
    else:
        body = f"        Ok(unsafe {{ entry({arguments}) }})"
        result_type = rusttypes.ffi(returns, enum_names, "ffi::")

    return f"""    /// {record.doc}
    ///
    /// # Safety
    ///
    /// The handles and pointers are the ABI's, and the ABI's rules apply unchanged: a handle must be
    /// live, a pointer must point at what its type says for the duration of the call, and a
    /// `CyBorrow` must be re-validated against the world's epoch before it is read. The safe API in
    /// the crate root is what discharges these; nothing outside the SDK calls this directly.
    pub unsafe fn {name}({signature_text}) -> Result<{result_type}, CallError> {{
        let entry = self.table().{name}.ok_or(CallError::Missing("{name}"))?;
{body}
    }}"""


def interface(description: dict) -> str:
    """`generated/interface.rs`: one typed call per table entry, over a validated table pointer."""
    entries.validate(description)
    enum_names = _enum_names(description)
    described = entries.function_entries(description)

    parts = [BANNER, """
//! One method per `CyInterface` entry, generated so that the set cannot fall behind the table.
//!
//! Each method resolves its entry, reports a `Missing` when the runtime's table does not have it,
//! and converts a `CyResult` into a `Status`. That is all it does: no allocation, no string
//! conversion, no null checking of the caller's arguments. The safe API in the crate root is where
//! those decisions live, because they are decisions and this file contains none.

use super::enums::Status;
use super::ffi;

/// Why a call through the table did not produce a value.
///
/// Three cases, and the difference between them matters to a caller. `Missing` is a runtime older
/// than this SDK — the entry is not in its table at all, so nothing can be done but report which
/// one. `Failed` is the engine answering with a reason. `UnknownStatus` is an engine one minor
/// version AHEAD, returning a `CyResult` this build has no name for; it is kept distinct rather
/// than folded into `Failed(Unknown)` because the two call for different actions.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CallError {
    /// The runtime's table has no entry of this name.
    Missing(&'static str),
    /// The engine reported a failure.
    Failed(Status),
    /// The engine returned a `CyResult` this build does not know.
    UnknownStatus(i32),
}

impl ::std::fmt::Display for CallError {
    fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
        match self {
            CallError::Missing(name) => {
                write!(f, "the runtime's interface table has no `{name}` entry")
            }
            CallError::Failed(status) => write!(f, "{status}"),
            CallError::UnknownStatus(raw) => {
                write!(f, "the engine returned CyResult {raw}, which this build has no name for")
            }
        }
    }
}

impl ::std::error::Error for CallError {}

/// A validated pointer to a runtime's `CyInterface`.
///
/// Constructed only by the crate's `host` module, which is where the header is checked. Holding one
/// is the claim that the table is live and at least as large as `abi::TABLE_SIZE`; every method
/// below relies on that claim and on nothing else.
#[derive(Clone, Copy)]
pub struct Interface {
    table: *const ffi::CyInterface,
}

// SAFETY: the table is immutable for the lifetime of the runtime that published it — `cy_abi.h`
// requires `cy_get_interface` to return a table with static storage duration — so sharing the
// pointer across threads reads the same constant bytes from every one of them. The engine state the
// entries reach is a different question, and one the SDK's session types answer: they are what
// confine calls to the thread that owns the connection.
unsafe impl Send for Interface {}
unsafe impl Sync for Interface {}

impl Interface {
    /// Wrap a table pointer that has already been validated.
    ///
    /// # Safety
    ///
    /// `table` must be non-null, must point at a live `CyInterface` for as long as this value is
    /// used, and its `header.table_size` must be at least `abi::TABLE_SIZE`. `host::Runtime` is the
    /// only caller in this crate and it checks all three.
    #[must_use]
    pub const unsafe fn from_raw(table: *const ffi::CyInterface) -> Self {
        Self { table }
    }

    /// The table this interface wraps, for the crate's own layout and version checks.
    #[must_use]
    pub fn table(&self) -> &ffi::CyInterface {
        // SAFETY: the constructor's contract is that the pointer is non-null and outlives us.
        unsafe { &*self.table }
    }

    /// The header the runtime published: its ABI version and the size of its table.
    #[must_use]
    pub fn header(&self) -> ffi::CyInterfaceHeader {
        self.table().header
    }
"""]
    parts.append("\n\n".join(_call(entry, entries.ENTRIES[entry["name"]], enum_names)
                             for entry in described))
    parts.append("}\n")
    return "\n".join(parts)


def _layout_assertions(declaration: dict) -> list[str]:
    name = declaration["name"]
    lines = [
        f"const _: () = assert!(size_of::<ffi::{name}>() == {declaration['size']},",
        f"    \"{name} is not {declaration['size']} bytes; the ABI description and rustc disagree\");",
        f"const _: () = assert!(align_of::<ffi::{name}>() == {declaration['alignment']},",
        f"    \"{name} is not {declaration['alignment']}-byte aligned\");",
    ]
    if declaration["kind"] == "union":
        # Every member of a union is at offset zero by definition, so there is nothing to assert
        # that the size and alignment above do not already say.
        return lines
    for item in declaration["members"]:
        lines.append(f"const _: () = assert!("
                     f"offset_of!(ffi::{name}, {identifier(item['name'])}) == {item['offset']},")
        lines.append(f"    \"{name}::{item['name']} is not at byte {item['offset']}\");")
    return lines


def layout(description: dict) -> str:
    """`generated/layout.rs`: the compiler's own layout, asserted against the description's.

    These are `const` assertions rather than `#[test]`s deliberately. `editor-rust-application`
    requires that when the ABI changes "the SDK SHALL be regenerated or updated and the mismatch
    SHALL be a build failure" — a test would report the mismatch after producing a binary that had
    already read the struct wrongly. A `const _: () = assert!(..)` fails the compile.
    """
    lines = [BANNER, """
//! The layout `tools/abi/abi_describe.py` computed, asserted against what `rustc` produced.
//!
//! `src/abi/tests/test_layout.cpp` asserts the same numbers against the C compiler. Between them,
//! the description is checked from both sides of the boundary it describes: if the layout model is
//! ever wrong on a platform, one of the two fails there rather than the description quietly
//! describing a struct that does not exist.
//!
//! Compiled only under `cfg(test)` — they cost nothing at run time either way, but a build failure
//! in a dependency's release build is a worse way to learn this than a failing `cargo test`.

use std::mem::{align_of, offset_of, size_of};

use super::ffi;
"""]
    for declaration in description["types"]:
        if declaration["kind"] not in ("struct", "union"):
            continue
        lines.append("\n".join(_layout_assertions(declaration)))

    count = len(entries.function_entries(description))
    lines.append(f"""#[test]
fn the_table_has_every_entry_the_description_declares() {{
    // A count rather than a name check: the fields ARE the names, so a missing one is a compile
    // error above. What a count catches is the other direction — a hand edit that added a field to
    // the generated table without the description having one, which would move every entry after
    // it and be invisible to a compiler that only sees Rust.
    assert_eq!(
        (size_of::<ffi::CyInterface>() - size_of::<ffi::CyInterfaceHeader>())
            / size_of::<usize>(),
        {count},
        "CyInterface has a different number of function-pointer entries than the ABI description"
    );
}}""")
    return "\n\n".join(lines) + "\n"
