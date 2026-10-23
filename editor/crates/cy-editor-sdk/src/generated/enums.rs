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
    /// `CY_VAR_FIXED` = 19.
    Fixed = 19,
}

impl VarType {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [VarType; 20] = [
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
        VarType::Fixed,
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
            19 => Some(VarType::Fixed),
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
            VarType::Fixed => "CY_VAR_FIXED",
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
/// `CyServiceEventKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum ServiceEventKind {
    /// `CY_SERVICE_EVENT_ACCEPTED` = 0.
    Accepted = 0,
    /// `CY_SERVICE_EVENT_PROGRESS` = 1.
    Progress = 1,
    /// `CY_SERVICE_EVENT_COMPLETED` = 2.
    Completed = 2,
    /// `CY_SERVICE_EVENT_FAILED` = 3.
    Failed = 3,
    /// `CY_SERVICE_EVENT_CANCELLED` = 4.
    Cancelled = 4,
}

impl ServiceEventKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [ServiceEventKind; 5] = [
        ServiceEventKind::Accepted,
        ServiceEventKind::Progress,
        ServiceEventKind::Completed,
        ServiceEventKind::Failed,
        ServiceEventKind::Cancelled,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(ServiceEventKind::Accepted),
            1 => Some(ServiceEventKind::Progress),
            2 => Some(ServiceEventKind::Completed),
            3 => Some(ServiceEventKind::Failed),
            4 => Some(ServiceEventKind::Cancelled),
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
            ServiceEventKind::Accepted => "CY_SERVICE_EVENT_ACCEPTED",
            ServiceEventKind::Progress => "CY_SERVICE_EVENT_PROGRESS",
            ServiceEventKind::Completed => "CY_SERVICE_EVENT_COMPLETED",
            ServiceEventKind::Failed => "CY_SERVICE_EVENT_FAILED",
            ServiceEventKind::Cancelled => "CY_SERVICE_EVENT_CANCELLED",
        }
    }
}
/// `CyPhase`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum Phase {
    /// `CY_PHASE_NONE` = 0.
    None = 0,
    /// `CY_PHASE_FIXED_UPDATE` = 1.
    FixedUpdate = 1,
    /// `CY_PHASE_FRAME_UPDATE` = 2.
    FrameUpdate = 2,
}

impl Phase {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [Phase; 3] = [Phase::None, Phase::FixedUpdate, Phase::FrameUpdate];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Phase::None),
            1 => Some(Phase::FixedUpdate),
            2 => Some(Phase::FrameUpdate),
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
            Phase::None => "CY_PHASE_NONE",
            Phase::FixedUpdate => "CY_PHASE_FIXED_UPDATE",
            Phase::FrameUpdate => "CY_PHASE_FRAME_UPDATE",
        }
    }
}
/// `CyShapeKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum ShapeKind {
    /// `CY_SHAPE_SPHERE` = 0.
    Sphere = 0,
    /// `CY_SHAPE_CAPSULE` = 1.
    Capsule = 1,
    /// `CY_SHAPE_BOX` = 2.
    Box = 2,
}

impl ShapeKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [ShapeKind; 3] = [ShapeKind::Sphere, ShapeKind::Capsule, ShapeKind::Box];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(ShapeKind::Sphere),
            1 => Some(ShapeKind::Capsule),
            2 => Some(ShapeKind::Box),
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
            ShapeKind::Sphere => "CY_SHAPE_SPHERE",
            ShapeKind::Capsule => "CY_SHAPE_CAPSULE",
            ShapeKind::Box => "CY_SHAPE_BOX",
        }
    }
}
/// `CyNavPathStatus`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum NavPathStatus {
    /// `CY_NAV_PATH_STATUS_IDLE` = 0.
    Idle = 0,
    /// `CY_NAV_PATH_STATUS_COMPUTING` = 1.
    Computing = 1,
    /// `CY_NAV_PATH_STATUS_FOLLOWING` = 2.
    Following = 2,
    /// `CY_NAV_PATH_STATUS_ARRIVED` = 3.
    Arrived = 3,
    /// `CY_NAV_PATH_STATUS_FAILED` = 4.
    Failed = 4,
}

impl NavPathStatus {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [NavPathStatus; 5] = [
        NavPathStatus::Idle,
        NavPathStatus::Computing,
        NavPathStatus::Following,
        NavPathStatus::Arrived,
        NavPathStatus::Failed,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(NavPathStatus::Idle),
            1 => Some(NavPathStatus::Computing),
            2 => Some(NavPathStatus::Following),
            3 => Some(NavPathStatus::Arrived),
            4 => Some(NavPathStatus::Failed),
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
            NavPathStatus::Idle => "CY_NAV_PATH_STATUS_IDLE",
            NavPathStatus::Computing => "CY_NAV_PATH_STATUS_COMPUTING",
            NavPathStatus::Following => "CY_NAV_PATH_STATUS_FOLLOWING",
            NavPathStatus::Arrived => "CY_NAV_PATH_STATUS_ARRIVED",
            NavPathStatus::Failed => "CY_NAV_PATH_STATUS_FAILED",
        }
    }
}
/// `CyNavQueryState`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum NavQueryState {
    /// `CY_NAV_QUERY_PENDING` = 0.
    Pending = 0,
    /// `CY_NAV_QUERY_READY` = 1.
    Ready = 1,
    /// `CY_NAV_QUERY_CONSUMED` = 2.
    Consumed = 2,
    /// `CY_NAV_QUERY_CANCELLED` = 3.
    Cancelled = 3,
}

impl NavQueryState {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [NavQueryState; 4] = [
        NavQueryState::Pending,
        NavQueryState::Ready,
        NavQueryState::Consumed,
        NavQueryState::Cancelled,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(NavQueryState::Pending),
            1 => Some(NavQueryState::Ready),
            2 => Some(NavQueryState::Consumed),
            3 => Some(NavQueryState::Cancelled),
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
            NavQueryState::Pending => "CY_NAV_QUERY_PENDING",
            NavQueryState::Ready => "CY_NAV_QUERY_READY",
            NavQueryState::Consumed => "CY_NAV_QUERY_CONSUMED",
            NavQueryState::Cancelled => "CY_NAV_QUERY_CANCELLED",
        }
    }
}
/// `CyAccessMode`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum AccessMode {
    /// `CY_ACCESS_READ` = 0.
    Read = 0,
    /// `CY_ACCESS_WRITE` = 1.
    Write = 1,
    /// `CY_ACCESS_EXCLUDE` = 2.
    Exclude = 2,
}

