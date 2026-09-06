// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.

//! The ABI's enums.
//!
//! Two hand-written copies of these used to exist in the Swift overlay and one of them was wrong —
//! six enumerators against the engine's three, which put every `Log.info` on the wire as an error
//! on a green run. That is the failure this file exists to make impossible: there is one
//! declaration, in `cy_abi.h`, and every language's copy is produced from it.

/// `CyResult`, as the ABI declares it.
///
/// Stored as `i32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(i32)]
pub enum Status {
    /// `CY_RESULT_OK` = 0.
    Ok = 0,
    /// `CY_RESULT_UNKNOWN` = 1.
    Unknown = 1,
    /// `CY_RESULT_INVALID_ARGUMENT` = 2.
    InvalidArgument = 2,
    /// `CY_RESULT_OUT_OF_RANGE` = 3.
    OutOfRange = 3,
    /// `CY_RESULT_NOT_FOUND` = 4.
    NotFound = 4,
    /// `CY_RESULT_ALREADY_EXISTS` = 5.
    AlreadyExists = 5,
    /// `CY_RESULT_PERMISSION_DENIED` = 6.
    PermissionDenied = 6,
    /// `CY_RESULT_UNSUPPORTED` = 7.
    Unsupported = 7,
    /// `CY_RESULT_NOT_IMPLEMENTED` = 8.
    NotImplemented = 8,
    /// `CY_RESULT_UNAVAILABLE` = 9.
    Unavailable = 9,
    /// `CY_RESULT_TIMEOUT` = 10.
    Timeout = 10,
    /// `CY_RESULT_OUT_OF_MEMORY` = 11.
    OutOfMemory = 11,
    /// `CY_RESULT_BUFFER_TOO_SMALL` = 12.
    BufferTooSmall = 12,
    /// `CY_RESULT_IO` = 13.
    Io = 13,
    /// `CY_RESULT_INTERNAL` = 14.
    Internal = 14,
    /// `CY_RESULT_VERSION_MISMATCH` = 100.
    VersionMismatch = 100,
    /// `CY_RESULT_SCHEMA_TOO_NEW` = 101.
    SchemaTooNew = 101,
    /// `CY_RESULT_SCHEMA_UNMIGRATABLE` = 102.
    SchemaUnmigratable = 102,
    /// `CY_RESULT_MODULE_LOAD_FAILED` = 103.
    ModuleLoadFailed = 103,
}

impl Status {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [Status; 19] = [
        Status::Ok,
        Status::Unknown,
        Status::InvalidArgument,
        Status::OutOfRange,
        Status::NotFound,
        Status::AlreadyExists,
        Status::PermissionDenied,
        Status::Unsupported,
        Status::NotImplemented,
        Status::Unavailable,
        Status::Timeout,
        Status::OutOfMemory,
        Status::BufferTooSmall,
        Status::Io,
        Status::Internal,
        Status::VersionMismatch,
        Status::SchemaTooNew,
        Status::SchemaUnmigratable,
        Status::ModuleLoadFailed,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: i32) -> Option<Self> {
        match raw {
            0 => Some(Status::Ok),
            1 => Some(Status::Unknown),
            2 => Some(Status::InvalidArgument),
            3 => Some(Status::OutOfRange),
            4 => Some(Status::NotFound),
            5 => Some(Status::AlreadyExists),
            6 => Some(Status::PermissionDenied),
            7 => Some(Status::Unsupported),
            8 => Some(Status::NotImplemented),
            9 => Some(Status::Unavailable),
            10 => Some(Status::Timeout),
            11 => Some(Status::OutOfMemory),
            12 => Some(Status::BufferTooSmall),
            13 => Some(Status::Io),
            14 => Some(Status::Internal),
            100 => Some(Status::VersionMismatch),
            101 => Some(Status::SchemaTooNew),
            102 => Some(Status::SchemaUnmigratable),
            103 => Some(Status::ModuleLoadFailed),
            _ => None,
        }
    }

    /// The integer the ABI carries this value as.
    #[must_use]
    pub const fn as_raw(self) -> i32 {
        self as i32
    }