impl AccessMode {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [AccessMode; 3] = [AccessMode::Read, AccessMode::Write, AccessMode::Exclude];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(AccessMode::Read),
            1 => Some(AccessMode::Write),
            2 => Some(AccessMode::Exclude),
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
            AccessMode::Read => "CY_ACCESS_READ",
            AccessMode::Write => "CY_ACCESS_WRITE",
            AccessMode::Exclude => "CY_ACCESS_EXCLUDE",
        }
    }
}
/// `CyGroundState`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GroundState {
    /// `CY_GROUND_GROUNDED` = 0.
    Grounded = 0,
    /// `CY_GROUND_STEEP_SLOPE` = 1.
    SteepSlope = 1,
    /// `CY_GROUND_IN_AIR` = 2.
    InAir = 2,
}

impl GroundState {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GroundState; 3] = [
        GroundState::Grounded,
        GroundState::SteepSlope,
        GroundState::InAir,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GroundState::Grounded),
            1 => Some(GroundState::SteepSlope),
            2 => Some(GroundState::InAir),
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
            GroundState::Grounded => "CY_GROUND_GROUNDED",
            GroundState::SteepSlope => "CY_GROUND_STEEP_SLOPE",
            GroundState::InAir => "CY_GROUND_IN_AIR",
        }
    }
}
/// `CyUiEventKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiEventKind {
    /// `CY_UI_EVENT_CLICK` = 0.
    Click = 0,
    /// `CY_UI_EVENT_FOCUS` = 1.
    Focus = 1,
    /// `CY_UI_EVENT_BLUR` = 2.
    Blur = 2,
    /// `CY_UI_EVENT_CHANGED` = 3.
    Changed = 3,
    /// `CY_UI_EVENT_SUBMITTED` = 4.
    Submitted = 4,
    /// `CY_UI_EVENT_OPENED` = 5.
    Opened = 5,
    /// `CY_UI_EVENT_CLOSED` = 6.
    Closed = 6,
}

impl UiEventKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiEventKind; 7] = [
        UiEventKind::Click,
        UiEventKind::Focus,
        UiEventKind::Blur,
        UiEventKind::Changed,
        UiEventKind::Submitted,
        UiEventKind::Opened,
        UiEventKind::Closed,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiEventKind::Click),
            1 => Some(UiEventKind::Focus),
            2 => Some(UiEventKind::Blur),
            3 => Some(UiEventKind::Changed),
            4 => Some(UiEventKind::Submitted),
            5 => Some(UiEventKind::Opened),
            6 => Some(UiEventKind::Closed),
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
            UiEventKind::Click => "CY_UI_EVENT_CLICK",
            UiEventKind::Focus => "CY_UI_EVENT_FOCUS",
            UiEventKind::Blur => "CY_UI_EVENT_BLUR",
            UiEventKind::Changed => "CY_UI_EVENT_CHANGED",
            UiEventKind::Submitted => "CY_UI_EVENT_SUBMITTED",
            UiEventKind::Opened => "CY_UI_EVENT_OPENED",
            UiEventKind::Closed => "CY_UI_EVENT_CLOSED",
        }
    }
}
/// `CyUiKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiKind {
    /// `CY_UI_PANEL` = 0.
    Panel = 0,
    /// `CY_UI_LABEL` = 1.
    Label = 1,
    /// `CY_UI_IMAGE` = 2.
    Image = 2,
    /// `CY_UI_PROGRESS` = 3.
    Progress = 3,
    /// `CY_UI_BUTTON` = 4.
    Button = 4,
}

impl UiKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiKind; 5] = [
        UiKind::Panel,
        UiKind::Label,
        UiKind::Image,
        UiKind::Progress,
        UiKind::Button,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiKind::Panel),
            1 => Some(UiKind::Label),
            2 => Some(UiKind::Image),
            3 => Some(UiKind::Progress),
            4 => Some(UiKind::Button),
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
            UiKind::Panel => "CY_UI_PANEL",
            UiKind::Label => "CY_UI_LABEL",
            UiKind::Image => "CY_UI_IMAGE",
            UiKind::Progress => "CY_UI_PROGRESS",
            UiKind::Button => "CY_UI_BUTTON",
        }
    }
}
/// `CyUiLayoutModel`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiLayoutModel {
    /// `CY_UI_LAYOUT_FLEX` = 0.
    Flex = 0,
    /// `CY_UI_LAYOUT_GRID` = 1.
    Grid = 1,
    /// `CY_UI_LAYOUT_ABSOLUTE` = 2.
    Absolute = 2,
}

impl UiLayoutModel {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiLayoutModel; 3] = [
        UiLayoutModel::Flex,
        UiLayoutModel::Grid,
        UiLayoutModel::Absolute,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiLayoutModel::Flex),
            1 => Some(UiLayoutModel::Grid),
            2 => Some(UiLayoutModel::Absolute),
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
            UiLayoutModel::Flex => "CY_UI_LAYOUT_FLEX",
            UiLayoutModel::Grid => "CY_UI_LAYOUT_GRID",
            UiLayoutModel::Absolute => "CY_UI_LAYOUT_ABSOLUTE",
        }
    }
}
/// `CyUiDirection`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiDirection {
    /// `CY_UI_DIRECTION_ROW` = 0.
    Row = 0,
    /// `CY_UI_DIRECTION_COLUMN` = 1.
    Column = 1,
    /// `CY_UI_DIRECTION_ROW_REVERSE` = 2.
    RowReverse = 2,
    /// `CY_UI_DIRECTION_COLUMN_REVERSE` = 3.
    ColumnReverse = 3,
}

impl UiDirection {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiDirection; 4] = [
        UiDirection::Row,
        UiDirection::Column,
        UiDirection::RowReverse,
        UiDirection::ColumnReverse,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiDirection::Row),
            1 => Some(UiDirection::Column),
            2 => Some(UiDirection::RowReverse),
            3 => Some(UiDirection::ColumnReverse),
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
            UiDirection::Row => "CY_UI_DIRECTION_ROW",
            UiDirection::Column => "CY_UI_DIRECTION_COLUMN",
            UiDirection::RowReverse => "CY_UI_DIRECTION_ROW_REVERSE",
            UiDirection::ColumnReverse => "CY_UI_DIRECTION_COLUMN_REVERSE",
        }
    }
}
/// `CyUiJustify`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiJustify {
    /// `CY_UI_JUSTIFY_START` = 0.
    Start = 0,
    /// `CY_UI_JUSTIFY_CENTRE` = 1.
    Centre = 1,
    /// `CY_UI_JUSTIFY_END` = 2.
    End = 2,
    /// `CY_UI_JUSTIFY_SPACE_BETWEEN` = 3.
    SpaceBetween = 3,
    /// `CY_UI_JUSTIFY_SPACE_AROUND` = 4.
    SpaceAround = 4,
    /// `CY_UI_JUSTIFY_SPACE_EVENLY` = 5.
    SpaceEvenly = 5,
}

impl UiJustify {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiJustify; 6] = [
        UiJustify::Start,
        UiJustify::Centre,
        UiJustify::End,
        UiJustify::SpaceBetween,
        UiJustify::SpaceAround,
        UiJustify::SpaceEvenly,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiJustify::Start),
            1 => Some(UiJustify::Centre),
            2 => Some(UiJustify::End),
            3 => Some(UiJustify::SpaceBetween),
            4 => Some(UiJustify::SpaceAround),
            5 => Some(UiJustify::SpaceEvenly),
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
            UiJustify::Start => "CY_UI_JUSTIFY_START",
            UiJustify::Centre => "CY_UI_JUSTIFY_CENTRE",
            UiJustify::End => "CY_UI_JUSTIFY_END",
            UiJustify::SpaceBetween => "CY_UI_JUSTIFY_SPACE_BETWEEN",
            UiJustify::SpaceAround => "CY_UI_JUSTIFY_SPACE_AROUND",
            UiJustify::SpaceEvenly => "CY_UI_JUSTIFY_SPACE_EVENLY",
        }
    }
}
/// `CyUiAlign`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiAlign {
    /// `CY_UI_ALIGN_STRETCH` = 0.
    Stretch = 0,
    /// `CY_UI_ALIGN_START` = 1.
    Start = 1,
    /// `CY_UI_ALIGN_CENTRE` = 2.
    Centre = 2,
    /// `CY_UI_ALIGN_END` = 3.
    End = 3,
}

impl UiAlign {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiAlign; 4] = [
        UiAlign::Stretch,
        UiAlign::Start,
        UiAlign::Centre,
        UiAlign::End,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiAlign::Stretch),
            1 => Some(UiAlign::Start),
            2 => Some(UiAlign::Centre),
            3 => Some(UiAlign::End),
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
            UiAlign::Stretch => "CY_UI_ALIGN_STRETCH",
            UiAlign::Start => "CY_UI_ALIGN_START",
            UiAlign::Centre => "CY_UI_ALIGN_CENTRE",
            UiAlign::End => "CY_UI_ALIGN_END",
        }
    }
}
/// `CyUiVisibility`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiVisibility {
    /// `CY_UI_VISIBLE` = 0.
    Visible = 0,
    /// `CY_UI_HIDDEN` = 1.
    Hidden = 1,
    /// `CY_UI_COLLAPSED` = 2.
    Collapsed = 2,
}

impl UiVisibility {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiVisibility; 3] = [
        UiVisibility::Visible,
        UiVisibility::Hidden,
        UiVisibility::Collapsed,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiVisibility::Visible),
            1 => Some(UiVisibility::Hidden),
            2 => Some(UiVisibility::Collapsed),
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
            UiVisibility::Visible => "CY_UI_VISIBLE",
            UiVisibility::Hidden => "CY_UI_HIDDEN",
            UiVisibility::Collapsed => "CY_UI_COLLAPSED",
        }
    }
}
/// `CyRootMotionMode`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum RootMotionMode {
    /// `CY_ROOT_MOTION_IGNORE` = 0.
    Ignore = 0,
    /// `CY_ROOT_MOTION_TRANSFORM` = 1.
    Transform = 1,
    /// `CY_ROOT_MOTION_ACCUMULATE` = 2.
    Accumulate = 2,
    /// `CY_ROOT_MOTION_EXTRACT` = 3.
    Extract = 3,
    /// `CY_ROOT_MOTION_CHARACTER` = 4.
    Character = 4,
}

impl RootMotionMode {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [RootMotionMode; 5] = [
        RootMotionMode::Ignore,
        RootMotionMode::Transform,
        RootMotionMode::Accumulate,
        RootMotionMode::Extract,
        RootMotionMode::Character,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(RootMotionMode::Ignore),
            1 => Some(RootMotionMode::Transform),
            2 => Some(RootMotionMode::Accumulate),
            3 => Some(RootMotionMode::Extract),
            4 => Some(RootMotionMode::Character),
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
            RootMotionMode::Ignore => "CY_ROOT_MOTION_IGNORE",
            RootMotionMode::Transform => "CY_ROOT_MOTION_TRANSFORM",
            RootMotionMode::Accumulate => "CY_ROOT_MOTION_ACCUMULATE",
            RootMotionMode::Extract => "CY_ROOT_MOTION_EXTRACT",
            RootMotionMode::Character => "CY_ROOT_MOTION_CHARACTER",
        }
    }
}
/// `CyAnimationTier`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum AnimationTier {
    /// `CY_ANIMATION_TIER_FULL` = 0.
    Full = 0,
    /// `CY_ANIMATION_TIER_SIMPLIFIED` = 1.
    Simplified = 1,
    /// `CY_ANIMATION_TIER_CACHED` = 2.
    Cached = 2,
    /// `CY_ANIMATION_TIER_BAKED` = 3.
    Baked = 3,
}

impl AnimationTier {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [AnimationTier; 4] = [
        AnimationTier::Full,
        AnimationTier::Simplified,
        AnimationTier::Cached,
        AnimationTier::Baked,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(AnimationTier::Full),
            1 => Some(AnimationTier::Simplified),
            2 => Some(AnimationTier::Cached),
            3 => Some(AnimationTier::Baked),
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
            AnimationTier::Full => "CY_ANIMATION_TIER_FULL",
            AnimationTier::Simplified => "CY_ANIMATION_TIER_SIMPLIFIED",
            AnimationTier::Cached => "CY_ANIMATION_TIER_CACHED",
            AnimationTier::Baked => "CY_ANIMATION_TIER_BAKED",
        }
    }
}
/// `CyDetmathFunction`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum DetmathFunction {
    /// `CY_DETMATH_SQRT` = 0.
    Sqrt = 0,
    /// `CY_DETMATH_SIN` = 1.
    Sin = 1,
    /// `CY_DETMATH_COS` = 2.
    Cos = 2,
    /// `CY_DETMATH_TAN` = 3.
    Tan = 3,
    /// `CY_DETMATH_ATAN` = 4.
    Atan = 4,
    /// `CY_DETMATH_ATAN2` = 5.
    Atan2 = 5,
    /// `CY_DETMATH_ASIN` = 6.
    Asin = 6,
    /// `CY_DETMATH_ACOS` = 7.
    Acos = 7,
    /// `CY_DETMATH_EXP2` = 8.
    Exp2 = 8,
    /// `CY_DETMATH_LOG2` = 9.
    Log2 = 9,
    /// `CY_DETMATH_EXP` = 10.
    Exp = 10,
    /// `CY_DETMATH_LOG` = 11.
    Log = 11,
    /// `CY_DETMATH_POW` = 12.
    Pow = 12,
}