    /// The C spelling, for diagnostics that have to be read beside the header.
    #[must_use]
    pub const fn c_name(self) -> &'static str {
        match self {
            Status::Ok => "CY_RESULT_OK",
            Status::Unknown => "CY_RESULT_UNKNOWN",
            Status::InvalidArgument => "CY_RESULT_INVALID_ARGUMENT",
            Status::OutOfRange => "CY_RESULT_OUT_OF_RANGE",
            Status::NotFound => "CY_RESULT_NOT_FOUND",
            Status::AlreadyExists => "CY_RESULT_ALREADY_EXISTS",
            Status::PermissionDenied => "CY_RESULT_PERMISSION_DENIED",
            Status::Unsupported => "CY_RESULT_UNSUPPORTED",
            Status::NotImplemented => "CY_RESULT_NOT_IMPLEMENTED",
            Status::Unavailable => "CY_RESULT_UNAVAILABLE",
            Status::Timeout => "CY_RESULT_TIMEOUT",
            Status::OutOfMemory => "CY_RESULT_OUT_OF_MEMORY",
            Status::BufferTooSmall => "CY_RESULT_BUFFER_TOO_SMALL",
            Status::Io => "CY_RESULT_IO",
            Status::Internal => "CY_RESULT_INTERNAL",
            Status::VersionMismatch => "CY_RESULT_VERSION_MISMATCH",
            Status::SchemaTooNew => "CY_RESULT_SCHEMA_TOO_NEW",
            Status::SchemaUnmigratable => "CY_RESULT_SCHEMA_UNMIGRATABLE",
            Status::ModuleLoadFailed => "CY_RESULT_MODULE_LOAD_FAILED",
        }
    }
}
/// `CySeverity`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum Severity {
    /// `CY_SEVERITY_INFO` = 0.
    Info = 0,
    /// `CY_SEVERITY_WARNING` = 1.
    Warning = 1,
    /// `CY_SEVERITY_ERROR` = 2.
    Error = 2,
}

impl Severity {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [Severity; 3] = [Severity::Info, Severity::Warning, Severity::Error];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Severity::Info),
            1 => Some(Severity::Warning),
            2 => Some(Severity::Error),
            _ => None,
        }
    }

    /// The integer the ABI carries this value as.
    #[must_use]
    pub const fn as_raw(self) -> u32 {
        self as u32
    }

    /// The C spelling, for diagnostics that have to be read beside the header.
    #[must_use]
    pub const fn c_name(self) -> &'static str {
        match self {
            Severity::Info => "CY_SEVERITY_INFO",
            Severity::Warning => "CY_SEVERITY_WARNING",
            Severity::Error => "CY_SEVERITY_ERROR",
        }
    }
}
/// `CyVarType`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum VarType {
    /// `CY_VAR_NIL` = 0.
    Nil = 0,
    /// `CY_VAR_BOOL` = 1.
    Bool = 1,
    /// `CY_VAR_I64` = 2.
    I64 = 2,
    /// `CY_VAR_F32` = 3.
    F32 = 3,
    /// `CY_VAR_F64` = 4.
    F64 = 4,
    /// `CY_VAR_VEC2` = 5.
    Vec2 = 5,
    /// `CY_VAR_VEC3` = 6.
    Vec3 = 6,
    /// `CY_VAR_VEC4` = 7.
    Vec4 = 7,
    /// `CY_VAR_QUAT` = 8.
    Quat = 8,
    /// `CY_VAR_STRING` = 9.
    String = 9,
    /// `CY_VAR_BYTES` = 10.
    Bytes = 10,
    /// `CY_VAR_ENTITY` = 11.
    Entity = 11,
    /// `CY_VAR_I8` = 12.
    I8 = 12,
    /// `CY_VAR_I16` = 13.
    I16 = 13,
    /// `CY_VAR_I32` = 14.
    I32 = 14,
    /// `CY_VAR_U8` = 15.
    U8 = 15,
    /// `CY_VAR_U16` = 16.
    U16 = 16,
    /// `CY_VAR_U32` = 17.
    U32 = 17,
    /// `CY_VAR_U64` = 18.
    U64 = 18,
}