impl DetmathFunction {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [DetmathFunction; 13] = [
        DetmathFunction::Sqrt,
        DetmathFunction::Sin,
        DetmathFunction::Cos,
        DetmathFunction::Tan,
        DetmathFunction::Atan,
        DetmathFunction::Atan2,
        DetmathFunction::Asin,
        DetmathFunction::Acos,
        DetmathFunction::Exp2,
        DetmathFunction::Log2,
        DetmathFunction::Exp,
        DetmathFunction::Log,
        DetmathFunction::Pow,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(DetmathFunction::Sqrt),
            1 => Some(DetmathFunction::Sin),
            2 => Some(DetmathFunction::Cos),
            3 => Some(DetmathFunction::Tan),
            4 => Some(DetmathFunction::Atan),
            5 => Some(DetmathFunction::Atan2),
            6 => Some(DetmathFunction::Asin),
            7 => Some(DetmathFunction::Acos),
            8 => Some(DetmathFunction::Exp2),
            9 => Some(DetmathFunction::Log2),
            10 => Some(DetmathFunction::Exp),
            11 => Some(DetmathFunction::Log),
            12 => Some(DetmathFunction::Pow),
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
            DetmathFunction::Sqrt => "CY_DETMATH_SQRT",
            DetmathFunction::Sin => "CY_DETMATH_SIN",
            DetmathFunction::Cos => "CY_DETMATH_COS",
            DetmathFunction::Tan => "CY_DETMATH_TAN",
            DetmathFunction::Atan => "CY_DETMATH_ATAN",
            DetmathFunction::Atan2 => "CY_DETMATH_ATAN2",
            DetmathFunction::Asin => "CY_DETMATH_ASIN",
            DetmathFunction::Acos => "CY_DETMATH_ACOS",
            DetmathFunction::Exp2 => "CY_DETMATH_EXP2",
            DetmathFunction::Log2 => "CY_DETMATH_LOG2",
            DetmathFunction::Exp => "CY_DETMATH_EXP",
            DetmathFunction::Log => "CY_DETMATH_LOG",
            DetmathFunction::Pow => "CY_DETMATH_POW",
        }
    }
}
/// `CyLockstepOrderKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum LockstepOrderKind {
    /// `CY_LOCKSTEP_ORDER_MOVE` = 0.
    Move = 0,
    /// `CY_LOCKSTEP_ORDER_STOP` = 1.
    Stop = 1,
}

impl LockstepOrderKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [LockstepOrderKind; 2] = [LockstepOrderKind::Move, LockstepOrderKind::Stop];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(LockstepOrderKind::Move),
            1 => Some(LockstepOrderKind::Stop),
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
            LockstepOrderKind::Move => "CY_LOCKSTEP_ORDER_MOVE",
            LockstepOrderKind::Stop => "CY_LOCKSTEP_ORDER_STOP",
        }
    }
}
/// `CyGiMode`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GiMode {
    /// `CY_GI_MODE_NONE` = 0.
    None = 0,
    /// `CY_GI_MODE_BAKED` = 1.
    Baked = 1,
    /// `CY_GI_MODE_PROBE` = 2.
    Probe = 2,
    /// `CY_GI_MODE_DYNAMIC` = 3.
    Dynamic = 3,
    /// `CY_GI_MODE_HYBRID` = 4.
    Hybrid = 4,
}

impl GiMode {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GiMode; 5] = [
        GiMode::None,
        GiMode::Baked,
        GiMode::Probe,
        GiMode::Dynamic,
        GiMode::Hybrid,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GiMode::None),
            1 => Some(GiMode::Baked),
            2 => Some(GiMode::Probe),
            3 => Some(GiMode::Dynamic),
            4 => Some(GiMode::Hybrid),
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
            GiMode::None => "CY_GI_MODE_NONE",
            GiMode::Baked => "CY_GI_MODE_BAKED",
            GiMode::Probe => "CY_GI_MODE_PROBE",
            GiMode::Dynamic => "CY_GI_MODE_DYNAMIC",
            GiMode::Hybrid => "CY_GI_MODE_HYBRID",
        }
    }
}
/// `CyGiQuality`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GiQuality {
    /// `CY_GI_QUALITY_LOW` = 0.
    Low = 0,
    /// `CY_GI_QUALITY_MEDIUM` = 1.
    Medium = 1,
    /// `CY_GI_QUALITY_HIGH` = 2.
    High = 2,
    /// `CY_GI_QUALITY_EPIC` = 3.
    Epic = 3,
}

impl GiQuality {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GiQuality; 4] = [
        GiQuality::Low,
        GiQuality::Medium,
        GiQuality::High,
        GiQuality::Epic,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GiQuality::Low),
            1 => Some(GiQuality::Medium),
            2 => Some(GiQuality::High),
            3 => Some(GiQuality::Epic),
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
            GiQuality::Low => "CY_GI_QUALITY_LOW",
            GiQuality::Medium => "CY_GI_QUALITY_MEDIUM",
            GiQuality::High => "CY_GI_QUALITY_HIGH",
            GiQuality::Epic => "CY_GI_QUALITY_EPIC",
        }
    }
}
/// `CyGiTier`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GiTier {
    /// `CY_GI_TIER_AUTOMATIC` = 0.
    Automatic = 0,
    /// `CY_GI_TIER_SOFTWARE_ONLY` = 1.
    SoftwareOnly = 1,
    /// `CY_GI_TIER_WORLD_ONLY` = 2.
    WorldOnly = 2,
}

impl GiTier {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GiTier; 3] = [GiTier::Automatic, GiTier::SoftwareOnly, GiTier::WorldOnly];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GiTier::Automatic),
            1 => Some(GiTier::SoftwareOnly),
            2 => Some(GiTier::WorldOnly),
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
            GiTier::Automatic => "CY_GI_TIER_AUTOMATIC",
            GiTier::SoftwareOnly => "CY_GI_TIER_SOFTWARE_ONLY",
            GiTier::WorldOnly => "CY_GI_TIER_WORLD_ONLY",
        }
    }
}
/// `CyGiClass`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GiClass {
    /// `CY_GI_CLASS_STATIC` = 0.
    Static = 0,
    /// `CY_GI_CLASS_SLOW_DYNAMIC` = 1.
    SlowDynamic = 1,
    /// `CY_GI_CLASS_FAST_DYNAMIC` = 2.
    FastDynamic = 2,
    /// `CY_GI_CLASS_TRANSIENT` = 3.
    Transient = 3,
    /// `CY_GI_CLASS_AUTOMATIC` = 4.
    Automatic = 4,
}

impl GiClass {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GiClass; 5] = [
        GiClass::Static,
        GiClass::SlowDynamic,
        GiClass::FastDynamic,
        GiClass::Transient,
        GiClass::Automatic,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GiClass::Static),
            1 => Some(GiClass::SlowDynamic),
            2 => Some(GiClass::FastDynamic),
            3 => Some(GiClass::Transient),
            4 => Some(GiClass::Automatic),
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
            GiClass::Static => "CY_GI_CLASS_STATIC",
            GiClass::SlowDynamic => "CY_GI_CLASS_SLOW_DYNAMIC",
            GiClass::FastDynamic => "CY_GI_CLASS_FAST_DYNAMIC",
            GiClass::Transient => "CY_GI_CLASS_TRANSIENT",
            GiClass::Automatic => "CY_GI_CLASS_AUTOMATIC",
        }
    }
}
/// `CyGiEmission`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GiEmission {
    /// `CY_GI_EMISSION_AUTOMATIC` = 0.
    Automatic = 0,
    /// `CY_GI_EMISSION_CACHE_ONLY` = 1.
    CacheOnly = 1,
    /// `CY_GI_EMISSION_PROMOTE` = 2.
    Promote = 2,
    /// `CY_GI_EMISSION_OFF` = 3.
    Off = 3,
}

impl GiEmission {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GiEmission; 4] = [
        GiEmission::Automatic,
        GiEmission::CacheOnly,
        GiEmission::Promote,
        GiEmission::Off,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GiEmission::Automatic),
            1 => Some(GiEmission::CacheOnly),
            2 => Some(GiEmission::Promote),
            3 => Some(GiEmission::Off),
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
            GiEmission::Automatic => "CY_GI_EMISSION_AUTOMATIC",
            GiEmission::CacheOnly => "CY_GI_EMISSION_CACHE_ONLY",
            GiEmission::Promote => "CY_GI_EMISSION_PROMOTE",
            GiEmission::Off => "CY_GI_EMISSION_OFF",
        }
    }
}
/// `CyGiSource`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum GiSource {
    /// `CY_GI_SOURCE_NONE` = 0.
    None = 0,
    /// `CY_GI_SOURCE_LIGHTMAP` = 1.
    Lightmap = 1,
    /// `CY_GI_SOURCE_IRRADIANCE_VOLUME` = 2.
    IrradianceVolume = 2,
    /// `CY_GI_SOURCE_RADIANCE_CACHE` = 3.
    RadianceCache = 3,
    /// `CY_GI_SOURCE_SURFACE_CACHE` = 4.
    SurfaceCache = 4,
    /// `CY_GI_SOURCE_SCREEN_TRACE` = 5.
    ScreenTrace = 5,
    /// `CY_GI_SOURCE_SOFTWARE_TRACE` = 6.
    SoftwareTrace = 6,
    /// `CY_GI_SOURCE_HARDWARE_TRACE` = 7.
    HardwareTrace = 7,
    /// `CY_GI_SOURCE_REFLECTION_PROBE` = 8.
    ReflectionProbe = 8,
    /// `CY_GI_SOURCE_SKY` = 9.
    Sky = 9,
}

impl GiSource {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [GiSource; 10] = [
        GiSource::None,
        GiSource::Lightmap,
        GiSource::IrradianceVolume,
        GiSource::RadianceCache,
        GiSource::SurfaceCache,
        GiSource::ScreenTrace,
        GiSource::SoftwareTrace,
        GiSource::HardwareTrace,
        GiSource::ReflectionProbe,
        GiSource::Sky,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(GiSource::None),
            1 => Some(GiSource::Lightmap),
            2 => Some(GiSource::IrradianceVolume),
            3 => Some(GiSource::RadianceCache),
            4 => Some(GiSource::SurfaceCache),
            5 => Some(GiSource::ScreenTrace),
            6 => Some(GiSource::SoftwareTrace),
            7 => Some(GiSource::HardwareTrace),
            8 => Some(GiSource::ReflectionProbe),
            9 => Some(GiSource::Sky),
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
            GiSource::None => "CY_GI_SOURCE_NONE",
            GiSource::Lightmap => "CY_GI_SOURCE_LIGHTMAP",
            GiSource::IrradianceVolume => "CY_GI_SOURCE_IRRADIANCE_VOLUME",
            GiSource::RadianceCache => "CY_GI_SOURCE_RADIANCE_CACHE",
            GiSource::SurfaceCache => "CY_GI_SOURCE_SURFACE_CACHE",
            GiSource::ScreenTrace => "CY_GI_SOURCE_SCREEN_TRACE",
            GiSource::SoftwareTrace => "CY_GI_SOURCE_SOFTWARE_TRACE",
            GiSource::HardwareTrace => "CY_GI_SOURCE_HARDWARE_TRACE",
            GiSource::ReflectionProbe => "CY_GI_SOURCE_REFLECTION_PROBE",
            GiSource::Sky => "CY_GI_SOURCE_SKY",
        }
    }
}
/// `CyFogCell`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum FogCell {
    /// `CY_FOG_CELL_UNEXPLORED` = 0.
    Unexplored = 0,
    /// `CY_FOG_CELL_EXPLORED` = 1.
    Explored = 1,
    /// `CY_FOG_CELL_VISIBLE` = 2.
    Visible = 2,
}

impl FogCell {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [FogCell; 3] = [FogCell::Unexplored, FogCell::Explored, FogCell::Visible];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(FogCell::Unexplored),
            1 => Some(FogCell::Explored),
            2 => Some(FogCell::Visible),
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
            FogCell::Unexplored => "CY_FOG_CELL_UNEXPLORED",
            FogCell::Explored => "CY_FOG_CELL_EXPLORED",
            FogCell::Visible => "CY_FOG_CELL_VISIBLE",
        }
    }
}
/// `CyFogSight`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum FogSight {
    /// `CY_FOG_SIGHT_HIDDEN` = 0.
    Hidden = 0,
    /// `CY_FOG_SIGHT_GHOST` = 1.
    Ghost = 1,
    /// `CY_FOG_SIGHT_VISIBLE` = 2.
    Visible = 2,
}

impl FogSight {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [FogSight; 3] = [FogSight::Hidden, FogSight::Ghost, FogSight::Visible];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(FogSight::Hidden),
            1 => Some(FogSight::Ghost),
            2 => Some(FogSight::Visible),
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
            FogSight::Hidden => "CY_FOG_SIGHT_HIDDEN",
            FogSight::Ghost => "CY_FOG_SIGHT_GHOST",
            FogSight::Visible => "CY_FOG_SIGHT_VISIBLE",
        }
    }
}
/// `CyLightKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum LightKind {
    /// `CY_LIGHT_POINT` = 1.
    Point = 1,
    /// `CY_LIGHT_SPOT` = 2.
    Spot = 2,
    /// `CY_LIGHT_RECT` = 3.
    Rect = 3,
    /// `CY_LIGHT_DISC` = 4.
    Disc = 4,
    /// `CY_LIGHT_SPHERE` = 5.
    Sphere = 5,
    /// `CY_LIGHT_TUBE` = 6.
    Tube = 6,
}