impl VarType {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [VarType; 19] = [
        VarType::Nil,
        VarType::Bool,
        VarType::I64,
        VarType::F32,
        VarType::F64,
        VarType::Vec2,
        VarType::Vec3,
        VarType::Vec4,
        VarType::Quat,
        VarType::String,
        VarType::Bytes,
        VarType::Entity,
        VarType::I8,
        VarType::I16,
        VarType::I32,
        VarType::U8,
        VarType::U16,
        VarType::U32,
        VarType::U64,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(VarType::Nil),
            1 => Some(VarType::Bool),
            2 => Some(VarType::I64),
            3 => Some(VarType::F32),
            4 => Some(VarType::F64),
            5 => Some(VarType::Vec2),
            6 => Some(VarType::Vec3),
            7 => Some(VarType::Vec4),
            8 => Some(VarType::Quat),
            9 => Some(VarType::String),
            10 => Some(VarType::Bytes),
            11 => Some(VarType::Entity),
            12 => Some(VarType::I8),
            13 => Some(VarType::I16),
            14 => Some(VarType::I32),
            15 => Some(VarType::U8),
            16 => Some(VarType::U16),
            17 => Some(VarType::U32),
            18 => Some(VarType::U64),
            _ => None,
        }
    }

    /// The integer the ABI carries this value as.
    #[must_use]
    pub const fn as_raw(self) -> u32 {
        self as u32
    }

    /// The C spelling, for diagnostics that have to be read beside the header.
    #[must_use]
    pub const fn c_name(self) -> &'static str {
        match self {
            VarType::Nil => "CY_VAR_NIL",
            VarType::Bool => "CY_VAR_BOOL",
            VarType::I64 => "CY_VAR_I64",
            VarType::F32 => "CY_VAR_F32",
            VarType::F64 => "CY_VAR_F64",
            VarType::Vec2 => "CY_VAR_VEC2",
            VarType::Vec3 => "CY_VAR_VEC3",
            VarType::Vec4 => "CY_VAR_VEC4",
            VarType::Quat => "CY_VAR_QUAT",
            VarType::String => "CY_VAR_STRING",
            VarType::Bytes => "CY_VAR_BYTES",
            VarType::Entity => "CY_VAR_ENTITY",
            VarType::I8 => "CY_VAR_I8",
            VarType::I16 => "CY_VAR_I16",
            VarType::I32 => "CY_VAR_I32",
            VarType::U8 => "CY_VAR_U8",
            VarType::U16 => "CY_VAR_U16",
            VarType::U32 => "CY_VAR_U32",
            VarType::U64 => "CY_VAR_U64",
        }
    }
}
/// `CyInitLevel`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum InitLevel {
    /// `CY_INIT_LEVEL_CORE` = 0.
    Core = 0,
    /// `CY_INIT_LEVEL_SERVERS` = 1.
    Servers = 1,
    /// `CY_INIT_LEVEL_SCENE` = 2.
    Scene = 2,
    /// `CY_INIT_LEVEL_EDITOR` = 3.
    Editor = 3,
}

impl InitLevel {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [InitLevel; 4] = [
        InitLevel::Core,
        InitLevel::Servers,
        InitLevel::Scene,
        InitLevel::Editor,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(InitLevel::Core),
            1 => Some(InitLevel::Servers),
            2 => Some(InitLevel::Scene),
            3 => Some(InitLevel::Editor),
            _ => None,
        }
    }

    /// The integer the ABI carries this value as.
    #[must_use]
    pub const fn as_raw(self) -> u32 {
        self as u32
    }

    /// The C spelling, for diagnostics that have to be read beside the header.
    #[must_use]
    pub const fn c_name(self) -> &'static str {
        match self {
            InitLevel::Core => "CY_INIT_LEVEL_CORE",
            InitLevel::Servers => "CY_INIT_LEVEL_SERVERS",
            InitLevel::Scene => "CY_INIT_LEVEL_SCENE",
            InitLevel::Editor => "CY_INIT_LEVEL_EDITOR",
        }
    }
}
/// `CyStage`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum Stage {
    /// `CY_STAGE_PRE_SIMULATION` = 0.
    PreSimulation = 0,
    /// `CY_STAGE_PHYSICS` = 1.
    Physics = 1,
    /// `CY_STAGE_SIMULATION` = 2.
    Simulation = 2,
    /// `CY_STAGE_POST_SIMULATION` = 3.
    PostSimulation = 3,
    /// `CY_STAGE_FRAME` = 4.
    Frame = 4,
    /// `CY_STAGE_ANIMATION` = 5.
    Animation = 5,
    /// `CY_STAGE_UI` = 6.
    Ui = 6,
    /// `CY_STAGE_RENDER` = 7.
    Render = 7,
}