impl LightKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [LightKind; 6] = [
        LightKind::Point,
        LightKind::Spot,
        LightKind::Rect,
        LightKind::Disc,
        LightKind::Sphere,
        LightKind::Tube,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            1 => Some(LightKind::Point),
            2 => Some(LightKind::Spot),
            3 => Some(LightKind::Rect),
            4 => Some(LightKind::Disc),
            5 => Some(LightKind::Sphere),
            6 => Some(LightKind::Tube),
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
            LightKind::Point => "CY_LIGHT_POINT",
            LightKind::Spot => "CY_LIGHT_SPOT",
            LightKind::Rect => "CY_LIGHT_RECT",
            LightKind::Disc => "CY_LIGHT_DISC",
            LightKind::Sphere => "CY_LIGHT_SPHERE",
            LightKind::Tube => "CY_LIGHT_TUBE",
        }
    }
}
/// `CyLightFade`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum LightFade {
    /// `CY_LIGHT_FADE_NONE` = 0.
    None = 0,
    /// `CY_LIGHT_FADE_LINEAR` = 1.
    Linear = 1,
    /// `CY_LIGHT_FADE_QUADRATIC` = 2.
    Quadratic = 2,
}

impl LightFade {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [LightFade; 3] = [LightFade::None, LightFade::Linear, LightFade::Quadratic];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(LightFade::None),
            1 => Some(LightFade::Linear),
            2 => Some(LightFade::Quadratic),
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
            LightFade::None => "CY_LIGHT_FADE_NONE",
            LightFade::Linear => "CY_LIGHT_FADE_LINEAR",
            LightFade::Quadratic => "CY_LIGHT_FADE_QUADRATIC",
        }
    }
}
/// `CyUiWidgetKind`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiWidgetKind {
    /// `CY_UI_WIDGET_PANEL` = 0.
    Panel = 0,
    /// `CY_UI_WIDGET_ROW` = 1.
    Row = 1,
    /// `CY_UI_WIDGET_COLUMN` = 2.
    Column = 2,
    /// `CY_UI_WIDGET_GRID` = 3.
    Grid = 3,
    /// `CY_UI_WIDGET_STACK` = 4.
    Stack = 4,
    /// `CY_UI_WIDGET_SCROLL_VIEW` = 5.
    ScrollView = 5,
    /// `CY_UI_WIDGET_LABEL` = 6.
    Label = 6,
    /// `CY_UI_WIDGET_RICH_TEXT` = 7.
    RichText = 7,
    /// `CY_UI_WIDGET_IMAGE` = 8.
    Image = 8,
    /// `CY_UI_WIDGET_BUTTON` = 9.
    Button = 9,
    /// `CY_UI_WIDGET_TOGGLE` = 10.
    Toggle = 10,
    /// `CY_UI_WIDGET_CHECKBOX` = 11.
    Checkbox = 11,
    /// `CY_UI_WIDGET_RADIO_GROUP` = 12.
    RadioGroup = 12,
    /// `CY_UI_WIDGET_SLIDER` = 13.
    Slider = 13,
    /// `CY_UI_WIDGET_PROGRESS_BAR` = 14.
    ProgressBar = 14,
    /// `CY_UI_WIDGET_TEXT_FIELD` = 15.
    TextField = 15,
    /// `CY_UI_WIDGET_DROPDOWN` = 16.
    Dropdown = 16,
    /// `CY_UI_WIDGET_LIST_VIEW` = 17.
    ListView = 17,
    /// `CY_UI_WIDGET_TABS` = 18.
    Tabs = 18,
    /// `CY_UI_WIDGET_DIALOG` = 19.
    Dialog = 19,
}

impl UiWidgetKind {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiWidgetKind; 20] = [
        UiWidgetKind::Panel,
        UiWidgetKind::Row,
        UiWidgetKind::Column,
        UiWidgetKind::Grid,
        UiWidgetKind::Stack,
        UiWidgetKind::ScrollView,
        UiWidgetKind::Label,
        UiWidgetKind::RichText,
        UiWidgetKind::Image,
        UiWidgetKind::Button,
        UiWidgetKind::Toggle,
        UiWidgetKind::Checkbox,
        UiWidgetKind::RadioGroup,
        UiWidgetKind::Slider,
        UiWidgetKind::ProgressBar,
        UiWidgetKind::TextField,
        UiWidgetKind::Dropdown,
        UiWidgetKind::ListView,
        UiWidgetKind::Tabs,
        UiWidgetKind::Dialog,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiWidgetKind::Panel),
            1 => Some(UiWidgetKind::Row),
            2 => Some(UiWidgetKind::Column),
            3 => Some(UiWidgetKind::Grid),
            4 => Some(UiWidgetKind::Stack),
            5 => Some(UiWidgetKind::ScrollView),
            6 => Some(UiWidgetKind::Label),
            7 => Some(UiWidgetKind::RichText),
            8 => Some(UiWidgetKind::Image),
            9 => Some(UiWidgetKind::Button),
            10 => Some(UiWidgetKind::Toggle),
            11 => Some(UiWidgetKind::Checkbox),
            12 => Some(UiWidgetKind::RadioGroup),
            13 => Some(UiWidgetKind::Slider),
            14 => Some(UiWidgetKind::ProgressBar),
            15 => Some(UiWidgetKind::TextField),
            16 => Some(UiWidgetKind::Dropdown),
            17 => Some(UiWidgetKind::ListView),
            18 => Some(UiWidgetKind::Tabs),
            19 => Some(UiWidgetKind::Dialog),
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
            UiWidgetKind::Panel => "CY_UI_WIDGET_PANEL",
            UiWidgetKind::Row => "CY_UI_WIDGET_ROW",
            UiWidgetKind::Column => "CY_UI_WIDGET_COLUMN",
            UiWidgetKind::Grid => "CY_UI_WIDGET_GRID",
            UiWidgetKind::Stack => "CY_UI_WIDGET_STACK",
            UiWidgetKind::ScrollView => "CY_UI_WIDGET_SCROLL_VIEW",
            UiWidgetKind::Label => "CY_UI_WIDGET_LABEL",
            UiWidgetKind::RichText => "CY_UI_WIDGET_RICH_TEXT",
            UiWidgetKind::Image => "CY_UI_WIDGET_IMAGE",
            UiWidgetKind::Button => "CY_UI_WIDGET_BUTTON",
            UiWidgetKind::Toggle => "CY_UI_WIDGET_TOGGLE",
            UiWidgetKind::Checkbox => "CY_UI_WIDGET_CHECKBOX",
            UiWidgetKind::RadioGroup => "CY_UI_WIDGET_RADIO_GROUP",
            UiWidgetKind::Slider => "CY_UI_WIDGET_SLIDER",
            UiWidgetKind::ProgressBar => "CY_UI_WIDGET_PROGRESS_BAR",
            UiWidgetKind::TextField => "CY_UI_WIDGET_TEXT_FIELD",
            UiWidgetKind::Dropdown => "CY_UI_WIDGET_DROPDOWN",
            UiWidgetKind::ListView => "CY_UI_WIDGET_LIST_VIEW",
            UiWidgetKind::Tabs => "CY_UI_WIDGET_TABS",
            UiWidgetKind::Dialog => "CY_UI_WIDGET_DIALOG",
        }
    }
}
/// `CyUiAction`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiAction {
    /// `CY_UI_ACTION_ACCEPT` = 0.
    Accept = 0,
    /// `CY_UI_ACTION_CANCEL` = 1.
    Cancel = 1,
    /// `CY_UI_ACTION_NAVIGATE_UP` = 2.
    NavigateUp = 2,
    /// `CY_UI_ACTION_NAVIGATE_DOWN` = 3.
    NavigateDown = 3,
    /// `CY_UI_ACTION_NAVIGATE_LEFT` = 4.
    NavigateLeft = 4,
    /// `CY_UI_ACTION_NAVIGATE_RIGHT` = 5.
    NavigateRight = 5,
    /// `CY_UI_ACTION_NEXT_TAB` = 6.
    NextTab = 6,
    /// `CY_UI_ACTION_PREVIOUS_TAB` = 7.
    PreviousTab = 7,
    /// `CY_UI_ACTION_CONTEXT` = 8.
    Context = 8,
    /// `CY_UI_ACTION_SCROLL_UP` = 9.
    ScrollUp = 9,
    /// `CY_UI_ACTION_SCROLL_DOWN` = 10.
    ScrollDown = 10,
    /// `CY_UI_ACTION_FOCUS_NEXT` = 11.
    FocusNext = 11,
    /// `CY_UI_ACTION_FOCUS_PREVIOUS` = 12.
    FocusPrevious = 12,
}

impl UiAction {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiAction; 13] = [
        UiAction::Accept,
        UiAction::Cancel,
        UiAction::NavigateUp,
        UiAction::NavigateDown,
        UiAction::NavigateLeft,
        UiAction::NavigateRight,
        UiAction::NextTab,
        UiAction::PreviousTab,
        UiAction::Context,
        UiAction::ScrollUp,
        UiAction::ScrollDown,
        UiAction::FocusNext,
        UiAction::FocusPrevious,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiAction::Accept),
            1 => Some(UiAction::Cancel),
            2 => Some(UiAction::NavigateUp),
            3 => Some(UiAction::NavigateDown),
            4 => Some(UiAction::NavigateLeft),
            5 => Some(UiAction::NavigateRight),
            6 => Some(UiAction::NextTab),
            7 => Some(UiAction::PreviousTab),
            8 => Some(UiAction::Context),
            9 => Some(UiAction::ScrollUp),
            10 => Some(UiAction::ScrollDown),
            11 => Some(UiAction::FocusNext),
            12 => Some(UiAction::FocusPrevious),
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
            UiAction::Accept => "CY_UI_ACTION_ACCEPT",
            UiAction::Cancel => "CY_UI_ACTION_CANCEL",
            UiAction::NavigateUp => "CY_UI_ACTION_NAVIGATE_UP",
            UiAction::NavigateDown => "CY_UI_ACTION_NAVIGATE_DOWN",
            UiAction::NavigateLeft => "CY_UI_ACTION_NAVIGATE_LEFT",
            UiAction::NavigateRight => "CY_UI_ACTION_NAVIGATE_RIGHT",
            UiAction::NextTab => "CY_UI_ACTION_NEXT_TAB",
            UiAction::PreviousTab => "CY_UI_ACTION_PREVIOUS_TAB",
            UiAction::Context => "CY_UI_ACTION_CONTEXT",
            UiAction::ScrollUp => "CY_UI_ACTION_SCROLL_UP",
            UiAction::ScrollDown => "CY_UI_ACTION_SCROLL_DOWN",
            UiAction::FocusNext => "CY_UI_ACTION_FOCUS_NEXT",
            UiAction::FocusPrevious => "CY_UI_ACTION_FOCUS_PREVIOUS",
        }
    }
}
/// `CyUiAnimatedProperty`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiAnimatedProperty {
    /// `CY_UI_ANIMATE_OPACITY` = 0.
    Opacity = 0,
    /// `CY_UI_ANIMATE_TRANSLATE_X` = 1.
    TranslateX = 1,
    /// `CY_UI_ANIMATE_TRANSLATE_Y` = 2.
    TranslateY = 2,
    /// `CY_UI_ANIMATE_SCALE` = 3.
    Scale = 3,
    /// `CY_UI_ANIMATE_BACKGROUND` = 4.
    Background = 4,
}

impl UiAnimatedProperty {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiAnimatedProperty; 5] = [
        UiAnimatedProperty::Opacity,
        UiAnimatedProperty::TranslateX,
        UiAnimatedProperty::TranslateY,
        UiAnimatedProperty::Scale,
        UiAnimatedProperty::Background,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiAnimatedProperty::Opacity),
            1 => Some(UiAnimatedProperty::TranslateX),
            2 => Some(UiAnimatedProperty::TranslateY),
            3 => Some(UiAnimatedProperty::Scale),
            4 => Some(UiAnimatedProperty::Background),
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
            UiAnimatedProperty::Opacity => "CY_UI_ANIMATE_OPACITY",
            UiAnimatedProperty::TranslateX => "CY_UI_ANIMATE_TRANSLATE_X",
            UiAnimatedProperty::TranslateY => "CY_UI_ANIMATE_TRANSLATE_Y",
            UiAnimatedProperty::Scale => "CY_UI_ANIMATE_SCALE",
            UiAnimatedProperty::Background => "CY_UI_ANIMATE_BACKGROUND",
        }
    }
}
/// `CyUiEasing`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum UiEasing {
    /// `CY_UI_EASING_LINEAR` = 0.
    Linear = 0,
    /// `CY_UI_EASING_EASE_IN` = 1.
    EaseIn = 1,
    /// `CY_UI_EASING_EASE_OUT` = 2.
    EaseOut = 2,
    /// `CY_UI_EASING_EASE_IN_OUT` = 3.
    EaseInOut = 3,
    /// `CY_UI_EASING_EASE` = 4.
    Ease = 4,
    /// `CY_UI_EASING_SPRING` = 5.
    Spring = 5,
    /// `CY_UI_EASING_BACK` = 6.
    Back = 6,
}

impl UiEasing {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [UiEasing; 7] = [
        UiEasing::Linear,
        UiEasing::EaseIn,
        UiEasing::EaseOut,
        UiEasing::EaseInOut,
        UiEasing::Ease,
        UiEasing::Spring,
        UiEasing::Back,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(UiEasing::Linear),
            1 => Some(UiEasing::EaseIn),
            2 => Some(UiEasing::EaseOut),
            3 => Some(UiEasing::EaseInOut),
            4 => Some(UiEasing::Ease),
            5 => Some(UiEasing::Spring),
            6 => Some(UiEasing::Back),
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
            UiEasing::Linear => "CY_UI_EASING_LINEAR",
            UiEasing::EaseIn => "CY_UI_EASING_EASE_IN",
            UiEasing::EaseOut => "CY_UI_EASING_EASE_OUT",
            UiEasing::EaseInOut => "CY_UI_EASING_EASE_IN_OUT",
            UiEasing::Ease => "CY_UI_EASING_EASE",
            UiEasing::Spring => "CY_UI_EASING_SPRING",
            UiEasing::Back => "CY_UI_EASING_BACK",
        }
    }
}
/// `CyInputScheme`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum InputScheme {
    /// `CY_INPUT_SCHEME_KEYBOARD_MOUSE` = 0.
    KeyboardMouse = 0,
    /// `CY_INPUT_SCHEME_GAMEPAD` = 1.
    Gamepad = 1,
}