impl Stage {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [Stage; 8] = [
        Stage::PreSimulation,
        Stage::Physics,
        Stage::Simulation,
        Stage::PostSimulation,
        Stage::Frame,
        Stage::Animation,
        Stage::Ui,
        Stage::Render,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Stage::PreSimulation),
            1 => Some(Stage::Physics),
            2 => Some(Stage::Simulation),
            3 => Some(Stage::PostSimulation),
            4 => Some(Stage::Frame),
            5 => Some(Stage::Animation),
            6 => Some(Stage::Ui),
            7 => Some(Stage::Render),
            _ => None,
        }
    }

    /// The integer the ABI carries this value as.
    #[must_use]
    pub const fn as_raw(self) -> u32 {
        self as u32
    }

    /// The C spelling, for diagnostics that have to be read beside the header.
    #[must_use]
    pub const fn c_name(self) -> &'static str {
        match self {
            Stage::PreSimulation => "CY_STAGE_PRE_SIMULATION",
            Stage::Physics => "CY_STAGE_PHYSICS",
            Stage::Simulation => "CY_STAGE_SIMULATION",
            Stage::PostSimulation => "CY_STAGE_POST_SIMULATION",
            Stage::Frame => "CY_STAGE_FRAME",
            Stage::Animation => "CY_STAGE_ANIMATION",
            Stage::Ui => "CY_STAGE_UI",
            Stage::Render => "CY_STAGE_RENDER",
        }
    }
}

impl Status {
    /// What a caller should understand by this status.
    ///
    /// `editor-rust-application` requires a failure crossing the boundary to arrive as "a typed
    /// error with a reason, not a null or a code to interpret". This is the reason.
    #[must_use]
    pub const fn reason(self) -> &'static str {
        match self {
            Status::Ok => "the call succeeded",
            Status::Unknown => "the engine reported a failure it did not classify",
            Status::InvalidArgument => "an argument was not valid for this call",
            Status::OutOfRange => "a value was outside the range this call accepts",
            Status::NotFound => "the named thing does not exist",
            Status::AlreadyExists => "the thing being created is already there",
            Status::PermissionDenied => "the caller is not allowed to do this",
            Status::Unsupported => "this build of the engine does not support the operation",
            Status::NotImplemented => "the operation is declared but not implemented yet",
            Status::Unavailable => "the subsystem is not available right now",
            Status::Timeout => "the operation did not complete in the time allowed",
            Status::OutOfMemory => "an allocation failed",
            Status::BufferTooSmall => "the buffer supplied was too small; ask for the size first",
            Status::Io => "an input or output operation failed",
            Status::Internal => "the engine hit an internal invariant failure",
            Status::VersionMismatch => "the ABI versions of the caller and the engine do not match",
            Status::SchemaTooNew => "the data was written by a newer schema than this build knows",
            Status::SchemaUnmigratable => "the data's schema cannot be migrated to this build's",
            Status::ModuleLoadFailed => "a native module could not be loaded",
        }
    }

    /// Whether this status means the call did what it was asked.
    #[must_use]
    pub const fn is_ok(self) -> bool {
        matches!(self, Status::Ok)
    }
}

impl ::std::fmt::Display for Status {
    fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
        write!(f, "{} ({})", self.reason(), self.c_name())
    }
}

impl ::std::error::Error for Status {}