impl InputScheme {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [InputScheme; 2] = [InputScheme::KeyboardMouse, InputScheme::Gamepad];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(InputScheme::KeyboardMouse),
            1 => Some(InputScheme::Gamepad),
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
            InputScheme::KeyboardMouse => "CY_INPUT_SCHEME_KEYBOARD_MOUSE",
            InputScheme::Gamepad => "CY_INPUT_SCHEME_GAMEPAD",
        }
    }
}
/// `CyInputConflict`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum InputConflict {
    /// `CY_INPUT_CONFLICT_REJECT` = 0.
    Reject = 0,
    /// `CY_INPUT_CONFLICT_SWAP` = 1.
    Swap = 1,
    /// `CY_INPUT_CONFLICT_ALLOW` = 2.
    Allow = 2,
    /// `CY_INPUT_CONFLICT_UNBIND_PREVIOUS` = 3.
    UnbindPrevious = 3,
}

impl InputConflict {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [InputConflict; 4] = [
        InputConflict::Reject,
        InputConflict::Swap,
        InputConflict::Allow,
        InputConflict::UnbindPrevious,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(InputConflict::Reject),
            1 => Some(InputConflict::Swap),
            2 => Some(InputConflict::Allow),
            3 => Some(InputConflict::UnbindPrevious),
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
            InputConflict::Reject => "CY_INPUT_CONFLICT_REJECT",
            InputConflict::Swap => "CY_INPUT_CONFLICT_SWAP",
            InputConflict::Allow => "CY_INPUT_CONFLICT_ALLOW",
            InputConflict::UnbindPrevious => "CY_INPUT_CONFLICT_UNBIND_PREVIOUS",
        }
    }
}
/// `CyInputRebindStatus`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum InputRebindStatus {
    /// `CY_INPUT_REBIND_IDLE` = 0.
    Idle = 0,
    /// `CY_INPUT_REBIND_LISTENING` = 1.
    Listening = 1,
    /// `CY_INPUT_REBIND_APPLIED` = 2.
    Applied = 2,
    /// `CY_INPUT_REBIND_CONFLICT` = 3.
    Conflict = 3,
    /// `CY_INPUT_REBIND_CANCELLED` = 4.
    Cancelled = 4,
}

impl InputRebindStatus {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [InputRebindStatus; 5] = [
        InputRebindStatus::Idle,
        InputRebindStatus::Listening,
        InputRebindStatus::Applied,
        InputRebindStatus::Conflict,
        InputRebindStatus::Cancelled,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(InputRebindStatus::Idle),
            1 => Some(InputRebindStatus::Listening),
            2 => Some(InputRebindStatus::Applied),
            3 => Some(InputRebindStatus::Conflict),
            4 => Some(InputRebindStatus::Cancelled),
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
            InputRebindStatus::Idle => "CY_INPUT_REBIND_IDLE",
            InputRebindStatus::Listening => "CY_INPUT_REBIND_LISTENING",
            InputRebindStatus::Applied => "CY_INPUT_REBIND_APPLIED",
            InputRebindStatus::Conflict => "CY_INPUT_REBIND_CONFLICT",
            InputRebindStatus::Cancelled => "CY_INPUT_REBIND_CANCELLED",
        }
    }
}
/// `CyVideoQuality`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum VideoQuality {
    /// `CY_VIDEO_QUALITY_LOW` = 0.
    Low = 0,
    /// `CY_VIDEO_QUALITY_MEDIUM` = 1.
    Medium = 1,
    /// `CY_VIDEO_QUALITY_HIGH` = 2.
    High = 2,
    /// `CY_VIDEO_QUALITY_ULTRA` = 3.
    Ultra = 3,
}

impl VideoQuality {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [VideoQuality; 4] = [
        VideoQuality::Low,
        VideoQuality::Medium,
        VideoQuality::High,
        VideoQuality::Ultra,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(VideoQuality::Low),
            1 => Some(VideoQuality::Medium),
            2 => Some(VideoQuality::High),
            3 => Some(VideoQuality::Ultra),
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
            VideoQuality::Low => "CY_VIDEO_QUALITY_LOW",
            VideoQuality::Medium => "CY_VIDEO_QUALITY_MEDIUM",
            VideoQuality::High => "CY_VIDEO_QUALITY_HIGH",
            VideoQuality::Ultra => "CY_VIDEO_QUALITY_ULTRA",
        }
    }
}
/// `CyVideoUpscale`, as the ABI declares it.
///
/// Stored as `u32` because that is what the ABI carries it in. `from_raw` is the only
/// way in: a value the engine sent that this SDK does not know is a `None` to be reported,
/// never a transmute — an unknown discriminant in a Rust enum is undefined behaviour, and an
/// engine one minor version ahead is exactly how one arrives.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
#[repr(u32)]
pub enum VideoUpscale {
    /// `CY_VIDEO_UPSCALE_NATIVE` = 0.
    Native = 0,
    /// `CY_VIDEO_UPSCALE_QUALITY` = 1.
    Quality = 1,
    /// `CY_VIDEO_UPSCALE_BALANCED` = 2.
    Balanced = 2,
    /// `CY_VIDEO_UPSCALE_PERFORMANCE` = 3.
    Performance = 3,
}

impl VideoUpscale {
    /// Every value, in declaration order. Lets a caller enumerate without a range.
    pub const ALL: [VideoUpscale; 4] = [
        VideoUpscale::Native,
        VideoUpscale::Quality,
        VideoUpscale::Balanced,
        VideoUpscale::Performance,
    ];

    /// The value the ABI carries, or `None` when this build has no name for it.
    #[must_use]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(VideoUpscale::Native),
            1 => Some(VideoUpscale::Quality),
            2 => Some(VideoUpscale::Balanced),
            3 => Some(VideoUpscale::Performance),
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
            VideoUpscale::Native => "CY_VIDEO_UPSCALE_NATIVE",
            VideoUpscale::Quality => "CY_VIDEO_UPSCALE_QUALITY",
            VideoUpscale::Balanced => "CY_VIDEO_UPSCALE_BALANCED",
            VideoUpscale::Performance => "CY_VIDEO_UPSCALE_PERFORMANCE",
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
