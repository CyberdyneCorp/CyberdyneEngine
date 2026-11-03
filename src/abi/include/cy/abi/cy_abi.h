/* cy/abi/cy_abi.h — the stable, flat C ABI CyberdyneEngine exports. Tasks 2.1, 2.2, 2.4, 2.5, 2.6.
 *
 * THIS FILE IS THE BOUNDARY. Everything above it is C++20 with -fno-exceptions and -fno-rtti;
 * everything below it is C that a Swift, Rust or C compiler reads without knowing that. It is the
 * one header the Swift overlay imports, the one the Rust SDK binds, and the one tools/abi/ parses
 * to produce the machine-readable description the compatibility gate diffs.
 *
 * --- THE FOUR RULES THAT MAKE IT AN ABI RATHER THAN AN INTERFACE --------------------------------
 *
 * 1. C CONSTRUCTS ONLY. `extern "C"` functions, opaque pointer handles, POD structs with explicit
 *    layout, fixed-width integers, function pointers. No class, no template, no reference, no
 *    `std::` type, no virtual dispatch, no exception. C++ has no stable ABI across compilers,
 *    standard library versions or optimisation settings; C does, and that is the whole reason this
 *    boundary is C.
 *
 * 2. APPEND-ONLY WITHIN A MAJOR VERSION. Every struct that a module may have compiled against
 *    carries `struct_size` as its first member, and `CyInterface` carries `CyInterfaceHeader` with
 *    `table_size`. New entries are appended at the end and the size grows; an existing entry is
 *    never reordered, removed, or given a different signature. `tools/abi/abi_gate.py` enforces
 *    that against a committed baseline, and it is a merge gate — see src/abi/README.md.
 *
 * 3. FAILURE CROSSES AS A VALUE. `CyResult` is returned; there is no exception, no `longjmp`, and
 *    no callback that may unwind. `cy::Expected<T, Error>` maps to (CyResult, out-parameter), and
 *    the mapping is mechanical: the first fifteen `CyResult` values are `cy::ErrorCode`'s own
 *    values in `cy::ErrorCode`'s own order, checked by static assertion in src/abi/src/errors.cpp.
 *
 * 4. NAMES ARE PREFIXED. Functions `cy_`, types `Cy`, enum constants and macros `CY_`. A symbol
 *    that is not is not part of this ABI.
 *
 * --- HOW A MODULE USES IT ----------------------------------------------------------------------
 *
 * The engine exports exactly one symbol for discovery, `cy_get_interface`. A module reaches
 * everything else through the returned table rather than by linking engine symbols, which is what
 * lets a module built against 1.2 keep running against 1.8 with no recompilation.
 *
 * A module is a shared library exporting `cy_module_entry`; its `module.toml` declares the entry
 * symbol, the minimum ABI version, its per-platform library paths, and whether it is
 * hot-reloadable. See cy/abi/module.h for the loader's side of that.
 *
 * --- THE FOUR SILENCED LINTS, AND WHY THEY ARE SPLIT ACROSS TWO DIRECTIVES ---------------------
 *
 * This file is C. `typedef` is how C names a type, <stdint.h> is how C spells the fixed-width
 * integers, a `#define` is how C states a compile-time constant a preprocessor conditional can
 * test, and `(void)` is how C says a function takes no parameters — an empty list means
 * `unspecified` there, which is the opposite of what is meant. clang-tidy reads this header as C++
 * because it is included from C++ translation units, and each of the four checks silenced below
 * proposes C++ that no C compiler accepts; silencing exactly those four at the file level is the
 * accurate exception rather than a broad one.
 *
 * They are two directives rather than one because clang-tidy reads a NOLINT comment ONE LINE AT A
 * TIME: a check list wrapped across two lines is not parsed, and the result is a suppression that
 * silently covers everything or nothing. Two lines, each inside the column limit, cannot do that.
 */
/* NOLINTBEGIN(modernize-use-using, modernize-deprecated-headers) */
/* NOLINTBEGIN(modernize-macro-to-enum, modernize-redundant-void-arg) */
#ifndef CY_ABI_H
#define CY_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Version --------------------------------------------------------------------------------- */

/* The version this header declares. A module records it at compile time and the loader compares it
 * with what the engine exports; see `cy_module_entry` for which direction each check runs in. */
#define CY_ABI_MAJOR 1u
#define CY_ABI_MINOR 18u
#define CY_ABI_PATCH 0u

/* One comparable number, so a `#if` in a module can ask "is this at least 1.3?" without arithmetic
 * at every site. Major and minor only: a patch never changes what is callable. */
#define CY_ABI_VERSION(major, minor) (((uint32_t)(major) << 16u) | (uint32_t)(minor))
#define CY_ABI_VERSION_CURRENT CY_ABI_VERSION(CY_ABI_MAJOR, CY_ABI_MINOR)

/* Static assertion in both languages. Every struct below is asserted on both sides of the boundary:
 * the engine asserts in src/abi/src/interface.cpp, and a module asserts by including this header.
 */
#if defined(__cplusplus)
#    define CY_ABI_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#    define CY_ABI_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

/* --- Handles --------------------------------------------------------------------------------- */

/* Opaque pointers. The module never dereferences one; only the engine knows what is behind it. A
 * distinct struct tag per handle means a C compiler rejects passing an engine where a world is
 * expected, which a `void*` would not. */
typedef struct CyEngine_T* CyEngine;
typedef struct CyWorld_T* CyWorld;
typedef struct CyBehaviourType_T* CyBehaviourType;
/* An editor-service session is owned by the registered backend and opaque to every consumer. */
typedef struct CyServiceSession_T* CyServiceSession;

/* An instance the *module* owns — a Swift object, a C struct, anything. The engine stores it and
 * hands it back to the module's own vtable; it never dereferences it. */
typedef void* CyInstance;

/* An entity, flat. The 32-bit index and the 32-bit generation of `cy::ecs::Entity`, packed
 * index-low. Zero is the null entity, because a generation of zero is never issued. */
typedef uint64_t CyEntity;
#define CY_ENTITY_NULL ((CyEntity)0)

/* A component type's id within one world. Registration order is id order — see
 * `cy::ecs::ComponentRegistry` — so an id is meaningful only against the world that issued it. */
typedef uint32_t CyComponentTypeId;
#define CY_COMPONENT_TYPE_INVALID ((CyComponentTypeId)0xFFFFFFFFu)

/* --- Results --------------------------------------------------------------------------------- */

/* Why the first fifteen values are what they are: they are `cy::ErrorCode`'s enumerators, in
 * `cy::ErrorCode`'s order, so that mapping an engine error to an ABI result is a cast rather than a
 * switch that can fall out of step. src/abi/src/errors.cpp asserts each pairing individually, so a
 * reordering of either enum is a compile error rather than a silently wrong code.
 *
 * Values from 100 up are the ABI's own: failures that only exist because there is a boundary. */
typedef enum CyResult {
    CY_RESULT_OK = 0,
    CY_RESULT_UNKNOWN = 1,
    CY_RESULT_INVALID_ARGUMENT = 2,
    CY_RESULT_OUT_OF_RANGE = 3,
    CY_RESULT_NOT_FOUND = 4,
    CY_RESULT_ALREADY_EXISTS = 5,
    CY_RESULT_PERMISSION_DENIED = 6,
    CY_RESULT_UNSUPPORTED = 7,
    CY_RESULT_NOT_IMPLEMENTED = 8,
    CY_RESULT_UNAVAILABLE = 9,
    CY_RESULT_TIMEOUT = 10,
    CY_RESULT_OUT_OF_MEMORY = 11,
    CY_RESULT_BUFFER_TOO_SMALL = 12,
    CY_RESULT_IO = 13,
    CY_RESULT_INTERNAL = 14,

    /* The engine and the module disagree about the version of this table. */
    CY_RESULT_VERSION_MISMATCH = 100,
    /* A saved blob was written by a schema newer than the code asked to read it. The reload that
     * produced it must be rejected and the previous generation kept live — `native-abi`'s
     * "Incompatible reload". */
    CY_RESULT_SCHEMA_TOO_NEW = 101,
    /* The schemas are ordered correctly but the migration is not expressible. */
    CY_RESULT_SCHEMA_UNMIGRATABLE = 102,
    /* The shared library could not be opened, or did not export its declared entry symbol. */
    CY_RESULT_MODULE_LOAD_FAILED = 103
} CyResult;

/* Never null, for every value including one this build does not know. */
const char* cy_result_name(CyResult result);

/* --- Diagnostics ------------------------------------------------------------------------------ */

/* The severities the `log` entry carries. ADDED AT 1.1, AND THE REASON IS A BUG THAT SHIPPED.
 *
 * `log`'s `severity` has always been "`cy::DiagnosticSeverity`'s value", and until 1.1 there was no
 * enum here to say what those values are. The Swift overlay therefore could not generate one, so
 * CyberdyneKit hand-wrote a copy — and the copy had SIX enumerators (`trace`, `debug`, `info`,
 * `warning`, `error`, `fatal`) against the engine's three. `Log.info` put 2 on the wire, the host
 * clamped nothing because 2 is in range, and every informational line from a Swift behaviour
 * arrived in the engine's log labelled `[error]`. It ran green for a whole milestone.
 *
 * The fix is not more care in the copy; it is that there is no copy. These three enumerators are
 * generated into the overlay like everything else, and src/abi/src/interface.cpp asserts each one
 * against `cy::DiagnosticSeverity`'s own value, so a fourth level in the engine is a compile error
 * here rather than a relabelled line somewhere else. */
typedef enum CySeverity {
    CY_SEVERITY_INFO = 0,
    CY_SEVERITY_WARNING = 1,
    CY_SEVERITY_ERROR = 2
} CySeverity;

/* --- Values ---------------------------------------------------------------------------------- */

/* The kinds a value may carry. A deliberate subset of `cy::VarType` (cy/core/values/var.h) with
 * numbering of its own: that enum's own comment says its numbers are not an interchange format, and
 * this is the interchange format. Growing this enum means appending to it — see src/abi/README.md.
 */
typedef enum CyVarType {
    CY_VAR_NIL = 0,
    CY_VAR_BOOL = 1,
    CY_VAR_I64 = 2,
    CY_VAR_F32 = 3,
    CY_VAR_F64 = 4,
    CY_VAR_VEC2 = 5,
    CY_VAR_VEC3 = 6,
    CY_VAR_VEC4 = 7,
    CY_VAR_QUAT = 8,
    CY_VAR_STRING = 9, /* UTF-8, `length` bytes, not required to be NUL-terminated */
    CY_VAR_BYTES = 10,
    CY_VAR_ENTITY = 11,

    /* --- Appended at 1.1: the narrow integers a reflected engine component actually holds -------
     *
     * 1.0 carried one integer width, and every reflected type in the engine disagrees with it:
     * `cy::scene::ChildOrder` is a u32, `cy::scene::NodeState` a u8,
     * `cy::demo::Health::last_damage` a u8 behind an Enum attribute. A generated inspector that can
     * only read i64 fields can read none of them.
     *
     * THE PAYLOAD IS ALWAYS `as_i64`, AND THE TYPE TAG IS ALWAYS THE STORAGE WIDTH. A value
     * crossing the boundary is widened into the 64-bit slot — sign-extended for the signed kinds,
     * zero- extended for the unsigned ones — and narrowed back on the way in, with a range check
     * that refuses rather than truncates. So a consumer that only understands integers may treat
     * all eight kinds as one, and a consumer that writes has to be told the width, which is exactly
     * what the tag is for. */
    CY_VAR_I8 = 12,
    CY_VAR_I16 = 13,
    CY_VAR_I32 = 14,
    CY_VAR_U8 = 15,
    CY_VAR_U16 = 16,
    CY_VAR_U32 = 17,
    CY_VAR_U64 = 18,

    /* --- Appended at 1.8: the deterministic math scalar -------------------------------------
     *
     * `cy::detmath::Fixed`, Q32.32, stored as its raw `int64_t`. The payload is `as_i64` holding
     * the RAW value (value times 2^32), never a conversion through floating point; the tag names
     * the storage format, as 1.1's integer tags name a width. A float written to a field of this
     * kind is refused — with CY_RESULT_PERMISSION_DENIED, naming the field, when the session's
     * profile is `CrossPlatform` or `Lockstep` (openspec/changes/add-deterministic-math §13). */
    CY_VAR_FIXED = 19
} CyVarType;

/* The receiver owns this value and must pass it to `var_release` exactly once. Set by every
 * interface entry that returns a heap-backed `CyVar`; clear on every borrowed or inline one, so a
 * caller that releases unconditionally is correct and a caller that never releases leaks visibly
 * (see `var_live_count`). */
#define CY_VAR_FLAG_OWNED 0x1u

/* A dynamic value crossing the boundary: a type tag and a fixed-size payload, with anything larger
 * than the payload heap-allocated and reference counted by the engine.
 *
 * THE LAYOUT IS THE POINT. Four bytes of tag, four of flags, eight of length, sixteen of payload —
 * 32 bytes, alignment 8, on every platform this engine targets, because every member is either
 * fixed-width or a pointer. `tools/abi/abi_describe.py` computes that layout from the declaration
 * and src/abi/tests/test_layout.cpp asserts the compiler agrees, so the description the overlays
 * are generated from is checked against the compiler rather than trusted. */
typedef union CyVarPayload {
    bool as_bool;
    int64_t as_i64;
    double as_f64;
    float as_f32;      /* a scalar float; the typed fast paths' currency */
    float as_f32x4[4]; /* vec2 uses [0..1], vec3 [0..2], vec4 and quat all four */
    CyEntity as_entity;
    const void* as_bytes; /* string and bytes: `length` bytes at this address */
} CyVarPayload;

typedef struct CyVar {
    uint32_t type;   /* CyVarType */
    uint32_t flags;  /* CY_VAR_FLAG_* */
    uint64_t length; /* string and bytes: the byte count. Zero for every other type. */
    CyVarPayload payload;
} CyVar;

/* --- Type registration -------------------------------------------------------------------------
 *
 * A module registers component types by describing them. The description is POD and borrowed for
 * the duration of the call: the engine copies what it keeps, so a module may build a descriptor on
 * its stack. Names must outlive the registration, because the engine stores the pointer — a string
 * literal or a static buffer, never a temporary. */

typedef struct CyFieldDesc {
    uint32_t struct_size; /* sizeof(CyFieldDesc) as the caller compiled it */
    uint32_t type;        /* CyVarType */
    uint32_t offset;      /* byte offset within the component */
    uint32_t size;        /* byte size of the field */
    const char* name;     /* must outlive the registration */
} CyFieldDesc;

typedef struct CyComponentTypeDesc {
    uint32_t struct_size;
    uint32_t size;        /* sizeof(the component) */
    uint32_t alignment;   /* alignof(the component) */
    uint32_t field_count; /* entries in `fields`; may be zero */
    const char* name;     /* must outlive the registration */
    const CyFieldDesc* fields;
} CyComponentTypeDesc;

/* --- Reading the world back, added at 1.1 -------------------------------------------------------
 *
 * WHY THESE EXIST. 1.0 let a MODULE describe its own components and then address them; it gave
 * nothing that could look at a component the ENGINE registered. An editor is on the other side of
 * that: it did not write the scene's components, it has to discover them, and a generated inspector
 * is exactly "enumerate what is there, describe each field, read it, write it". Two of those four
 * halves were missing, so the whole was.
 *
 * `CyComponentInfo` is `CyComponentTypeDesc` read back rather than written down — the same facts
 * without the `fields` pointer, because the caller does not own that storage and asks for the
 * fields one at a time through `world_component_field`. */
typedef struct CyComponentInfo {
    uint32_t struct_size; /* sizeof(CyComponentInfo) as the engine wrote it */
    uint32_t size;        /* bytes per row; zero for a tag, which has no column */
    uint32_t alignment;
    uint32_t field_count; /* fields this ABI can describe — see `world_component_field` */
    const char* name;     /* the engine's own, valid for the life of the world */
} CyComponentInfo;

/* One chunk of one archetype, borrowed.
 *
 * THE ENTRY THAT WAS MISSING FROM 1.0 AND THAT EVERY BULK READER NEEDS. `CyInterface` at 1.0 could
 * reach one component of one entity per call; a Swift system's inner loop and an editor's viewport
 * both want a column. This is that column: `entities` and `data` are parallel arrays of
 * `entity_count` rows, contiguous, in the storage the ECS actually holds.
 *
 * IT IS A BORROW AND IT CARRIES ITS EPOCH FOR THE SAME REASON `CyBorrow` DOES. Chunk storage moves
 * when an entity changes archetype, so `data` is valid only until the next structural change; check
 * it with `borrow_valid` against a `CyBorrow{data, epoch}` rather than remembering when that was.
 */
typedef struct CyChunk {
    uint32_t struct_size;
    uint32_t entity_count;
    const CyEntity* entities; /* `entity_count` entities, in row order */
    void* data;               /* the requested component's column, or null for a kind with none */
    uint32_t stride;          /* bytes per row in `data`; zero when `data` is null */
    uint32_t archetype;       /* which archetype this chunk belongs to, for grouping */
    uint64_t epoch;           /* the world's structural epoch when the chunk was taken */
} CyChunk;

/* --- Asynchronous editor services, added at 1.2 -----------------------------------------------
 *
 * Payloads use independently versioned schemas. They are borrowed only for the call (`Request`)
 * or until the next poll on the same session (`Event`). Request ids are non-zero and unique within
 * a session until a terminal event has been consumed. */
typedef enum CyServiceEventKind {
    CY_SERVICE_EVENT_ACCEPTED = 0,
    CY_SERVICE_EVENT_PROGRESS = 1,
    CY_SERVICE_EVENT_COMPLETED = 2,
    CY_SERVICE_EVENT_FAILED = 3,
    CY_SERVICE_EVENT_CANCELLED = 4
} CyServiceEventKind;

typedef struct CyServiceRequest {
    uint32_t struct_size;
    uint32_t schema_version;
    uint64_t request_id;
    const char* operation; /* stable UTF-8 operation identity; NUL-terminated */
    const uint8_t* payload;
    uint64_t payload_size;
} CyServiceRequest;

typedef struct CyServiceEvent {
    uint32_t struct_size;
    uint32_t kind; /* CyServiceEventKind */
    uint64_t request_id;
    uint32_t schema_version;
    uint32_t reserved;
    const uint8_t* payload;
    uint64_t payload_size;
} CyServiceEvent;

/* --- 1.6: interface events ---------------------------------------------------------------------
 *
 * Declared here rather than with the rest of 1.6's interface types further down because the
 * behaviour vtable below takes one: an interface event is delivered to the behaviours of the entity
 * that owns the element, through `CyBehaviourVTable.ui_event`. */

/* An element of the runtime interface. CyberUI's `ElementId` flat: the 32-bit index low, the 32-bit
 * generation high. Zero is null, because a generation of zero is never issued. NOT an entity:
 * `ui-system` keeps interface elements out of the ECS world. */
typedef uint64_t CyUiElement;
#define CY_UI_ELEMENT_NULL ((CyUiElement)0)

typedef enum CyUiEventKind {
    /* A button was pressed and released with the pointer over it. */
    CY_UI_EVENT_CLICK = 0,
    /* The element took keyboard focus. */
    CY_UI_EVENT_FOCUS = 1,
    /* The element lost keyboard focus. */
    CY_UI_EVENT_BLUR = 2,
    /* --- 1.12: a widget's (see CyUiWidgetKind). For CHANGED and SUBMITTED, `position[0]` is the
     * widget's value — a slider's, a toggle's 0 or 1 — and `button` its selected index. */
    /* The player changed a widget's value, selection or text. */
    CY_UI_EVENT_CHANGED = 3,
    /* A text field's Enter, or UI.Accept on a list's selected row. */
    CY_UI_EVENT_SUBMITTED = 4,
    /* A drop-down's list or a dialog opened, or closed. */
    CY_UI_EVENT_OPENED = 5,
    CY_UI_EVENT_CLOSED = 6
} CyUiEventKind;

typedef struct CyUiEvent {
    uint32_t struct_size;
    uint32_t kind;       /* CyUiEventKind */
    CyUiElement element; /* the element the event is about */
    CyEntity owner;      /* the entity the element was created for; the receiver's own */
    float position[2];   /* the pointer, window pixels, for CLICK; zero otherwise */
    uint32_t button;     /* CY_INPUT_BUTTON_* that clicked, for CLICK; zero otherwise */
    uint32_t reserved;
} CyUiEvent;

/* --- Behaviours --------------------------------------------------------------------------------
 *
 * The vtable a module registers for one behaviour type. It is the module's code, called by the
 * engine, so every entry carries the module's own `user_data` and none of them may unwind.
 *
 * --- WHY `schema_version` IS IN THE VTABLE AND NOT SOMEWHERE ELSE ------------------------------
 *
 * Hot reload serializes every live instance through the vtable of the generation that created it,
 * then restores through the new generation's. The blob therefore carries the *old* generation's
 * schema, and only the new generation can say whether it can read it. A new module whose schema is
 * OLDER than the blob returns CY_RESULT_SCHEMA_TOO_NEW from `deserialize`, and the loader keeps the
 * previous generation live and reports — which is `native-abi`'s "Incompatible reload" scenario.
 * See cy/abi/module.h for the sequence and for why no image is ever unloaded. */
typedef struct CyBehaviourVTable {
    uint32_t struct_size;
    uint32_t schema_version; /* bumped by the module whenever its serialized shape changes */

    /* Create an instance for `entity`. Returns null on failure, having set the last error. */
    CyInstance (*create)(CyEngine engine, CyEntity entity, void* user_data);
    /* Destroy an instance. Called through the vtable of the generation that created it, always. */
    void (*destroy)(CyInstance self, void* user_data);
    /* One fixed tick. `dt` is the fixed step, never a frame's variable delta. */
    void (*fixed_update)(CyInstance self, float dt, void* user_data);

    /* Write the instance's state into `buffer`. Returns the bytes written, or — when `capacity` is
     * too small — the number of bytes required, having written nothing. A caller therefore sizes by
     * calling once with a null buffer and a capacity of zero. */
    uint32_t (*serialize)(CyInstance self, uint8_t* buffer, uint32_t capacity, void* user_data);
    /* Restore from a blob written by schema `from_schema`, migrating field by field. Returns a
     * CyResult; CY_RESULT_SCHEMA_TOO_NEW when `from_schema` exceeds this vtable's own. */
    int32_t (*deserialize)(CyInstance self, const uint8_t* buffer, uint32_t size,
                           uint32_t from_schema, void* user_data);

    /* Passed back to every entry above. The module's own; the engine never interprets it. */
    void* user_data;

    /* --- Appended at 1.3 ---------------------------------------------------------------------
     *
     * One variable-rate frame. `dt` is the frame's delta in seconds. Called during
     * CY_PHASE_FRAME_UPDATE, after the frame's fixed steps, which is the only phase in which the
     * presentation entries (pointer, camera reads) answer. Null when the behaviour has no frame
     * callback, and the engine then never schedules it for one: `swift-scripting`'s "Unimplemented
     * callback costs nothing". A module compiled before 1.3 has a shorter `struct_size`, so the
     * engine's copy leaves this null — see `register_behaviour`. */
    void (*frame_update)(CyInstance self, float dt, void* user_data);

    /* --- Appended at 1.5: the tree callbacks -------------------------------------------------
     *
     * `scene-graph-and-nodes`' transitions, driven by the scene tree's pump for an instance that is
     * attached to a NODE (see `cy::game_backend::ScriptSceneBridge`). An instance created on a bare
     * entity has no tree and receives none of them. The order is the tree's: `enter_tree` parent
     * first, `ready` child first and once per attachment, `exit_tree` child first, and `enable` /
     * `disable` on a change of the node's effective enablement. They run in the phase of the pump's
     * caller, which the frame loop calls at the frame boundary (CY_PHASE_NONE), so the same tree
     * operations dispatch the same callbacks in the same order on every run.
     *
     * Null when the behaviour does not implement the callback, and the engine then never calls it.
     * A module compiled before 1.5 has a shorter `struct_size`, so all five are null for it. */
    void (*enter_tree)(CyInstance self, void* user_data);
    void (*ready)(CyInstance self, void* user_data);
    void (*enable)(CyInstance self, void* user_data);
    void (*disable)(CyInstance self, void* user_data);
    void (*exit_tree)(CyInstance self, void* user_data);

    /* --- Appended at 1.6: the interface ------------------------------------------------------
     *
     * An event on an interface element created for this instance's entity (`CyUiElementDesc.owner`)
     * — a button's click, a focus change. Called in CY_PHASE_FRAME_UPDATE, before the frame's
     * `frame_update`, once per event per instance on that entity, in the order the events happened.
     * `event` is borrowed for the call. Null when the behaviour takes no interface events; a module
     * compiled before 1.6 has a shorter `struct_size`, so it is null for it. */
    void (*ui_event)(CyInstance self, const CyUiEvent* event, void* user_data);
} CyBehaviourVTable;

/* --- Borrowed pointers -------------------------------------------------------------------------
 *
 * `native-abi` forbids handing a module a raw pointer into ECS chunk storage that outlives a frame,
 * because that storage moves when an entity changes archetype. A borrow is therefore a pointer plus
 * the world's structural epoch at the moment it was taken, and `cy_borrow_valid` compares the two.
 *
 * The epoch is bumped by every structural change, so a borrow taken before an `add`, a `remove`, a
 * `create` or a `destroy` is detectably stale afterwards — in every configuration, not only in
 * development builds, because the check is a comparison rather than an assertion. */
typedef struct CyBorrow {
    void* data;     /* null when the entity does not have that component */
    uint64_t epoch; /* the world's structural epoch when this borrow was taken */
} CyBorrow;

/* === 1.3: THE GAME SERVICES ======================================================================
 *
 * WHY THESE EXIST. Up to 1.2 the table carried only the engine-neutral core, and a game reached
 * input, physics, cameras, navigation and audio through components a C++ host carried across for it
 * (samples/04-character/game/Contract.swift says so in as many words). An RTS cannot be written
 * that way: selecting a unit is a pointer read, a camera ray and a physics query in one frame, and
 * ordering it is a path request and a crowd move. So the servers' gameplay-facing verbs are
 * appended here, each as values in and values or caller-owned buffers out.
 *
 * --- THE RULES EVERY 1.3 ENTRY FOLLOWS, STATED ONCE -------------------------------------------
 *
 *  * NO ENGINE POINTER ESCAPES. Every result is a value, a struct the caller owns, or a buffer
 *    the caller supplied. Every service handle below (`CyAudioVoice`, `CyNavQuery` and the rest)
 *    is an integer the engine resolves and generation-checks, never an address. Strings passed in
 *    are borrowed for the call.
 *  * ERRORS ARE `CyResult`. CY_RESULT_INVALID_ARGUMENT for a null engine, a null required pointer
 *    or a malformed struct; CY_RESULT_PERMISSION_DENIED for a call in a phase its entry does not
 *    allow; CY_RESULT_UNAVAILABLE when the embedder bound no backend for that service, or the
 *    service cannot answer now; CY_RESULT_NOT_FOUND for an unknown name or a stale handle. The
 *    out-parameter is left untouched on failure unless the entry says otherwise.
 *  * `struct_size` IS SET BY THE CALLER on every struct that has one, in and out. The engine reads
 *    (or writes) only the prefix both sides know, so a module compiled against 1.3 keeps working
 *    when these structs grow. Zero means "the size this header declares". Structs WITHOUT a
 *    `struct_size` are passed in arrays; they are fixed forever and grow only by a new struct and a
 *    new entry.
 *  * PHASES. Each entry names the phases it may be called in — `N` (CY_PHASE_NONE: module
 *    initialisation, a frame boundary, a tool), `F` (CY_PHASE_FIXED_UPDATE), `U`
 *    (CY_PHASE_FRAME_UPDATE) — and refuses the others with CY_RESULT_PERMISSION_DENIED in EVERY
 *    build, because the check is a comparison and a rule that holds in two configurations is not a
 *    rule. `time_get` reports the current phase.
 *  * DETERMINISM. Anything callable in `F` answers from simulation state only: the same inputs,
 *    the same tick and the same world give the same answer, bit for bit, with every multi-result
 *    list in a stated total order. Device state (the pointer, modifier keys) and presentation
 *    state (the camera) are therefore NOT callable in `F`; a game turns them into simulation input
 *    by writing a command during `U`.
 *  * THREADS. The game thread, unless an entry says otherwise. The physics queries and
 *    `nav_find_path` are also safe from a job worker running inside an `F` or `U` stage, because
 *    both are const queries over state nothing mutates during that stage.
 *  * RELOAD. Service handles are engine state, so they stay valid across a module hot reload. They
 *    do not survive the world or level that issued them; afterwards they answer NOT_FOUND. */

/* The update phase the engine is in. Not a stage: behaviours have no stage, and several stages
 * share one phase. CY_STAGE_PRE_SIMULATION to CY_STAGE_POST_SIMULATION and `fixed_update` run in
 * FIXED_UPDATE; CY_STAGE_FRAME to CY_STAGE_UI and `frame_update` in FRAME_UPDATE; everything else,
 * including CY_STAGE_RENDER, is NONE. */
typedef enum CyPhase {
    CY_PHASE_NONE = 0,
    CY_PHASE_FIXED_UPDATE = 1,
    CY_PHASE_FRAME_UPDATE = 2
} CyPhase;

/* A position and an orientation. `rotation` is a unit quaternion, x y z w. An all-zero quaternion
 * is read as the identity, so a zeroed struct is a valid pose at the origin. */
typedef struct CyPose {
    float position[3];
    float rotation[4];
} CyPose;

/* A ray. `direction` is expected to be unit length, so distances are metres. */
typedef struct CyRay {
    float origin[3];
    float direction[3];
    float max_distance;
} CyRay;

/* --- 1.3: time ------------------------------------------------------------------------------- */

/* The tick being simulated is a resimulation (rollback or replay catch-up). Presentation side
 * effects — audio, camera writes — issued during it are accepted and dropped. */
#define CY_TIME_RESIMULATING 0x1u
/* The simulation is paused; frames still run. */
#define CY_TIME_PAUSED 0x2u

typedef struct CyTime {
    uint32_t struct_size;
    uint32_t phase; /* CyPhase */
    /* In F, the tick being simulated. Otherwise the last committed tick. */
    uint64_t tick;
    double fixed_delta; /* seconds per fixed step */
    /* Seconds since the previous frame. ZERO IN F, so a fixed step cannot come to depend on it. */
    double frame_delta;
    /* How far between the last committed tick and the next the frame is, in [0, 1). Zero in F. */
    double interpolation;
    uint32_t flags; /* CY_TIME_* */
    uint32_t reserved;
} CyTime;

/* --- 1.3: input -------------------------------------------------------------------------------
 *
 * Actions are read from the state the input server RESOLVED FOR A TICK — the same state the
 * committed command frame was built from — so an `F` read replays exactly. The pointer and the
 * modifier keys are device state and are `U` only. */

/* A declared action's dense runtime index. Resolve once by name; it is stable for the process. */
typedef uint32_t CyInputAction;
#define CY_INPUT_ACTION_INVALID ((CyInputAction)0xFFFFFFFFu)

/* A registered mapping context. Zero is null. */
typedef uint64_t CyInputContext;
#define CY_INPUT_CONTEXT_NULL ((CyInputContext)0)

#define CY_INPUT_ACTION_PRESSED 0x1u       /* actuated at the end of the tick */
#define CY_INPUT_ACTION_JUST_PRESSED 0x2u  /* went down at least once during the tick */
#define CY_INPUT_ACTION_JUST_RELEASED 0x4u /* came up at least once during the tick */
#define CY_INPUT_ACTION_TRIGGERED 0x8u     /* the action's trigger fired during the tick */
#define CY_INPUT_ACTION_SYNTHETIC 0x10u    /* the value came from injection or replay */

typedef struct CyInputActionState {
    uint32_t struct_size;
    uint32_t flags; /* CY_INPUT_ACTION_* */
    /* Digital: x is 0 or 1. One axis: x. Two axes: x y. Three: x y z. */
    float value[3];
    /* Transitions during the tick. A press and a release inside one tick are 1 and 1. */
    uint16_t press_count;
    uint16_t release_count;
    uint64_t tick; /* the tick this state was resolved for */
} CyInputActionState;

#define CY_INPUT_BUTTON_LEFT 0x1u
#define CY_INPUT_BUTTON_RIGHT 0x2u
#define CY_INPUT_BUTTON_MIDDLE 0x4u
#define CY_INPUT_BUTTON_EXTRA1 0x8u
#define CY_INPUT_BUTTON_EXTRA2 0x10u

#define CY_INPUT_POINTER_PRESENT 0x1u   /* the user has a pointing device */
#define CY_INPUT_POINTER_IN_WINDOW 0x2u /* the pointer is inside the window's client area */
#define CY_INPUT_POINTER_OVER_UI 0x4u   /* an interface layer holds pointer focus */

typedef struct CyInputPointer {
    uint32_t struct_size;
    uint32_t flags;            /* CY_INPUT_POINTER_* */
    uint32_t buttons;          /* CY_INPUT_BUTTON_* held now */
    uint32_t buttons_pressed;  /* went down since the previous frame update */
    uint32_t buttons_released; /* came up since the previous frame update */
    float position[2];         /* window pixels, origin top-left, +y down */
    float delta[2];            /* pixels moved since the previous frame update */
    float wheel[2];            /* notches since the previous frame update; x horizontal */
    uint32_t reserved;
} CyInputPointer;

#define CY_INPUT_MOD_SHIFT 0x1u
#define CY_INPUT_MOD_CTRL 0x2u
#define CY_INPUT_MOD_ALT 0x4u
#define CY_INPUT_MOD_SUPER 0x8u

/* --- 1.3: camera ------------------------------------------------------------------------------
 *
 * The camera is presentation. Reading it is `U` only; writing it is allowed in `F` too because
 * nothing in the simulation reads it back. */

/* A camera rig. Zero is null. */
typedef uint64_t CyCamera;
#define CY_CAMERA_NULL ((CyCamera)0)

#define CY_CAMERA_VIEW_ORTHOGRAPHIC 0x1u

typedef struct CyCameraView {
    uint32_t struct_size;
    uint32_t flags;     /* CY_CAMERA_VIEW_* */
    CyPose pose;        /* the evaluated camera, world space */
    float vertical_fov; /* radians; zero when orthographic */
    float ortho_height; /* metres; zero when perspective */
    float near_plane;   /* metres */
    float far_plane;    /* metres */
    float viewport[4];  /* x, y, width, height in window pixels */
} CyCameraView;

#define CY_SCREEN_POINT_ON_SCREEN 0x1u /* inside the viewport and in front of the near plane */
#define CY_SCREEN_POINT_BEHIND 0x2u    /* behind the camera; `position` is mirrored and unusable */

/* Passed in arrays, so fixed forever: no `struct_size`. */
typedef struct CyScreenPoint {
    float position[2]; /* window pixels, the same space `camera_screen_to_ray` takes */
    float depth;       /* metres along the camera's forward axis; negative behind */
    uint32_t flags;    /* CY_SCREEN_POINT_* */
} CyScreenPoint;

#define CY_CAMERA_TARGET_FOLLOW_ENTITY 0x1u /* frame `entity`; otherwise frame `position` */

/* What an RTS camera is told: a focus, an angle round it and a distance from it. The rig decides
 * where that puts the camera; `camera_set_pose` bypasses the rig entirely. */
typedef struct CyCameraTarget {
    uint32_t struct_size;
    uint32_t flags; /* CY_CAMERA_TARGET_* */
    CyEntity entity;
    float position[3];   /* the focus point, world space, when not following an entity */
    float yaw;           /* radians about world +Y */
    float pitch;         /* radians; negative looks down */
    float distance;      /* metres from the focus */
    float blend_seconds; /* zero is a cut */
    uint32_t reserved;
} CyCameraTarget;

/* --- 1.3: physics queries ------------------------------------------------------------------ */

typedef enum CyShapeKind {
    CY_SHAPE_SPHERE = 0,
    CY_SHAPE_CAPSULE = 1,
    CY_SHAPE_BOX = 2
} CyShapeKind;

/* A query shape. Local +Y is a capsule's axis. */
typedef struct CyShape {
    uint32_t kind;         /* CyShapeKind */
    float radius;          /* sphere and capsule */
    float half_height;     /* capsule: half the cylindrical section, excluding the caps */
    float half_extents[3]; /* box */
} CyShape;

/* The defaults are the zero bits: triggers excluded, back faces culled, every motion type hit. */
#define CY_QUERY_INCLUDE_TRIGGERS 0x1u
#define CY_QUERY_HIT_BACK_FACES 0x2u
#define CY_QUERY_SKIP_STATIC 0x4u
#define CY_QUERY_SKIP_KINEMATIC 0x8u
#define CY_QUERY_SKIP_DYNAMIC 0x10u

/* The querying "collider": its layer, the layers it hits, and bodies to skip. Filtered mutually and
 * through the project's collision matrix, exactly as a contact is. A null filter is layer 0, every
 * layer, no flags, nothing ignored. */
typedef struct CyQueryFilter {
    uint32_t struct_size;
    uint32_t layer;         /* 0 to 31 */
    uint32_t mask;          /* a bit per layer; 0xFFFFFFFF hits everything */
    uint32_t flags;         /* CY_QUERY_* */
    const CyEntity* ignore; /* borrowed for the call; the bodies of these entities are skipped */
    uint32_t ignore_count;
    uint32_t reserved;
} CyQueryFilter;

#define CY_HIT_TRIGGER 0x1u             /* the body is a sensor */
#define CY_HIT_STARTED_PENETRATING 0x2u /* a sweep that overlapped at its start */

/* Passed in arrays, so fixed forever: no `struct_size`. */
typedef struct CyPhysicsHit {
    uint32_t flags; /* CY_HIT_* */
    uint32_t reserved;
    CyEntity entity; /* the entity that owns the body; CY_ENTITY_NULL for a body with none */
    float point[3];  /* world space */
    float normal[3]; /* unit, out of the surface hit */
    float distance;  /* metres travelled before the hit */
    float fraction;  /* distance / max_distance */
} CyPhysicsHit;

/* --- 1.3: navigation ------------------------------------------------------------------------ */

/* An asynchronous path request. Zero is null. */
typedef uint64_t CyNavQuery;
#define CY_NAV_QUERY_NULL ((CyNavQuery)0)

/* `cy::navigation::NavPathStatus`, value for value. */
typedef enum CyNavPathStatus {
    CY_NAV_PATH_STATUS_IDLE = 0,
    CY_NAV_PATH_STATUS_COMPUTING = 1,
    CY_NAV_PATH_STATUS_FOLLOWING = 2,
    CY_NAV_PATH_STATUS_ARRIVED = 3,
    CY_NAV_PATH_STATUS_FAILED = 4
} CyNavPathStatus;

/* `cy::navigation::QueryState`, value for value. */
typedef enum CyNavQueryState {
    CY_NAV_QUERY_PENDING = 0,
    CY_NAV_QUERY_READY = 1,
    CY_NAV_QUERY_CONSUMED = 2,
    CY_NAV_QUERY_CANCELLED = 3
} CyNavQueryState;

/* Zero means "the default" for every field after the endpoints, so a zeroed request is valid. */
typedef struct CyNavPathRequest {
    uint32_t struct_size;
    uint32_t world; /* the navigation world; 0 is the default one */
    float start[3];
    float end[3];
    float extents[3];      /* half-extents the endpoints are snapped within; zero: the default */
    uint32_t node_budget;  /* A* expansions before the best partial result; zero: the default */
    uint64_t area_mask;    /* traversable area types; zero: all of them */
    uint64_t capabilities; /* off-mesh link capabilities; zero: all of them */
} CyNavPathRequest;

#define CY_NAV_PATH_FOUND 0x1u
#define CY_NAV_PATH_PARTIAL 0x2u /* ends at the reachable point closest to the target */
#define CY_NAV_PATH_BUDGET_EXCEEDED 0x4u

typedef struct CyNavPathResult {
    uint32_t struct_size;
    uint32_t flags;       /* CY_NAV_PATH_* */
    uint32_t point_count; /* points in the whole straightened path, which may exceed the buffer */
    uint32_t state;       /* CyNavQueryState; READY for a synchronous query */
    float cost;           /* the search's path cost */
    float length;         /* metres along the straightened path */
} CyNavPathResult;

/* The agent parameters `navigation` names. Zero means "the default" for every field. */
typedef struct CyNavAgentParams {
    uint32_t struct_size;
    uint32_t world;
    float radius;
    float height;
    float max_speed;
    float max_acceleration;
    float arrival_distance;
    uint32_t priority; /* higher yields less */
    uint64_t area_mask;
    uint64_t capabilities;
} CyNavAgentParams;

/* Set for the one tick after the agent arrived or failed. */
#define CY_NAV_AGENT_EVENT 0x1u

typedef struct CyNavAgentState {
    uint32_t struct_size;
    uint32_t status; /* CyNavPathStatus */
    uint32_t flags;  /* CY_NAV_AGENT_* */
    uint32_t reserved;
    float position[3];
    float velocity[3];
    float target[3];
    float remaining_distance; /* metres along the path still to go; zero when idle */
} CyNavAgentState;

/* --- 1.3: audio ------------------------------------------------------------------------------
 *
 * Audio is presentation: it never feeds back into the simulation, so it is callable in every phase,
 * and a voice handle is not simulation state — a fixed step must not branch on one. */

typedef uint64_t CyAudioCue;   /* an authored cue or clip; zero is null */
typedef uint64_t CyAudioBus;   /* a bus of the mix graph; zero is null */
typedef uint64_t CyAudioVoice; /* one playing instance; zero is null */

#define CY_AUDIO_PLAY_LOOP 0x1u
#define CY_AUDIO_PLAY_SPATIAL 0x2u /* positional at `position`; otherwise non-spatial */
#define CY_AUDIO_PLAY_ATTACH 0x4u  /* follow `attach_to`, with `position` as the offset */

/* Zero means "as authored" for `bus`, `volume` and `pitch`, so a zeroed request plays the cue. */
typedef struct CyAudioPlay {
    uint32_t struct_size;
    uint32_t flags; /* CY_AUDIO_PLAY_* */
    CyAudioCue cue;
    CyEntity attach_to;
    CyAudioBus bus;
    float position[3];
    float volume; /* linear gain */
    float pitch;  /* playback-rate ratio */
    float fade_in_seconds;
} CyAudioPlay;

/* --- 1.3: spawning ---------------------------------------------------------------------------- */

/* A resolved prefab or scene asset. Zero is null. */
typedef uint64_t CyPrefab;
#define CY_PREFAB_NULL ((CyPrefab)0)

typedef struct CySpawnParams {
    uint32_t struct_size;
    uint32_t flags;  /* none yet; zero */
    CyEntity parent; /* CY_ENTITY_NULL makes the instance a root */
    CyPose pose;     /* relative to `parent` */
    float scale[3];  /* all zero is read as one */
} CySpawnParams;

/* === 1.5: SCHEDULED SYSTEMS, NODE PATHS, BODIES AND CHARACTERS =================================
 *
 * WHY THESE EXIST. M12 is an RTS written in Swift, and three things it needs were still missing at
 * 1.4: a Swift `@System` could not be scheduled (it ran only in the package's own tests), the tree
 * callbacks and `@Node(path)` were declared and never driven, and physics was queries only. The
 * rules stated above `CyPhase` hold for every entry below. */

/* --- 1.5: scheduled systems ---------------------------------------------------------------------
 */

/* What a system declares about one component. `cy::jobs::Access` in the scheduler's terms. */
typedef enum CyAccessMode {
    CY_ACCESS_READ = 0,
    CY_ACCESS_WRITE = 1,
    /* Narrows the match and declares no access, so it never conflicts. */
    CY_ACCESS_EXCLUDE = 2
} CyAccessMode;

/* Passed in arrays, so fixed forever: no `struct_size`. */
typedef struct CySystemAccess {
    CyComponentTypeId component;
    uint32_t mode; /* CyAccessMode */
} CySystemAccess;

/* A system a module asks the engine to schedule. THE ACCESS LIST IS THE DECLARATION the scheduler
 * orders the system by, against native systems, by the same conflict rules; the module derives it
 * from the query it iterates (Swift: the `Query<...>` in the function's signature), so the two
 * cannot drift. Borrowed for the call except `name`, which must outlive the registration. */
typedef struct CySystemDesc {
    uint32_t struct_size;
    uint32_t stage;   /* CyStage */
    const char* name; /* unique per engine; the identity a reload matches by */
    const CySystemAccess* access;
    uint32_t access_count;
    uint32_t reserved;
    /* The body. Called once per run of the stage, possibly on a job worker, in the phase the stage
     * belongs to. The world is iterating while it runs, so every structural entry answers
     * UNAVAILABLE; bulk data is reached through `world_chunks`, whose columns the body may read
     * (and write, where it declared WRITE) through the borrowed pointers until it returns. */
    void (*run)(CyEngine engine, CyWorld world, void* user_data);
    void* user_data; /* passed back to `run`; the module's own */
} CySystemDesc;

/* --- 1.5: characters ----------------------------------------------------------------------------
 */

/* `cy::physics::GroundState`, value for value. */
typedef enum CyGroundState {
    CY_GROUND_GROUNDED = 0,
    CY_GROUND_STEEP_SLOPE = 1,
    CY_GROUND_IN_AIR = 2
} CyGroundState;

#define CY_CHARACTER_FLOATING 0x1u /* six degrees of freedom: no gravity, no ground, no steps */
#define CY_CHARACTER_NO_PUSH 0x2u  /* do not push dynamic bodies */

/* A capsule character. Zero means "the default" for every field, so a zeroed struct is a 0.3 m by
 * 1.8 m grounded character at the origin on layer 0 hitting every layer. */
typedef struct CyCharacterDesc {
    uint32_t struct_size;
    uint32_t flags;          /* CY_CHARACTER_* */
    float radius;            /* metres */
    float height;            /* metres, total, both caps included */
    float max_slope_radians; /* steeper is a wall */
    float step_offset;       /* the tallest step climbed rather than blocked */
    float skin_width;
    float gravity_scale;
    float mass;       /* kilograms, what a pushed body is pushed with */
    float push_force; /* newtons */
    uint32_t layer;   /* 0 to 31 */
    uint32_t mask;    /* layers the capsule collides with; zero: every layer */
    CyPose start;
} CyCharacterDesc;

#define CY_CHARACTER_JUMP 0x1u /* replace the vertical velocity with `jump_speed` this step */

typedef struct CyCharacterInput {
    uint32_t struct_size;
    uint32_t flags;            /* CY_CHARACTER_JUMP */
    float desired_velocity[3]; /* metres per second, world space */
    float jump_speed;          /* metres per second, with CY_CHARACTER_JUMP */
} CyCharacterInput;

#define CY_CHARACTER_TOUCHING_CEILING 0x1u
#define CY_CHARACTER_TOUCHING_WALL 0x2u
#define CY_CHARACTER_STEPPED_UP 0x4u /* lifted onto a stair during the last move */

typedef struct CyCharacterState {
    uint32_t struct_size;
    uint32_t ground; /* CyGroundState */
    uint32_t flags;  /* CY_CHARACTER_TOUCHING_* and CY_CHARACTER_STEPPED_UP */
    uint32_t reserved;
    CyEntity ground_entity; /* the entity owning the ground body; null for none */
    float position[3];      /* the capsule's centre, world space */
    float velocity[3];      /* after gravity, collisions and the platform */
    float ground_normal[3];
    float platform_velocity[3]; /* the platform's share of the motion, per second */
} CyCharacterState;

/* === 1.6: THE RUNTIME INTERFACE ================================================================
 *
 * WHY THESE EXIST. CyberUI reached the screen at #102 — the element store, layout, text in the
 * built-in font and the interface pass — and a Swift game could not touch any of it: no entry
 * reached `cy::ui`, so a HUD had to be written in C++ by the host. These entries let a module build
 * and update an interface tree, receive its buttons' clicks, and ask what the pointer is over.
 *
 * THE RULES ABOVE `CyPhase` HOLD, with one addition: THE INTERFACE IS PRESENTATION. Every 1.6
 * entry is `[N U]` — module initialisation and the frame — and a fixed step is refused with
 * PERMISSION_DENIED, because nothing in the simulation may depend on an element and a resimulated
 * tick must not build one twice. A module reaches only the elements it created (and the root): an
 * element the embedder built itself, such as the developer console, answers NOT_FOUND.
 *
 * COLOURS are premultiplied 0xAARRGGBB, as everywhere in CyberUI. LENGTHS are reference units,
 * which are window pixels under the embedder's fixed-pixel scale. The cy::ui store and its three
 * dirty states are behind every entry; a write marks what it changed, and layout runs once per
 * frame in the embedder, so a module that changes a number repaints one label and relays out
 * nothing. */

/* What an element is. The kind decides which writes it takes. */
typedef enum CyUiKind {
    /* A box: a background, a border, children. A layout container when it draws nothing. */
    CY_UI_PANEL = 0,
    /* A line of text in the built-in font; `ui_set_text`. Measured from its text. */
    CY_UI_LABEL = 1,
    /* An atlas page drawn through the image material; `ui_set_image`. */
    CY_UI_IMAGE = 2,
    /* A track (`background`) and a fill (`accent`) covering `ui_set_progress`'s fraction of it. */
    CY_UI_PROGRESS = 3,
    /* A focusable panel with text whose clicks are delivered to its owner as CY_UI_EVENT_CLICK. */
    CY_UI_BUTTON = 4
} CyUiKind;

/* How an element lays out its children. `cy::ui::LayoutModel`, value for value. */
typedef enum CyUiLayoutModel {
    CY_UI_LAYOUT_FLEX = 0,
    CY_UI_LAYOUT_GRID = 1,
    /* Children placed by anchors and offsets within this element. */
    CY_UI_LAYOUT_ABSOLUTE = 2
} CyUiLayoutModel;

/* `cy::ui::FlexDirection`, value for value. */
typedef enum CyUiDirection {
    CY_UI_DIRECTION_ROW = 0,
    CY_UI_DIRECTION_COLUMN = 1,
    CY_UI_DIRECTION_ROW_REVERSE = 2,
    CY_UI_DIRECTION_COLUMN_REVERSE = 3
} CyUiDirection;

/* `cy::ui::Justify`, value for value: distribution along the main axis. */
typedef enum CyUiJustify {
    CY_UI_JUSTIFY_START = 0,
    CY_UI_JUSTIFY_CENTRE = 1,
    CY_UI_JUSTIFY_END = 2,
    CY_UI_JUSTIFY_SPACE_BETWEEN = 3,
    CY_UI_JUSTIFY_SPACE_AROUND = 4,
    CY_UI_JUSTIFY_SPACE_EVENLY = 5
} CyUiJustify;

/* Alignment across the cross axis. NOT `cy::ui::Align`'s order: STRETCH, the engine's default, is
 * zero here, so a zeroed `CyUiLayout` aligns as an element the module never laid out does. */
typedef enum CyUiAlign {
    CY_UI_ALIGN_STRETCH = 0,
    CY_UI_ALIGN_START = 1,
    CY_UI_ALIGN_CENTRE = 2,
    CY_UI_ALIGN_END = 3
} CyUiAlign;

/* Shown, hidden (drawn nowhere, not hit), or collapsed (also out of layout: CSS's display none). */
typedef enum CyUiVisibility {
    CY_UI_VISIBLE = 0,
    CY_UI_HIDDEN = 1,
    CY_UI_COLLAPSED = 2
} CyUiVisibility;

/* What `ui_create` makes. `name` is the element's type name — what a `.cyss` type selector and a
 * diagnostic read — and is copied (interned); null or empty is the kind's own name. */
typedef struct CyUiElementDesc {
    uint32_t struct_size;
    uint32_t kind;    /* CyUiKind */
    const char* name; /* borrowed for the call */
    CyEntity owner;   /* whose behaviours receive the element's events; null for nobody */
} CyUiElementDesc;

#define CY_UI_LAYOUT_WRAP 0x1u      /* flex children wrap onto further lines */
#define CY_UI_LAYOUT_NO_SHRINK 0x2u /* a flex child never shrinks below its size */

/* An element's layout input: how it lays out its children, and where its parent puts it. ZERO
 * MEANS THE DEFAULT for every field, so a zeroed struct is a stretching flex row of content-sized
 * elements: a zero `preferred` axis is measured from the content, a zero `maximum` axis is
 * unbounded, a zero `flex_shrink` is 1 (CY_UI_LAYOUT_NO_SHRINK is zero), and a zero span is 1. */
typedef struct CyUiLayout {
    uint32_t struct_size;
    uint32_t model;     /* CyUiLayoutModel, for this element's children */
    uint32_t direction; /* CyUiDirection */
    uint32_t justify;   /* CyUiJustify */
    uint32_t align;     /* CyUiAlign: the children across the cross axis */
    uint32_t
        self_align; /* CyUiAlign: this element in a flex parent; STRETCH defers to the parent */
    uint32_t flags; /* CY_UI_LAYOUT_* */
    float gap;      /* between flex children and grid tracks */
    float preferred[2];
    float minimum[2];
    float maximum[2];
    float margin[4];  /* left, top, right, bottom */
    float padding[4]; /* left, top, right, bottom */
    float flex_grow;
    float flex_shrink;
    float aspect_ratio; /* width over height; zero: unconstrained */
    /* In an ABSOLUTE parent: the corners as fractions of the parent's rect, then offset in units.
     * (0,0)-(1,1) with zero offsets fills the parent; (1,1)-(1,1) offset by (-100,-80)-(-6,-6)
     * is a 94 by 74 box six units in from the bottom-right corner. */
    float anchor_min[2];
    float anchor_max[2];
    float offset_min[2];
    float offset_max[2];
    /* In a GRID parent: the cell and its spans. On a grid container: its column count. */
    uint16_t grid_column;
    uint16_t grid_row;
    uint16_t grid_column_span;
    uint16_t grid_row_span;
    uint16_t grid_columns;
    uint16_t reserved;
} CyUiLayout;

#define CY_UI_STYLE_CLIP_CHILDREN 0x1u /* children are clipped to this element's rect */

/* What an element draws. Zero colours draw nothing, so a zeroed style is an invisible container. */
typedef struct CyUiStyle {
    uint32_t struct_size;
    uint32_t flags;         /* CY_UI_STYLE_* */
    uint32_t background;    /* the fill; an image's tint */
    uint32_t border_colour; /* drawn inside the bounds, `border_width` thick */
    uint32_t accent;        /* a progress bar's fill */
    float border_width;
    float corner_radius; /* rounds the fill, the border, an image and a progress bar's fill alike */
    uint32_t reserved;
} CyUiStyle;
/* --- 1.7: animation -----------------------------------------------------------------------------
 *
 * `cy::animation::AnimationSystem` from a game: an entity is given an animator over a rig the host
 * registered by name, a script plays and crossfades its states, raises its program's parameters,
 * reads the events its clips fired and takes the root motion they extracted. Everything that
 * changes what a character does is simulation (`N F`); what it looks like is read in any phase.
 *
 * NAMES ARE HASHED, NOT POINTED AT. No engine pointer escapes a 1.3 entry, so a state or an event
 * comes back as `CY_NAME_HASH` of its name — FNV-1a, 64 bits, over the name's UTF-8 bytes, with the
 * constants below — which a module computes from its own string and compares. `cy::abi::name_hash`
 * and CyberdyneKit's `AnimationName` are the two implementations, and `unit.abi` holds them to the
 * same values. */

#define CY_NAME_HASH_OFFSET 14695981039346656037ull
#define CY_NAME_HASH_PRIME 1099511628211ull

/* Where an animator's extracted root motion goes. The first four are
 * `cy::animation::RootMotionMode`, value for value. */
typedef enum CyRootMotionMode {
    CY_ROOT_MOTION_IGNORE = 0,     /* computed and thrown away; gameplay owns the movement */
    CY_ROOT_MOTION_TRANSFORM = 1,  /* composed onto the entity's local transform each tick */
    CY_ROOT_MOTION_ACCUMULATE = 2, /* accumulated until `animation_take_root_motion` */
    CY_ROOT_MOTION_EXTRACT = 3,    /* the last tick's delta, reported and applied nowhere */
    CY_ROOT_MOTION_CHARACTER = 4   /* fed to the entity's character controller every tick */
} CyRootMotionMode;

/* `cy::animation::LodTier`, value for value: how often a pose is evaluated, at what bone detail,
 * and at BAKED never. Every tier is advanced every tick, so root motion and events do not depend
 * on it. */
typedef enum CyAnimationTier {
    CY_ANIMATION_TIER_FULL = 0,
    CY_ANIMATION_TIER_SIMPLIFIED = 1,
    CY_ANIMATION_TIER_CACHED = 2,
    CY_ANIMATION_TIER_BAKED = 3
} CyAnimationTier;

#define CY_ANIMATOR_SUPPRESS_EVENTS 0x1u /* count event crossings and emit none */

/* An animator. `rig` is borrowed for the call and names a rig the host registered. Zero is the
 * default for every other field: full tier, events emitted, root motion ignored, play rate 1. */
typedef struct CyAnimatorDesc {
    uint32_t struct_size;
    uint32_t flags; /* CY_ANIMATOR_SUPPRESS_EVENTS */
    const char* rig;
    uint32_t tier;        /* CyAnimationTier */
    uint32_t root_motion; /* CyRootMotionMode */
    float play_rate;      /* zero is 1 */
    uint32_t reserved;
} CyAnimatorDesc;

/* A blend is in flight; `target` and `blend_weight` say where to and how far. */
#define CY_ANIMATOR_BLENDING 0x1u
/* That blend was requested by `animation_play` or `animation_stop`. */
#define CY_ANIMATOR_REQUESTED 0x2u

/* Where an animator's state machine is. */
typedef struct CyAnimatorState {
    uint32_t struct_size;
    uint32_t flags;     /* CY_ANIMATOR_* */
    uint64_t state;     /* CY_NAME_HASH of the state it is in */
    uint64_t target;    /* CY_NAME_HASH of the state a blend heads to; 0 with none in flight */
    float blend_weight; /* 0 to 1; 0 with no blend in flight */
    float state_time;   /* seconds since it entered `state` */
} CyAnimatorState;

/* One event a clip fired. Passed in arrays, so fixed forever: no `struct_size`. */
typedef struct CyAnimationEvent {
    CyEntity entity;
    uint64_t name;         /* CY_NAME_HASH of the event's name */
    float normalised_time; /* where in its clip the event sits, 0 to 1 */
    float parameter;       /* the event's authored payload */
} CyAnimationEvent;

/* Root motion: a delta in the character's own frame, plus the running total. */
typedef struct CyRootMotion {
    uint32_t struct_size;
    uint32_t contacts;    /* bit 0 the left foot down, bit 1 the right */
    float translation[3]; /* metres */
    float rotation[4];    /* a unit quaternion, x y z w */
    float distance;       /* the motion curve's distance over the interval, metres */
    float travelled[3];   /* everything since the animator was attached, metres */
} CyRootMotion;

/* --- 1.8: deterministic math and the lockstep path ----------------------------------------------
 *
 * openspec/changes/add-deterministic-math, stage 8 (design §13). A fixed-point value crosses as its
 * RAW integer and is never converted through floating point on the way:
 *
 *   CyFixed   `cy::detmath::Fixed`, Q32.32: the value times 2^32, two's complement
 *   CyAngle   `cy::detmath::Angle`: raw / 2^32 of a turn, counter-clockwise, wrapping
 *
 * The arithmetic is a module's own (CyberdyneKit's `Fixed` states the engine's rules bit for bit);
 * the TRANSCENDENTALS are the engine's, reached through `detmath_*`, so a module never carries a
 * second copy of a polynomial. Every `detmath_*` entry is a pure function of its arguments, legal
 * in every phase, and defined for every input: out of domain it answers what
 * src/core/detmath/include/cy/core/detmath/functions.h states.
 *
 * THE LOCKSTEP PATH. `lockstep_*` reach a fixed-point session the host runs
 * (`cy::abi::game::LockstepBackend`, implemented by `cy::game_backend::LockstepAdapter` over
 * `cy::movement` and a `cy::gameplay::CommandStream` under `Lockstep`): units are enlisted before
 * the first tick, an ORDER is recorded into the session's command stream as a command whose payload
 * is raw `CyFixed` values, and every peer executes the same log. Whatever produced the order — a
 * click converted once by the issuing peer, or arithmetic in `CyFixed` — the session reads the same
 * bits on every architecture. */

typedef int64_t CyFixed;
typedef uint32_t CyAngle;

typedef struct CyFixedVec2 {
    CyFixed x;
    CyFixed y; /* the lockstep mover reads it as world Z */
} CyFixedVec2;

typedef struct CyFixedVec3 {
    CyFixed x;
    CyFixed y;
    CyFixed z;
} CyFixedVec3;

typedef struct CyFixedQuat {
    CyFixed x;
    CyFixed y;
    CyFixed z;
    CyFixed w;
} CyFixedQuat;

/* The transcendentals `detmath_evaluate` applies over a span. A span is of `CyFixed` slots: an
 * `Angle` argument is the low 32 bits of its slot and an `Angle` result is zero-extended into one.
 */
typedef enum CyDetmathFunction {
    CY_DETMATH_SQRT = 0,  /* Fixed -> Fixed, correctly rounded */
    CY_DETMATH_SIN = 1,   /* Angle -> Fixed */
    CY_DETMATH_COS = 2,   /* Angle -> Fixed */
    CY_DETMATH_TAN = 3,   /* Angle -> Fixed */
    CY_DETMATH_ATAN = 4,  /* Fixed -> Angle */
    CY_DETMATH_ATAN2 = 5, /* (y Fixed, x Fixed) -> Angle */
    CY_DETMATH_ASIN = 6,  /* Fixed -> Angle */
    CY_DETMATH_ACOS = 7,  /* Fixed -> Angle */
    CY_DETMATH_EXP2 = 8,  /* Fixed -> Fixed */
    CY_DETMATH_LOG2 = 9,  /* Fixed -> Fixed */
    CY_DETMATH_EXP = 10,  /* Fixed -> Fixed */
    CY_DETMATH_LOG = 11,  /* Fixed -> Fixed */
    CY_DETMATH_POW = 12   /* (x Fixed, y Fixed) -> Fixed */
} CyDetmathFunction;

/* What an order tells a group of lockstep units to do. */
typedef enum CyLockstepOrderKind {
    CY_LOCKSTEP_ORDER_MOVE = 0, /* plan a path to `target` and form up around it */
    CY_LOCKSTEP_ORDER_STOP = 1, /* drop the path and stand */
    /* 1.16: armed units. Go to `target` fighting what comes within sight on the way; or go for
     * the enemy standing nearest `target`, an attack-move to it when none stands within 3 m. */
    CY_LOCKSTEP_ORDER_ATTACK_MOVE = 2,
    CY_LOCKSTEP_ORDER_ATTACK = 3
} CyLockstepOrderKind;

/* A unit, as `lockstep_enlist` takes it. Zero `radius` and `max_speed` are the mover's defaults,
 * half a metre and four metres a second. */
typedef struct CyLockstepUnitDesc {
    uint32_t struct_size;
    uint32_t group;       /* the squad an order addresses */
    CyEntity entity;      /* the scene entity that PRESENTS the unit, or CY_ENTITY_NULL */
    CyFixedVec2 position; /* x and world Z */
    CyFixed radius;
    CyFixed max_speed;
} CyLockstepUnitDesc;

/* An order, recorded for the session's next tick. */
typedef struct CyLockstepOrder {
    uint32_t struct_size;
    uint32_t kind; /* CyLockstepOrderKind */
    uint32_t group;
    uint32_t reserved;
    CyFixedVec2 target; /* raw CyFixed: the issuer converted, every peer reads these bits */
} CyLockstepOrder;

#define CY_LOCKSTEP_UNIT_MOVING 0x1u  /* it has a path it has not finished */
#define CY_LOCKSTEP_UNIT_ARRIVED 0x2u /* it finished its path on the last tick, and only then */

/* One unit's authoritative state, as the last tick left it. */
typedef struct CyLockstepUnit {
    uint32_t struct_size;
    uint32_t group;
    CyEntity entity;
    CyFixedVec2 position;
    CyFixedVec2 velocity;
    CyFixed height;
    CyAngle heading;
    uint32_t flags; /* CY_LOCKSTEP_UNIT_* */
} CyLockstepUnit;

/* The session. `digest` folds every tick's state hash in order from the session's world hash, so
 * two peers — or two architectures — that agree on it agreed on every tick. */
typedef struct CyLockstepStatus {
    uint32_t struct_size;
    uint32_t units;
    uint64_t tick;       /* ticks advanced */
    uint64_t commands;   /* commands in the session's log */
    uint64_t state_hash; /* the last tick's */
    uint64_t digest;
    uint32_t kernel_version; /* `cy::detmath::kKernelVersion` */
    uint32_t disagreements;  /* ticks a follower peer's state hash differed on; zero without one */
} CyLockstepStatus;

/* --- 1.9: global illumination -------------------------------------------------------------------
 *
 * Issue #35's API for a game: tune CyberGI while the world runs, place GI volumes, say how a moving
 * object takes part, switch emitters, and ask how much indirect light reaches a point — a stealth
 * game's visibility, an AI's sense of the dark. The entries reach
 * `cy::rendering::gi::IlluminationSystem` through `cy::abi::game::IlluminationBackend`
 * (cy/abi/game/illumination.h), implemented by `cy::game_backend::IlluminationAdapter`
 * (src/game_backend/illumination/). A host that runs no GI binds none, and every entry answers
 * UNAVAILABLE.
 *
 * ILLUMINATION IS PRESENTATION. Every 1.9 entry is [N U]: a fixed step is refused, so nothing in
 * the simulation depends on the light and a resimulated tick cannot change it twice. A game whose
 * AI reads the light samples it in its frame update and acts on it in the next fixed step, as it
 * acts on a click.
 *
 * The enumerations are the engine's, value for value; the adapter, which can name both, asserts
 * each (src/game_backend/illumination/src/illumination_backend.cpp). */

/* `gi::GiMode`: which sources light the world. */
typedef enum CyGiMode {
    CY_GI_MODE_NONE = 0,    /* no indirect light at all */
    CY_GI_MODE_BAKED = 1,   /* lightmaps and baked volumes only */
    CY_GI_MODE_PROBE = 2,   /* the seeded caches, updated by nothing */
    CY_GI_MODE_DYNAMIC = 3, /* the caches, updated every frame */
    CY_GI_MODE_HYBRID = 4   /* baked where there is a bake, dynamic elsewhere */
} CyGiMode;

/* `gi::GiQuality`: the probe ray multiplier, 0.5, 1, 2 and 4. */
typedef enum CyGiQuality {
    CY_GI_QUALITY_LOW = 0,
    CY_GI_QUALITY_MEDIUM = 1,
    CY_GI_QUALITY_HIGH = 2,
    CY_GI_QUALITY_EPIC = 3
} CyGiQuality;

/* `gi::GiTierPreference`: which world tier rays prefer. */
typedef enum CyGiTier {
    CY_GI_TIER_AUTOMATIC = 0,     /* screen, then hardware where available, else software */
    CY_GI_TIER_SOFTWARE_ONLY = 1, /* never the hardware tier */
    CY_GI_TIER_WORLD_ONLY = 2     /* skip the screen tier */
} CyGiTier;

/* `gi::IlluminationClass`, and AUTOMATIC: no override, the class derived from mobility and size. */
typedef enum CyGiClass {
    CY_GI_CLASS_STATIC = 0,       /* in the caches; a move invalidates now */
    CY_GI_CLASS_SLOW_DYNAMIC = 1, /* in the caches; moves coalesced */
    CY_GI_CLASS_FAST_DYNAMIC = 2, /* a proxy that occludes and reflects, in no cache */
    CY_GI_CLASS_TRANSIENT = 3,    /* ignored by GI */
    CY_GI_CLASS_AUTOMATIC = 4
} CyGiClass;

/* `gi::EmitterOverride`: what a rule makes of the emitters it matches. */
typedef enum CyGiEmission {
    CY_GI_EMISSION_AUTOMATIC = 0,  /* classified by radiant power and area */
    CY_GI_EMISSION_CACHE_ONLY = 1, /* never promoted to lights */
    CY_GI_EMISSION_PROMOTE = 2,    /* promoted to lights whatever its size */
    CY_GI_EMISSION_OFF = 3         /* switched off: it lights nothing */
} CyGiEmission;

/* `gi::RadianceSource`: where a sample's light came from. */
typedef enum CyGiSource {
    CY_GI_SOURCE_NONE = 0,
    CY_GI_SOURCE_LIGHTMAP = 1,
    CY_GI_SOURCE_IRRADIANCE_VOLUME = 2,
    CY_GI_SOURCE_RADIANCE_CACHE = 3,
    CY_GI_SOURCE_SURFACE_CACHE = 4,
    CY_GI_SOURCE_SCREEN_TRACE = 5,
    CY_GI_SOURCE_SOFTWARE_TRACE = 6,
    CY_GI_SOURCE_HARDWARE_TRACE = 7,
    CY_GI_SOURCE_REFLECTION_PROBE = 8,
    CY_GI_SOURCE_SKY = 9
} CyGiSource;

/* Hardware ray tracing is allowed where the device has ray queries. Clear behaves exactly as a
 * device without them. */
#define CY_GI_HARDWARE_RAY_TRACING 0x1u

/* The GI settings of the running world. */
typedef struct CyGiSettings {
    uint32_t struct_size;
    uint32_t mode;    /* CyGiMode */
    uint32_t quality; /* CyGiQuality: the global probe quality; a volume's where one reaches */
    uint32_t tier;    /* CyGiTier: the global tier preference; a volume's where one reaches */
    uint32_t flags;   /* CY_GI_HARDWARE_RAY_TRACING */
    float budget_ms;  /* the GI allocation in milliseconds, positive */
} CyGiSettings;

/* The parameters a GI volume overrides, one bit each: `gi::gi_volume_bit`. */
#define CY_GI_VOLUME_SETS_QUALITY 0x01u
#define CY_GI_VOLUME_SETS_PROBE_DENSITY 0x02u
#define CY_GI_VOLUME_SETS_UPDATE_RATE 0x04u
#define CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY 0x08u
#define CY_GI_VOLUME_SETS_SPECULAR_INTENSITY 0x10u
#define CY_GI_VOLUME_SETS_TIER 0x20u
#define CY_GI_VOLUME_SETS_FAR_FIELD 0x40u

/* A GI volume: a box that overrides, inside it and fading over `blend_distance` outside it, the
 * parameters `sets` names — and nothing else, so a field whose bit is clear is ignored. Where
 * volumes overlap, the higher priority dominates (gi/gi_volume.h). */
typedef struct CyGiVolumeDesc {
    uint32_t struct_size;
    uint32_t sets; /* CY_GI_VOLUME_SETS_* */
    float bounds_min[3];
    float bounds_max[3];
    int32_t priority;
    float blend_distance;     /* metres, >= 0 */
    float weight;             /* [0, 1]: dials the whole volume down without moving it */
    uint32_t quality;         /* CyGiQuality */
    float probe_density;      /* > 0; above 1 full density, below 1 the sparse lattice */
    float update_rate;        /* > 0; how fast its probes age in the scheduler */
    float diffuse_intensity;  /* >= 0 */
    float specular_intensity; /* >= 0 */
    uint32_t tier;            /* CyGiTier */
    float far_field_metres;   /* > 0; where the far field begins */
} CyGiVolumeDesc;

/* A placed GI volume. Never reused within a run, so a destroyed volume's handle is NOT_FOUND. */
typedef uint64_t CyGiVolume;
#define CY_GI_VOLUME_NULL ((CyGiVolume)0)

/* `gi_sample_irradiance` was given no normal and averaged six axis-aligned ones. */
#define CY_GI_SAMPLE_OMNIDIRECTIONAL 0x1u

/* The indirect light arriving at a point. */
typedef struct CyGiIrradiance {
    uint32_t struct_size;
    uint32_t flags;    /* CY_GI_SAMPLE_* */
    float radiance[3]; /* the mean cosine-weighted incoming indirect radiance, linear: the
                        * engine's `indirect_diffuse`, which a surface multiplies by its albedo */
    float luminance;   /* Rec. 709 luminance of `radiance` */
    float confidence;  /* [0, 1]: how much of the answer the caches stood behind */
    uint32_t source;   /* CyGiSource: the source that carried most of it */
} CyGiIrradiance;

/* What the GI did on the last frame. */
typedef struct CyGiStatus {
    uint32_t struct_size;
    uint32_t world_tier;        /* CyGiSource: the tier that answered world rays */
    float convergence;          /* the least converged region, [0, 1] */
    float measured_ms;          /* the GI's own cost on the last frame */
    float allocation_ms;        /* what it was allowed */
    uint32_t volumes;           /* GI volumes placed */
    uint32_t objects;           /* objects registered with the GI */
    uint32_t promoted_lights;   /* emitter cards lighting as lights */
    uint32_t emitters;          /* emitters classified */
    uint32_t disabled_emitters; /* of which switched off */
    uint64_t frame;             /* the frame the last update ran */
} CyGiStatus;

/* --- 1.10: fog of war ---------------------------------------------------------------------------
 *
 * What each team sees, has seen and remembers, kept by `cy::gameplay::fog::FogOfWar`
 * (src/gameplay/fog/) and reached through `cy::abi::game::FogBackend` (cy/abi/game/fog.h),
 * implemented by `cy::game_backend::FogAdapter` (src/game_backend/fog/). A host that runs no fog of
 * war binds none, and every 1.10 entry answers UNAVAILABLE.
 *
 * THE GRID IS SIMULATION. A reveal radius is a CyFixed and a position a CyFixedVec2 (`y` is world
 * Z), so what a team may see is decided in the fixed step from integers, the same on every peer of
 * a lockstep session. `fog_set_vision` changes it and is [N F]: refused in a frame update, which
 * would let presentation decide what a player knows. The questions are [N F U]: a frame update may
 * read the grid to draw it, which is all the renderer ever does with it. */

/* `gameplay::fog::CellState`: a cell as one team knows it. */
typedef enum CyFogCell {
    CY_FOG_CELL_UNEXPLORED = 0, /* nothing the team (or a team sharing its vision) has seen */
    CY_FOG_CELL_EXPLORED = 1,   /* seen before, not seen now */
    CY_FOG_CELL_VISIBLE = 2     /* in sight this tick */
} CyFogCell;

/* `gameplay::fog::UnitSight`: how a team sees an entity. */
typedef enum CyFogSight {
    CY_FOG_SIGHT_HIDDEN = 0, /* not seen, and not remembered: not drawn */
    CY_FOG_SIGHT_GHOST = 1,  /* a structure seen once: drawn where it was last seen */
    CY_FOG_SIGHT_VISIBLE = 2 /* seen now, or the team's own or an ally's */
} CyFogSight;

/* A structure: a team that saw it remembers it as a ghost. */
#define CY_FOG_LEAVES_GHOST 0x1u

/* An entity's part in the fog: whose sight it gives and how far, and whether it leaves a ghost. */
typedef struct CyFogVision {
    uint32_t struct_size;
    uint32_t team;         /* 1 to 32; 0 gives no sight, and is seen by any team that sees it */
    CyFixed reveal_radius; /* metres, Q32.32, >= 0 */
    uint32_t flags;        /* CY_FOG_LEAVES_GHOST */
    uint32_t reserved;     /* zero */
} CyFogVision;

/* What the last fixed step did to the grid. */
typedef struct CyFogStatus {
    uint32_t struct_size;
    uint32_t width;         /* cells along x */
    uint32_t height;        /* cells along z */
    uint32_t units;         /* entities with a vision, updated */
    uint64_t tick;          /* the last update's */
    uint32_t visible_cells; /* cells some unit sees, counted once */
    uint32_t ghosts;        /* remembered structures, every team's */
    uint64_t digest;        /* equal on every peer after the same updates */
} CyFogStatus;

/* --- 1.11: lights ------------------------------------------------------------------------------
 *
 * Lights a game makes while it runs — a muzzle flash, an explosion, a spark, a torch carried by a
 * character — kept by `cy::rendering::TransientLights` (rendering/lighting/transient_lights.h) and
 * reached through `cy::abi::game::LightsBackend` (cy/abi/game/lights.h), implemented by
 * `cy::game_backend::LightsAdapter` (src/game_backend/lights/). The host appends the live lights to
 * the frame's every frame, so a flash is clustered, sampled and lit like an authored lamp. A host
 * that draws nothing binds none, and every 1.11 entry answers UNAVAILABLE.
 *
 * LIGHTS ARE PRESENTATION. Every entry is [N U]: a fixed step is refused, because a resimulated
 * tick would spawn its flash again, and nothing a light does feeds back into the simulation. A
 * light is a slot of a fixed pool, so spawning allocates nothing and is cheap enough for a battle's
 * every shot; a full pool refuses with OUT_OF_RANGE and counts the refusal. A light with a lifetime
 * fades along it by its `CyLightFade` and is freed when it ends; with none it lives until
 * destroyed. A `CyLight` is the slot and its generation, so a handle to a light that ended never
 * answers for the light now in its slot. */

/* `render::LightKind`: what a light is. The numbers are the engine's; a directional light is not a
 * light a game spawns. */
typedef enum CyLightKind {
    CY_LIGHT_POINT = 1,  /* candela */
    CY_LIGHT_SPOT = 2,   /* candela, a cone along the pose's forward axis */
    CY_LIGHT_RECT = 3,   /* nits; a panel facing forward, `size` its width and height */
    CY_LIGHT_DISC = 4,   /* nits; a disc facing forward, `size[0]` across */
    CY_LIGHT_SPHERE = 5, /* nits; `size[0]` across */
    CY_LIGHT_TUBE = 6    /* nits; along the pose's right axis, `size[0]` long, `size[1]` across */
} CyLightKind;

/* `rendering::TransientFade`: how a light with a lifetime fades along it. */
typedef enum CyLightFade {
    CY_LIGHT_FADE_NONE = 0,     /* whole until it ends */
    CY_LIGHT_FADE_LINEAR = 1,   /* to zero along its life */
    CY_LIGHT_FADE_QUADRATIC = 2 /* as (1 - t)^2: a flash's afterglow */
} CyLightFade;

/* A rect or a disc that emits from both faces. */
#define CY_LIGHT_TWO_SIDED 0x1u

/* A light a game spawns. The pose is world metres; its forward axis (local -Z) is a spot's axis and
 * a panel's facing, its right axis (local +X) a panel's width and a tube's length. */
typedef struct CyLightDesc {
    uint32_t struct_size;
    uint32_t kind;        /* CyLightKind */
    CyPose pose;          /* an all-zero rotation is the identity: forward is -Z */
    float color[3];       /* linear */
    float intensity;      /* candela for a point or a spot, nits for an area light; >= 0 */
    float range;          /* metres, >= 0; zero derives an area light's reach */
    float inner_cone;     /* radians, a spot */
    float outer_cone;     /* radians, a spot */
    float size[2];        /* metres, an area light's: see CyLightKind */
    float lifetime;       /* seconds, >= 0; zero lives until destroyed */
    uint32_t fade;        /* CyLightFade */
    uint32_t flags;       /* CY_LIGHT_TWO_SIDED */
    uint32_t layer_mask;  /* the channels it lights; zero lights every channel */
    uint32_t reserved[2]; /* zero */
} CyLightDesc;

/* A light a game spawned: its pool slot in the low 32 bits and the slot's generation above. */
typedef uint64_t CyLight;
#define CY_LIGHT_NULL ((CyLight)0)

/* What the pool holds and has done. */
typedef struct CyLightStatus {
    uint32_t struct_size;
    uint32_t live;     /* lights alive now */
    uint32_t capacity; /* the most alive at once */
    uint32_t reserved; /* zero */
    uint64_t spawned;  /* since the pool was made */
    uint64_t expired;  /* whose lifetime ended */
    uint64_t refused;  /* spawns refused by a full pool */
} CyLightStatus;
/* --- 1.12: the widget toolkit ------------------------------------------------------------------
 *
 * CyberUI's widget toolkit (`cy::ui::Toolkit`, src/ui/toolkit/) from a module: widgets made one at
 * a time or a whole `.cyui` document at once, their values, items, selection and text, a style
 * sheet over them, animations, and navigation driven by a game's own input. Reached through the 1.6
 * backend (`cy::abi::game::UiBackend`), which `cy::game_backend::UiAdapter` implements over a
 * toolkit the embedder attached; without one every 1.12 entry answers UNAVAILABLE.
 *
 * A WIDGET IS A CyUiElement: its root element. The 1.6 entries that take any element — destroy,
 * visibility, opacity, rect, hit test, focus — take a widget; the 1.6 kinds and the widgets are
 * otherwise separate, and a widget takes no `ui_set_layout`: it is laid out by its kind, its sheet
 * and its document. Events arrive through `CyBehaviourVTable.ui_event` exactly as a button's click
 * does, to the behaviours of the widget's owner — the owner it was made with, or its document's.
 *
 * [N U] every one, as 1.6's: the interface is presentation. */

/* `cy::ui::WidgetKind`, value for value: what `ui_create_widget` makes. */
typedef enum CyUiWidgetKind {
    CY_UI_WIDGET_PANEL = 0,        /* children placed by anchors */
    CY_UI_WIDGET_ROW = 1,          /* a flex row */
    CY_UI_WIDGET_COLUMN = 2,       /* a flex column */
    CY_UI_WIDGET_GRID = 3,         /* `columns` columns */
    CY_UI_WIDGET_STACK = 4,        /* children layered, each filling it */
    CY_UI_WIDGET_SCROLL_VIEW = 5,  /* a clipped, scrolled viewport */
    CY_UI_WIDGET_LABEL = 6,        /* `text` */
    CY_UI_WIDGET_RICH_TEXT = 7,    /* `text` as markup: [color=#rrggbb], [size=N], [img=...] */
    CY_UI_WIDGET_IMAGE = 8,        /* not made through this entry: a document's `image` */
    CY_UI_WIDGET_BUTTON = 9,       /* CLICK */
    CY_UI_WIDGET_TOGGLE = 10,      /* a switch: CHANGED, the value 0 or 1 */
    CY_UI_WIDGET_CHECKBOX = 11,    /* CHANGED, the value 0 or 1 */
    CY_UI_WIDGET_RADIO_GROUP = 12, /* one of `items`: CHANGED, the selection */
    CY_UI_WIDGET_SLIDER = 13,      /* `value` in [minimum, maximum] by `step`: CHANGED */
    CY_UI_WIDGET_PROGRESS_BAR = 14,
    CY_UI_WIDGET_TEXT_FIELD = 15, /* CHANGED on each edit, SUBMITTED on Enter */
    CY_UI_WIDGET_DROPDOWN = 16,   /* one of `items`, picked from a list: OPENED, CHANGED, CLOSED */
    CY_UI_WIDGET_LIST_VIEW = 17,  /* `items`, virtualised: CHANGED, SUBMITTED */
    CY_UI_WIDGET_TABS = 18,       /* every child a page, titled by its `title`: CHANGED */
    CY_UI_WIDGET_DIALOG = 19      /* modal, opened by `ui_widget_open`: OPENED, CLOSED */
} CyUiWidgetKind;

/* `cy::ui::UiAction`, value for value: the semantic actions `ui_navigate` delivers. */
typedef enum CyUiAction {
    CY_UI_ACTION_ACCEPT = 0,
    CY_UI_ACTION_CANCEL = 1,
    CY_UI_ACTION_NAVIGATE_UP = 2,
    CY_UI_ACTION_NAVIGATE_DOWN = 3,
    CY_UI_ACTION_NAVIGATE_LEFT = 4,
    CY_UI_ACTION_NAVIGATE_RIGHT = 5,
    CY_UI_ACTION_NEXT_TAB = 6,
    CY_UI_ACTION_PREVIOUS_TAB = 7,
    CY_UI_ACTION_CONTEXT = 8,
    CY_UI_ACTION_SCROLL_UP = 9,
    CY_UI_ACTION_SCROLL_DOWN = 10,
    CY_UI_ACTION_FOCUS_NEXT = 11,    /* the tab order: Tab */
    CY_UI_ACTION_FOCUS_PREVIOUS = 12 /* Shift+Tab */
} CyUiAction;

/* `cy::ui::AnimatedProperty`, value for value: paint properties only, so an animation never lays
 * anything out again. */
typedef enum CyUiAnimatedProperty {
    CY_UI_ANIMATE_OPACITY = 0,
    CY_UI_ANIMATE_TRANSLATE_X = 1, /* units, a render transform: layout never sees it */
    CY_UI_ANIMATE_TRANSLATE_Y = 2,
    CY_UI_ANIMATE_SCALE = 3,     /* about the widget's centre */
    CY_UI_ANIMATE_BACKGROUND = 4 /* `from_colour` to `to_colour`, premultiplied */
} CyUiAnimatedProperty;

/* `cy::ui::Easing`, value for value. */
typedef enum CyUiEasing {
    CY_UI_EASING_LINEAR = 0,
    CY_UI_EASING_EASE_IN = 1,
    CY_UI_EASING_EASE_OUT = 2,
    CY_UI_EASING_EASE_IN_OUT = 3,
    CY_UI_EASING_EASE = 4,
    CY_UI_EASING_SPRING = 5, /* past the target and back */
    CY_UI_EASING_BACK = 6    /* overshoots and settles */
} CyUiEasing;

#define CY_UI_WIDGET_CHECKED 0x1u  /* a toggle or a checkbox made on */
#define CY_UI_WIDGET_DISABLED 0x2u /* made refusing input */

/* What `ui_create_widget` makes. Every string is borrowed for the call and copied; null is empty.
 * Zero is the default for every number but `maximum`, whose zero is one. */
typedef struct CyUiWidgetDesc {
    uint32_t struct_size;
    uint32_t kind;           /* CyUiWidgetKind */
    const char* id;          /* `ui_find`'s key and a sheet's #id */
    const char* classes;     /* space-separated, at most four */
    const char* text;        /* a caption, a text field's text, a rich text's markup */
    const char* placeholder; /* a text field's hint */
    const char* title;       /* a tab page's or a dialog's */
    const char* const* items;
    uint32_t item_count;
    uint32_t flags; /* CY_UI_WIDGET_* */
    float value;
    float minimum;
    float maximum;
    float step;
    uint32_t columns; /* a grid's */
    float row_height; /* a list's; zero is 22 */
    CyEntity owner;   /* whose behaviours receive the widget's events; null for nobody */
} CyUiWidgetDesc;

/* What `ui_load_document` loads: the document's own text, or — `text` null — a path the embedder
 * reads, resolves the document's `use` and `stylesheet` paths against, and reloads when it changes
 * on disk. */
typedef struct CyUiDocumentDesc {
    uint32_t struct_size;
    uint32_t reserved;  /* zero */
    const char* text;   /* `.cyui`, borrowed for the call; null to read `path` */
    uint64_t text_size; /* bytes of `text` */
    const char* path;   /* the document's path, when `text` is null */
    CyEntity owner;     /* every widget the document makes reports to this entity's behaviours */
} CyUiDocumentDesc;

/* Not decorative: still played under reduced motion. */
#define CY_UI_ANIMATION_STATE_FEEDBACK 0x1u

/* One animation of one paint property of a widget, evaluated by the toolkit every frame — nothing
 * crosses the boundary until it is over. */
typedef struct CyUiAnimation {
    uint32_t struct_size;
    uint32_t property; /* CyUiAnimatedProperty */
    float from;
    float to;
    uint32_t from_colour; /* CY_UI_ANIMATE_BACKGROUND's */
    uint32_t to_colour;
    float duration; /* seconds */
    float delay;
    uint32_t easing; /* CyUiEasing */
    uint32_t flags;  /* CY_UI_ANIMATION_* */
} CyUiAnimation;

/* --- 1.13: the first-person camera and the weapon view -----------------------------------------
 *
 * A first-person camera's behaviours — mouse look with a sensitivity, head bob, recoil kick and
 * recovery, the change of lens for aiming down the sights — and the WEAPON VIEW drawn beside it:
 * the arms and the weapon through their own field of view and near plane, in front of every wall
 * they are pushed into (`camera-system`'s "one camera, several views"). Kept by
 * `cy::camera::FirstPersonCamera` (servers/camera/first_person.h) and
 * `cy::rendering::viewmodel` (rendering/viewmodel/), reached through
 * `cy::abi::game::FirstPersonBackend` (cy/abi/game/first_person.h), implemented by
 * `cy::game_backend::FirstPersonAdapter` (src/game_backend/first_person/). A host that draws
 * nothing binds none, and every 1.13 entry answers UNAVAILABLE.
 *
 * THE CAMERA IS PRESENTATION. Every entry is [N U]: a fixed step is refused, because the camera is
 * not authoritative and a resimulated tick must not kick it again. A first-person camera is named
 * by the `CyCamera` its rig is; the host advances each one once a frame by the frame's time, places
 * the eye at the character's head and the weapon's instances at `CyFirstPersonState::weapon` in the
 * eye's space, and draws the weapon view's layers through its lens.
 *
 * THE LOOK'S SENSITIVITY IS APPLIED HERE, ONCE: `CyFirstPersonInput::look` is the device's deltas —
 * mouse counts — and the action that reads them carries no sensitivity processor. */

/* `CyFirstPersonDesc::flags`. */
#define CY_FIRST_PERSON_INVERT_Y 0x1u

/* A first-person camera's behaviours. Every number finite; angles in radians, lengths in metres. */
typedef struct CyFirstPersonDesc {
    uint32_t struct_size;
    uint32_t flags;            /* CY_FIRST_PERSON_INVERT_Y */
    float look_sensitivity;    /* radians a device count, >= 0 */
    float aim_sensitivity;     /* its multiplier with the sights fully up, >= 0 */
    float pitch_limit;         /* (0, pi/2]: the pitch is clamped to plus or minus this */
    float hip_fov;             /* the world's vertical field of view from the hip, (0, pi) */
    float aim_fov;             /* and with the sights up */
    float hip_viewmodel_fov;   /* the weapon view's, from the hip, (0, pi) */
    float aim_viewmodel_fov;   /* and with the sights up */
    float aim_half_life;       /* seconds the sights take to come half way, >= 0 */
    float bob_stride;          /* metres walked a full bob cycle, > 0 */
    float bob_vertical;        /* metres at the reference speed, >= 0 */
    float bob_lateral;         /* metres at the reference speed, >= 0 */
    float bob_reference_speed; /* metres a second, > 0 */
    float recoil_frequency;    /* the recoil spring's, hertz, > 0 */
    float recoil_damping;      /* its damping ratio, >= 0; one is critical */
    float recoil_camera_share; /* [0, 1]: the recoil the view takes; the weapon takes the rest */
    float motion_reduction;    /* [0, 1]: accessibility; one leaves no bob and no view recoil */
    float hip_offset[3];       /* the weapon in the eye's space (-Z ahead), from the hip */
    float aim_offset[3];       /* and with the sights up */
} CyFirstPersonDesc;

/* `CyFirstPersonInput::flags`. */
#define CY_FIRST_PERSON_AIMING 0x1u   /* the sights are held up */
#define CY_FIRST_PERSON_AIRBORNE 0x2u /* off the ground: the stride stops */

/* One frame's input to a first-person camera. Inputs between two frames add up. */
typedef struct CyFirstPersonInput {
    uint32_t struct_size;
    uint32_t flags;    /* CY_FIRST_PERSON_AIMING, CY_FIRST_PERSON_AIRBORNE */
    float look[2];     /* the device's deltas: +x turns right, +y looks up; no sensitivity */
    float move_speed;  /* metres a second over the ground, >= 0 */
    uint32_t reserved; /* zero */
} CyFirstPersonInput;

/* What a first-person camera's last frame produced. */
typedef struct CyFirstPersonState {
    uint32_t struct_size;
    float yaw; /* the look the player controls — the control aim — recoil excluded */
    float pitch;
    float recoil[2];     /* the recoil spring's displacement: pitch, yaw */
    float aim;           /* 0 at the hip, 1 with the sights fully up */
    float fov;           /* the world's vertical field of view now */
    float viewmodel_fov; /* the weapon view's */
    float eye_offset[3]; /* the bob, in the eye's space */
    CyPose weapon;       /* the weapon in the eye's space: held, bobbing, kicked */
    float view_aim[3];   /* where the camera looks, in the world */
    float weapon_aim[3]; /* where the weapon points, in the world */
} CyFirstPersonState;

/* `CyViewmodelDesc::flags`: the weapon view's part in the world's shadows. Zero, the default, is
 * a weapon that is shadowed by the world and casts nothing. */
#define CY_VIEWMODEL_IGNORE_SHADOWS 0x1u  /* not shadowed by the world's shadow map */
#define CY_VIEWMODEL_CAST_SHADOWS 0x2u    /* drawn into the world's shadow map from where it is */
#define CY_VIEWMODEL_CONTACT_SHADOWS 0x4u /* takes the screen-space contact term too */

/* A camera's weapon view: which layers it draws — and the world view therefore does not — and its
 * lens. `layers` zero is no weapon view. */
typedef struct CyViewmodelDesc {
    uint32_t struct_size;
    uint32_t layers;      /* the layers the arms and the weapon are on; zero for none */
    float fov;            /* vertical, (0, pi); zero follows the first-person camera's */
    float near_plane;     /* metres, > 0 */
    float far_plane;      /* metres, beyond the near plane */
    float depth_fraction; /* (0, 0.5]: the slice of the depth range nearest the eye it takes */
    uint32_t flags;       /* CY_VIEWMODEL_* */
    uint32_t reserved;    /* zero */
} CyViewmodelDesc;

/* --- 1.14: foot placement and aim -------------------------------------------------------------
 *
 * Issue #92. A character's program places its feet with a `pose.foot_ik` node and aims with a
 * `pose.aim` node; both are authored in its animation graph and solved by the engine every frame
 * on the ground the host handed the animation system (a physics world or a terrain). What crosses
 * here is what a game drives and reads: where an aim points, and what the feet stood on.
 *
 * Driving a blend space needs nothing new: its axes are program parameters, set through
 * `animation_set_float` from a behaviour (a speed, a direction). */

/* CyFootPlacement::flags */
#define CY_FOOT_PLACEMENT_SOLVED 0x1u       /* the last evaluation placed the feet */
#define CY_FOOT_PLACEMENT_LEFT_GROUND 0x2u  /* ground was found under the left foot */
#define CY_FOOT_PLACEMENT_RIGHT_GROUND 0x4u /* and under the right */
#define CY_FOOT_PLACEMENT_LEFT_REACHED 0x8u /* the left leg could reach its target */
#define CY_FOOT_PLACEMENT_RIGHT_REACHED 0x10u

/* What one foot placement did when the character's pose was last evaluated. Left first. */
typedef struct CyFootPlacement {
    uint32_t struct_size;
    uint32_t flags;  /* CY_FOOT_PLACEMENT_* */
    float pelvis;    /* metres the hips moved along the character's up; negative is down */
    float offset[2]; /* metres each foot moved along the character's up */
    float ground[6]; /* the ground under each foot, in the world: x y z, x y z */
    float normal[6]; /* its normal, unit, in the world */
} CyFootPlacement;

/* --- 1.15: effects and decals --------------------------------------------------------------------
 *
 * What a shot leaves behind: a burst of particles where it left the muzzle and where it struck, and
 * a mark on what it struck. Effects are the engine's VFX systems — `cy::vfx::SimulationWorld`,
 * reached through `cy::abi::game::EffectsBackend` (cy/abi/game/effects.h) and implemented by
 * `cy::game_backend::EffectsAdapter` (src/game_backend/effects/). Decals are the frame's projected
 * decals — `cy::rendering::DecalBudget`, reached through `cy::abi::game::DecalsBackend`
 * (cy/abi/game/decals.h) and implemented by `cy::game_backend::DecalsAdapter`
 * (src/game_backend/decals/). The host registers what a game may play by NAME — an effect it
 * cooked, a decal material it described — the way it registers an audio cue; a game resolves the
 * name once and plays it by its handle. A host that draws nothing binds neither, and every 1.15
 * entry answers UNAVAILABLE.
 *
 * BOTH ARE PRESENTATION. Playing an effect and spawning a decal are [N U]: a fixed step is refused,
 * because a resimulated tick would play its burst and leave its mark again, and nothing either does
 * feeds back into the simulation. Resolving a name is [N F U], so a fixed step may hold the handle
 * it hands a frame update. An effect plays fire-and-forget and releases itself when its last
 * particle dies; a full simulation world refuses with OUT_OF_RANGE. A decal joins the budget the
 * host sized, and when the budget is full the least worth keeping — by age, importance and screen
 * coverage, deterministically — is evicted, possibly the new one, which is not an error: its
 * `CyDecal` is then CY_DECAL_NULL. */

/* A registered effect: what `effect_find` hands out. */
typedef uint64_t CyEffect;
#define CY_EFFECT_NULL ((CyEffect)0)

/* How an effect plays. The parameters override the effect's declared parameters in declaration
 * order, four floats a parameter (a `float` parameter takes the first of its four); the rest keep
 * their authored values. A direction an effect sprays along is such a parameter. */
typedef struct CyEffectPlay {
    uint32_t struct_size;
    uint32_t parameter_count; /* floats of `parameters` read, 0 to 8 */
    float position[3];        /* world metres */
    float scale;              /* > 0 */
    float parameters[8];
    uint32_t reserved[2]; /* zero */
} CyEffectPlay;

/* A registered decal material: what `decal_find_material` hands out. */
typedef uint64_t CyDecalMaterial;
#define CY_DECAL_MATERIAL_NULL ((CyDecalMaterial)0)

/* A decal a game spawns: a box projected along its local -Z onto whatever the box contains, so an
 * impact's decal has its local +Z along the struck surface's normal. */
typedef struct CyDecalDesc {
    uint32_t struct_size;
    uint32_t reserved0;       /* zero */
    CyDecalMaterial material; /* from `decal_find_material` */
    CyPose pose;              /* world; an all-zero rotation is the identity */
    float half_extent[3]; /* metres, > 0: the mark's half width and height, the box's half depth */
    float importance;     /* 0 to 1: what eviction keeps longest */
    int32_t sort_order;   /* a higher order covers a lower; equal orders, the later spawn */
    float fade_angle;     /* radians, 0 to pi/2: past it the mark fades; zero is 60 degrees */
    float fade_start;     /* metres from the eye the mark starts fading; zero is 20 */
    float fade_end;       /* metres it is gone by; zero is 40; above fade_start */
    uint32_t reserved[2]; /* zero */
} CyDecalDesc;

/* A decal a game spawned: the budget's id, or CY_DECAL_NULL when it was the one evicted. */
typedef uint64_t CyDecal;
#define CY_DECAL_NULL ((CyDecal)0)

/* What the decal budget holds and has done. */
typedef struct CyDecalStatus {
    uint32_t struct_size;
    uint32_t live;     /* decals in the budget now */
    uint32_t capacity; /* the most it holds */
    uint32_t reserved; /* zero */
    uint64_t spawned;  /* since the budget was bound */
    uint64_t evicted;  /* by a full budget, the spawned one included */
} CyDecalStatus;

/* --- 1.16: combat in the lockstep path -----------------------------------------------------------
 *
 * openspec/changes/add-rts-combat-and-minimap. ABI 1.8's lockstep session learns to FIGHT: a unit
 * enlisted with `lockstep_enlist` is given health and a weapon with `lockstep_arm`, a building is
 * placed with `lockstep_fortify`, and the two new order kinds send a group to fight —
 * CY_LOCKSTEP_ORDER_ATTACK_MOVE to a point, engaging what it meets, and CY_LOCKSTEP_ORDER_ATTACK at
 * the enemy standing nearest a point. Every tick the session decides targets, chases, melee blows,
 * projectile launches, flights, hits, splash and deaths in integers and `CyFixed`
 * (cy/game_backend/lockstep_combat.h), so every peer that executes the same command log computes
 * the same battle and the state hash folds all of it.
 *
 * WHAT A GAME DRAWS IS READ, NEVER DECIDED. `lockstep_events` hands out what the ticks did — a blow
 * struck, a projectile launched and where it landed, the damage and the health it left, a death, a
 * building destroyed — each numbered from zero in the order the session made it, so a caller keeps
 * a cursor and reads each once, from a frame update where effects, lights and sounds may play.
 * `lockstep_projectiles` lists what is in flight, for a trail. Neither feeds back into a tick.
 *
 * Arming and fortifying are session configuration, [N] and before the first tick, as enlisting
 * is: PERMISSION_DENIED after it. Reading answers in every phase. A host whose session nobody
 * armed runs it exactly as before 1.16. */

/* How an armed unit attacks. */
typedef enum CyLockstepWeapon {
    CY_LOCKSTEP_WEAPON_MELEE = 0, /* a blow that lands the tick it is struck */
    CY_LOCKSTEP_WEAPON_RANGED = 1 /* a projectile that flies to its target */
} CyLockstepWeapon;

/* What a unit is attacking, or what an event names. */
typedef enum CyLockstepTargetKind {
    CY_LOCKSTEP_TARGET_NONE = 0,
    CY_LOCKSTEP_TARGET_UNIT = 1,     /* `target` is a unit's enlistment index */
    CY_LOCKSTEP_TARGET_STRUCTURE = 2 /* `target` is a structure's `lockstep_fortify` index */
} CyLockstepTargetKind;

/* What an armed unit is doing, as far as fighting goes. */
typedef enum CyLockstepStance {
    CY_LOCKSTEP_STANCE_IDLE = 0,        /* standing; fights what comes within its sight */
    CY_LOCKSTEP_STANCE_MOVE = 1,        /* going where it was sent, ignoring enemies */
    CY_LOCKSTEP_STANCE_ATTACK_MOVE = 2, /* going where it was sent, fighting on the way */
    CY_LOCKSTEP_STANCE_ATTACK = 3       /* after the one target it was given, until it falls */
} CyLockstepStance;

/* What `lockstep_events` reports. */
typedef enum CyLockstepEventKind {
    CY_LOCKSTEP_EVENT_STRIKE = 0, /* `unit` struck `target` in melee, from `position` at `toward` */
    CY_LOCKSTEP_EVENT_LAUNCH = 1, /* `projectile` left `unit` at `position`, aimed at `toward` */
    CY_LOCKSTEP_EVENT_IMPACT = 2, /* `projectile` landed at `position`; `amount` enemies it hit */
    CY_LOCKSTEP_EVENT_DAMAGE = 3, /* `target` lost `amount` to `unit`, leaving `health` */
    CY_LOCKSTEP_EVENT_DEATH = 4,  /* unit `unit` of `team` died at `position` */
    CY_LOCKSTEP_EVENT_DESTROYED = 5 /* structure `target` of `team` fell at `position` */
} CyLockstepEventKind;

/* An event or a projectile with no acting unit. */
#define CY_LOCKSTEP_NO_UNIT 0xFFFFFFFFu

/* A unit's health and weapon, as `lockstep_arm` takes it. A reach is the GAP between the two
 * footprints, so a melee unit with a half-metre range strikes from half a metre off its target's
 * edge whatever either one's radius. */
typedef struct CyLockstepCombatDesc {
    uint32_t struct_size;
    uint32_t team;            /* units and structures of another team are enemies */
    uint32_t health;          /* > 0 */
    uint32_t armour;          /* taken off every hit; a hit always deals at least one point */
    uint32_t weapon;          /* CyLockstepWeapon */
    uint32_t damage;          /* points a hit deals before armour, > 0 */
    uint32_t cooldown;        /* ticks between two attacks, > 0 */
    uint32_t reserved;        /* zero */
    CyFixed range;            /* metres of gap it attacks across, >= 0 */
    CyFixed sight;            /* metres of gap it notices an enemy across, >= 0 */
    CyFixed projectile_speed; /* metres a second, > 0 for a ranged weapon */
    CyFixed splash; /* a ranged weapon's: 0 hits its target alone; otherwise every enemy within */
} CyLockstepCombatDesc;

/* A building, as `lockstep_fortify` takes it: a static target, and an obstacle units walk round. */
typedef struct CyLockstepStructureDesc {
    uint32_t struct_size;
    uint32_t team;
    CyEntity entity;      /* the scene entity that PRESENTS it, or CY_ENTITY_NULL; not hashed */
    CyFixedVec2 position; /* x and world Z */
    CyFixed radius;       /* its footprint, > 0 */
    uint32_t health;      /* > 0 */
    uint32_t armour;
} CyLockstepStructureDesc;

#define CY_LOCKSTEP_COMBATANT_ARMED 0x1u   /* `lockstep_arm` gave it a weapon */
#define CY_LOCKSTEP_COMBATANT_ALIVE 0x2u   /* its health is above zero */
#define CY_LOCKSTEP_COMBATANT_ENGAGED 0x4u /* its target was within its range on the last tick */
#define CY_LOCKSTEP_COMBATANT_CHASING 0x8u /* it is following a path to its target */

/* One unit's combat state, as the last tick left it. */
typedef struct CyLockstepCombatant {
    uint32_t struct_size;
    uint32_t flags; /* CY_LOCKSTEP_COMBATANT_* */
    uint32_t team;
    uint32_t health;
    uint32_t max_health;
    uint32_t armour;
    uint32_t cooldown;    /* ticks until it may attack again */
    uint32_t kills;       /* units and structures it finished */
    uint32_t stance;      /* CyLockstepStance */
    uint32_t target_kind; /* CyLockstepTargetKind */
    uint32_t target;
    uint32_t reserved; /* zero */
} CyLockstepCombatant;

/* One structure's state. */
typedef struct CyLockstepStructure {
    uint32_t struct_size;
    uint32_t team;
    CyEntity entity;
    CyFixedVec2 position;
    CyFixed radius;
    uint32_t health; /* zero once destroyed */
    uint32_t max_health;
} CyLockstepStructure;

/* One thing a tick did. Passed in arrays, so fixed forever: no `struct_size`. */
typedef struct CyLockstepEvent {
    uint64_t sequence;    /* from zero, in the order the session made them */
    uint64_t tick;        /* the tick that made it */
    uint32_t kind;        /* CyLockstepEventKind */
    uint32_t unit;        /* the unit that acted (or died), or CY_LOCKSTEP_NO_UNIT */
    uint32_t target_kind; /* CyLockstepTargetKind */
    uint32_t target;
    uint32_t amount;     /* DAMAGE: points dealt; IMPACT: enemies hit */
    uint32_t health;     /* DAMAGE: what the target has left */
    uint32_t projectile; /* LAUNCH and IMPACT */
    uint32_t team;       /* the acting side's; DEATH and DESTROYED: the fallen's */
    CyFixedVec2 position;
    CyFixedVec2 toward; /* STRIKE and LAUNCH: the target's position; IMPACT: where it was fired */
} CyLockstepEvent;

/* A projectile in flight. Passed in arrays: no `struct_size`. */
typedef struct CyLockstepProjectile {
    uint32_t id;
    uint32_t shooter;
    uint32_t team;
    uint32_t target_kind; /* CyLockstepTargetKind */
    uint32_t target;
    uint32_t reserved;
    uint64_t launched; /* the tick it left */
    CyFixedVec2 origin;
    CyFixedVec2 position;
    CyFixedVec2 aim; /* where it is going: its target, or where its target fell */
} CyLockstepProjectile;

/* --- 1.17: what a game's menus need ------------------------------------------------------------
 *
 * The toolkit draws a menu; these let the menu DO what a settings screen does: rebind an action
 * through the input system's own rebinding flow and keep the player's profile, change the
 * resolution, the quality preset and the upscaler, keep the player's choices in their user-data
 * directory, and give a widget a tooltip. Each reaches a backend the embedder binds — the 1.3 input
 * backend for rebinding, a video backend for the display settings, a user-data backend for the
 * files, the 1.6 interface backend for tooltips — and answers UNAVAILABLE without one.
 *
 * REBINDING IS THE INPUT SYSTEM'S FLOW, not a key the game reads. `input_rebind_begin` opens a
 * `cy::input::RebindOperation` for one action, scheme and slot; the next eligible control the
 * player presses on that scheme is offered to it as the input server resolves it, under the
 * conflict policy the game chose; `input_rebind_poll` says where it is. An applied rebind is an
 * override in the user's profile, over the authored bindings, which stay immutable — so a shipped
 * update to the defaults keeps the player's choices. `input_profile_save` writes the profile as
 * text a game keeps with `user_data_write`; `input_profile_load` puts it back.
 *
 * [N U] every one: menus, display and files are not the simulation's. */

typedef enum CyInputScheme {
    CY_INPUT_SCHEME_KEYBOARD_MOUSE = 0,
    CY_INPUT_SCHEME_GAMEPAD = 1
} CyInputScheme;

/* `cy::input::ConflictPolicy`: what a rebind to a control another action holds does. */
typedef enum CyInputConflict {
    CY_INPUT_CONFLICT_REJECT = 0,         /* refused: the rebind ends in CONFLICT */
    CY_INPUT_CONFLICT_SWAP = 1,           /* the other action takes this one's old control */
    CY_INPUT_CONFLICT_ALLOW = 2,          /* both actions on one control */
    CY_INPUT_CONFLICT_UNBIND_PREVIOUS = 3 /* the other action is left unbound */
} CyInputConflict;

typedef enum CyInputRebindStatus {
    CY_INPUT_REBIND_IDLE = 0,      /* no rebind was begun for this user */
    CY_INPUT_REBIND_LISTENING = 1, /* waiting for a control */
    CY_INPUT_REBIND_APPLIED = 2,   /* the control is the action's now */
    CY_INPUT_REBIND_CONFLICT = 3,  /* refused under REJECT: `conflict` holds it */
    CY_INPUT_REBIND_CANCELLED = 4  /* cancelled by the game, or by the cancel control */
} CyInputRebindStatus;

/* Where a user's rebind is. Strings are NUL-terminated and cut to fit. */
typedef struct CyInputRebind {
    uint32_t struct_size;
    uint32_t status; /* CyInputRebindStatus */
    CyInputAction action;
    uint32_t scheme; /* CyInputScheme */
    uint32_t slot;
    char control[32];  /* the control offered, as a path: "keyboard/f", "gamepad/south" */
    char conflict[64]; /* CONFLICT: the action that holds it */
} CyInputRebind;

/* A quality preset: one level for every post-processing stage (`rendering::QualityLevel`). */
typedef enum CyVideoQuality {
    CY_VIDEO_QUALITY_LOW = 0,
    CY_VIDEO_QUALITY_MEDIUM = 1,
    CY_VIDEO_QUALITY_HIGH = 2,
    CY_VIDEO_QUALITY_ULTRA = 3
} CyVideoQuality;

/* `rendering::upscale::UpscaleMode`, value for value. */
typedef enum CyVideoUpscale {
    CY_VIDEO_UPSCALE_NATIVE = 0,     /* rendered at the output resolution, antialiased */
    CY_VIDEO_UPSCALE_QUALITY = 1,    /* 67 % per axis */
    CY_VIDEO_UPSCALE_BALANCED = 2,   /* 58 % per axis */
    CY_VIDEO_UPSCALE_PERFORMANCE = 3 /* 50 % per axis */
} CyVideoUpscale;

#define CY_VIDEO_FULLSCREEN 0x1u
#define CY_VIDEO_VSYNC 0x2u

/* The display settings a game's settings screen changes. */
typedef struct CyVideoSettings {
    uint32_t struct_size;
    uint32_t width; /* the output resolution, one of `video_modes` */
    uint32_t height;
    uint32_t quality; /* CyVideoQuality */
    uint32_t upscale; /* CyVideoUpscale */
    uint32_t flags;   /* CY_VIDEO_* */
} CyVideoSettings;

/* One resolution the display offers. */
typedef struct CyVideoMode {
    uint32_t width;
    uint32_t height;
} CyVideoMode;
/* --- 1.18: streamed music and ambience --------------------------------------------------------
 *
 * openspec/changes/add-audio-streaming-import. ABI 1.3 plays cues: short sounds decoded in memory.
 * A track — music, an ambience bed — is STREAMED: decoded ahead by a job into a bounded ring
 * buffer, so a ten-minute track holds a ring and a decoder and nothing that grows with its length.
 * The host registers cooked tracks by name (`cy::game_backend::AudioAdapter::add_track`); a game
 * finds one with `audio_find_track`, plays it with `audio_stream_play`, and moves between two with
 * `crossfade_from`: the new stream fades in while the old one fades out, both curves equal-power
 * and starting on the same sample, and the old one stops itself at the end of its fade.
 *
 * PRESENTATION, AS AUDIO IS. Every entry is [N F U]. While CY_TIME_RESIMULATING is set the writes
 * (`play`, `stop`, `seek`, `set_volume`) answer OK and do nothing — `play` hands back a null
 * stream — and a stream handle is not simulation state: a fixed step must not branch on one.
 *
 * Bus volumes are ABI 1.3's `audio_set_bus_volume`, which a stream routed to the bus obeys. */

typedef uint64_t CyAudioTrack;  /* a cooked track the host registered; zero is null */
typedef uint64_t CyAudioStream; /* one playing stream; zero is null, and an ended one never names
                                   a later stream */

/* Play the track at its own level, ignoring the loudness gain the importer measured for it. */
#define CY_AUDIO_STREAM_RAW_LEVEL 0x1u

/* Zero means "as authored" for `bus` (Master) and `volume` (1), as in `CyAudioPlay`. */
typedef struct CyAudioStreamPlay {
    uint32_t struct_size;
    uint32_t flags; /* CY_AUDIO_STREAM_* */
    CyAudioTrack track;
    CyAudioBus bus;
    CyAudioStream crossfade_from; /* fade this stream out while the new one fades in; or zero */
    float volume;                 /* linear gain */
    float fade_seconds;           /* the fade-in, and the crossfade's length */
    double start_seconds;         /* where in the track to begin */
} CyAudioStreamPlay;

/* Where a stream is. */
#define CY_AUDIO_STREAM_PLAYING 0x1u /* sounding, or about to once its ring is primed */
#define CY_AUDIO_STREAM_LOOPING 0x2u /* the track loops */

typedef struct CyAudioStreamState {
    uint32_t struct_size;
    uint32_t flags;          /* CY_AUDIO_STREAM_PLAYING, CY_AUDIO_STREAM_LOOPING */
    double position_seconds; /* where the listener is in the track */
    double duration_seconds; /* the played length of one pass */
    float gain;              /* the stream's gain after its fades */
    uint32_t underruns;      /* blocks its ring ran dry in, each played as silence */
} CyAudioStreamState;

/* --- The interface table -----------------------------------------------------------------------
 *
 * `table_size` is what makes growth additive: a module reads only the prefix it was compiled
 * against, and the engine writes only the entries it has. Neither side may reorder what is already
 * there. */
typedef struct CyInterfaceHeader {
    uint32_t abi_major; /* incompatible changes */
    uint32_t abi_minor; /* additive changes     */
    uint32_t abi_patch;
    uint32_t table_size; /* bytes; enables additive growth */
} CyInterfaceHeader;

/* THE APPEND-ONLY TABLE.
 *
 * Order is the contract. Adding an entry means adding it at the end, below the marker comment, and
 * incrementing CY_ABI_MINOR. Anything else — a reorder, a removal, a changed signature — is what
 * `just quality-abi` refuses, and it refuses it against src/abi/abi_baseline.json rather than
 * against a reviewer's memory.
 *
 * Every entry documents its thread role and its ownership rule where either is not obvious. The
 * default, stated once here rather than on thirty entries: an entry is called on the thread that
 * called into the module, arguments are borrowed for the duration of the call, and a returned
 * `CyVar` is owned by the caller if and only if it carries CY_VAR_FLAG_OWNED. */
typedef struct CyInterface {
    CyInterfaceHeader header;

    /* --- 1.0: diagnostics -------------------------------------------------------------------- */

    /* `severity` is `cy::DiagnosticSeverity`'s value. The message is borrowed and copied. */
    void (*log)(CyEngine engine, uint32_t severity, const char* message);
    /* The calling thread's last error message. Never null; empty when nothing failed. The pointer
     * is valid until this thread's next failing call. */
    const char* (*get_last_error)(void);
    /* The calling thread's last error code, CY_RESULT_OK when nothing failed. */
    CyResult (*get_last_error_code)(void);
    /* A module reporting its own failure, so that a module's diagnostic reads like the engine's.
     * `message` is copied into thread-local storage. */
    void (*set_last_error)(CyResult result, const char* message);

    /* --- 1.0: values ------------------------------------------------------------------------- */

    /* Both return a value carrying CY_VAR_FLAG_OWNED; release it exactly once. `length` bytes are
     * copied, so the caller's buffer need not outlive the call. */
    CyVar (*var_make_string)(CyEngine engine, const char* utf8, uint64_t length);
    CyVar (*var_make_bytes)(CyEngine engine, const void* data, uint64_t size);
    /* Another reference to the same payload, owned by the caller. An inline value is copied. */
    CyVar (*var_clone)(const CyVar* var);
    /* Drop one reference and clear `*var` to nil, so a double release is a no-op rather than a
     * corruption. Safe on a borrowed or inline value. */
    void (*var_release)(CyVar* var);
    /* Heap payloads currently alive. `native-abi` requires development builds to detect leaks of
     * returned values; this is what a test or an editor session asserts on, and it is counted in
     * every configuration because a counter that only exists in one is not evidence. */
    uint64_t (*var_live_count)(CyEngine engine);

    /* --- 1.0: the world ---------------------------------------------------------------------- */

    /* The world the engine is running, or null when none is bound. */
    CyWorld (*engine_world)(CyEngine engine);
    /* CY_ENTITY_NULL on failure, having set the last error. Structural: bumps the epoch. */
    CyEntity (*world_create_entity)(CyWorld world);
    CyResult (*world_destroy_entity)(CyWorld world, CyEntity entity);
    bool (*world_entity_alive)(CyWorld world, CyEntity entity);
    /* The world's structural epoch. Every structural change increments it; see `CyBorrow`. */
    uint64_t (*world_epoch)(CyWorld world);

    /* --- 1.0: components --------------------------------------------------------------------- */

    /* CY_COMPONENT_TYPE_INVALID on failure. Registering a name twice returns the existing id, so a
     * reloaded module re-registering its types is idempotent rather than an error. */
    CyComponentTypeId (*world_register_component)(CyWorld world, const CyComponentTypeDesc* desc);
    CyComponentTypeId (*world_find_component)(CyWorld world, const char* name);
    /* `initial` may be null, in which case the component is zero-initialised. Structural. */
    CyResult (*world_add_component)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                    const void* initial);
    CyResult (*world_remove_component)(CyWorld world, CyEntity entity, CyComponentTypeId component);
    bool (*world_has_component)(CyWorld world, CyEntity entity, CyComponentTypeId component);
    /* A borrowed pointer to the component's bytes, valid until the next structural change. Check it
     * with `borrow_valid` rather than remembering when that was. */
    CyBorrow (*world_borrow_component)(CyWorld world, CyEntity entity, CyComponentTypeId component);
    bool (*borrow_valid)(CyWorld world, CyBorrow borrow);

    /* The generic property path: a field by index, marshalled through CyVar. Correct for tools and
     * for anything reflective; not for a hot loop, which is what the typed entries below are. */
    CyResult (*component_get_var)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                  uint32_t field, CyVar* out_value);
    CyResult (*component_set_var)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                  uint32_t field, const CyVar* value);

    /* The typed fast paths. `native-abi` requires them so that a behaviour updating a transform
     * every tick does not marshal: these read and write the field's bytes directly after one type
     * check, and the generated overlay calls them rather than the CyVar path. */
    CyResult (*component_get_f32)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                  uint32_t field, float* out_value);
    CyResult (*component_set_f32)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                  uint32_t field, float value);
    CyResult (*component_get_vec3)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                   uint32_t field, float* out_xyz);
    CyResult (*component_set_vec3)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                   uint32_t field, const float* xyz);

    /* --- 1.0: behaviours --------------------------------------------------------------------- */

    /* Register a behaviour type. Null on failure. The vtable is copied — only the prefix both sides
     * agree on, `min(vtable->struct_size, sizeof(CyBehaviourVTable))` — so a module compiled
     * against a longer vtable than this engine knows is accepted and its extra entries ignored,
     * rather than the engine reading past what it understands. */
    CyBehaviourType (*register_behaviour)(CyEngine engine, const char* name,
                                          const CyBehaviourVTable* vtable);
    /* The registered type of that name in the current generation, or null. */
    CyBehaviourType (*find_behaviour)(CyEngine engine, const char* name);
    /* The generation that registered a behaviour type. The loader resolves create, destroy and
     * update through the generation that created an instance, never through "the current one" —
     * see cy/abi/module.h, and the spike measurement that made it a rule. */
    uint32_t (*behaviour_generation)(CyBehaviourType type);

    /* --- Append new entries below this line. Never above it, never between. ------------------ */

    /* --- 1.1: describing a world the caller did not build ------------------------------------ */

    /* Every component type registered in this world, including the engine's own. The ids are
     * `0 .. count - 1`, because `cy::ecs::ComponentRegistry` numbers in registration order. */
    uint32_t (*world_component_count)(CyWorld world);
    /* Describe one. The caller sets `out_info->struct_size` to its own `sizeof(CyComponentInfo)`
     * and the engine writes only that prefix, so a module compiled against a shorter struct is
     * filled in rather than overrun. Zero is accepted and means "the size I know", for a caller
     * that zeroed the struct. */
    CyResult (*world_component_info)(CyWorld world, CyComponentTypeId component,
                                     CyComponentInfo* out_info);
    /* One field of one component. `field` is `0 .. field_count - 1` from `world_component_info`.
     * `out_field->name` is borrowed from the engine and outlives the call.
     *
     * A COMPONENT MAY HAVE FEWER FIELDS HERE THAN IT HAS MEMBERS. Only fields whose type has a
     * fixed width this ABI can name are described, so the count is what is describable rather than
     * what the C++ struct holds — and a caller never has to guess whether an index is real. */
    CyResult (*world_component_field)(CyWorld world, CyComponentTypeId component, uint32_t field,
                                      CyFieldDesc* out_field);

    /* --- 1.1: the hierarchy, which is what an outliner is ------------------------------------ */

    /* The entity's parent, or CY_ENTITY_NULL when it is a root or does not exist. */
    CyEntity (*world_parent)(CyWorld world, CyEntity entity);
    /* Reparent. `parent` of CY_ENTITY_NULL makes `child` a root. Structural: it bumps the epoch,
     * and the ECS refuses it while a query is iterating rather than performing it late. */
    CyResult (*world_set_parent)(CyWorld world, CyEntity child, CyEntity parent);
    /* How many children `entity` has. Zero for an entity with none and for one that is not alive,
     * which are the same answer to "what is below it". */
    uint32_t (*world_child_count)(CyWorld world, CyEntity entity);
    /* The child at `index`, or CY_ENTITY_NULL. THE ORDER IS THE ECS'S AND IT IS NOT THE AUTHORED
     * ORDER: `ecs-core` leaves the children buffer unordered and removing a child swaps the last
     * one into the gap. A tool that shows children in the order a designer set them reads
     * `cy::scene::ChildOrder`, which is the component that exists to say so. */
    CyEntity (*world_child)(CyWorld world, CyEntity entity, uint32_t index);

    /* --- 1.1: chunks ------------------------------------------------------------------------- */

    /* Every chunk holding `component`, in archetype then chunk order.
     *
     * Writes at most `capacity` chunks and always reports the total in `out_count`, so the
     * two-call sizing pattern works: once with a NULL buffer to learn the count — which is a
     * question and answers CY_RESULT_OK — then again with room. With a non-null buffer that is too
     * small it returns CY_RESULT_BUFFER_TOO_SMALL having filled `capacity` of them, which is a
     * partial result the caller can use rather than an error that discards the work. */
    CyResult (*world_chunks)(CyWorld world, CyComponentTypeId component, CyChunk* out_chunks,
                             uint32_t capacity, uint32_t* out_count);

    /* --- 1.2: asynchronous, cancellable editor backend services ------------------------------ */

    CyResult (*service_open)(CyEngine engine, CyServiceSession* out_session);
    void (*service_close)(CyEngine engine, CyServiceSession session);
    CyResult (*service_submit)(CyEngine engine, CyServiceSession session,
                               const CyServiceRequest* request);
    CyResult (*service_cancel)(CyEngine engine, CyServiceSession session, uint64_t request_id);
    /* Non-blocking. `out_has_event` is false when no event is ready; that is not an error. */
    CyResult (*service_poll)(CyEngine engine, CyServiceSession session, CyServiceEvent* out_event,
                             bool* out_has_event);

    /* --- 1.3: the game services --------------------------------------------------------------
     *
     * The rules every entry below follows — ownership, errors, `struct_size`, phases, determinism,
     * threads — are stated once above `CyPhase`. Each entry adds only what is its own; `[N F U]`
     * lists the phases it answers in. */

    /* --- 1.3: time --- */

    /* [N F U] The clock and the current phase. Never refused by phase: it is how a caller learns
     * which phase it is in. Deterministic in F, where `frame_delta` and `interpolation` are zero.
     */
    CyResult (*time_get)(CyEngine engine, CyTime* out_time);

    /* --- 1.3: input --- */

    /* [N F U] The action declared under `name`, or NOT_FOUND. The index is stable for the process,
     * so resolve once and keep it. */
    CyResult (*input_find_action)(CyEngine engine, const char* name, CyInputAction* out_action);
    /* [N F U] One action's state for one input user, as resolved for the current tick (in F) or
     * the last resolved tick (otherwise). Deterministic in F: it is the state the committed command
     * frame was built from. OUT_OF_RANGE for a user the server does not have. */
    CyResult (*input_action_state)(CyEngine engine, uint32_t user, CyInputAction action,
                                   CyInputActionState* out_state);
    /* [N F U] The same by name: `input_find_action` and `input_action_state` in one call. The name
     * lookup is a hash, not a scan, but a per-tick caller should still resolve once. */
    CyResult (*input_action_state_by_name)(CyEngine engine, uint32_t user, const char* name,
                                           CyInputActionState* out_state);
    /* [N U] The user's pointer, in window pixels, with edges since the previous frame update.
     * Device state: PERMISSION_DENIED in F. A user with no pointer answers OK with
     * CY_INPUT_POINTER_PRESENT clear and everything else zero. */
    CyResult (*input_pointer)(CyEngine engine, uint32_t user, CyInputPointer* out_pointer);
    /* [N U] The CY_INPUT_MOD_* keys held now by the user's keyboard. Device state: denied in F. */
    CyResult (*input_modifiers)(CyEngine engine, uint32_t user, uint32_t* out_modifiers);
    /* [N F U] The mapping context registered under `name`, or NOT_FOUND. */
    CyResult (*input_find_context)(CyEngine engine, const char* name, CyInputContext* out_context);
    /* [N F U] Push a context onto the user's stack at `priority`; higher wins. It takes effect at
     * the next tick's resolution, never mid-tick, so a push in F is replayed identically.
     * ALREADY_EXISTS when that context is already on the user's stack. */
    CyResult (*input_push_context)(CyEngine engine, uint32_t user, CyInputContext context,
                                   int32_t priority);
    /* [N F U] Remove a context wherever it sits on the stack; NOT_FOUND when it is not there.
     * Switching context is a pop and a push. */
    CyResult (*input_pop_context)(CyEngine engine, uint32_t user, CyInputContext context);

    /* --- 1.3: camera --- */

    /* [N U] The camera of the primary view, or UNAVAILABLE when there is none. */
    CyResult (*camera_active)(CyEngine engine, CyCamera* out_camera);
    /* [N U] That camera as last evaluated: pose, projection and viewport. */
    CyResult (*camera_view)(CyEngine engine, CyCamera camera, CyCameraView* out_view);
    /* [N U] The world ray under `screen_xy` (two floats, window pixels), from the near plane to
     * the far plane — `max_distance` is set to that span. Feed it to `physics_raycast` to pick. */
    CyResult (*camera_screen_to_ray)(CyEngine engine, CyCamera camera, const float* screen_xy,
                                     CyRay* out_ray);
    /* [N U] Project `count` world points (`count * 3` floats) into `out_points`, which has room
     * for `count`. One call for a whole selection box's worth of units. */
    CyResult (*camera_world_to_screen)(CyEngine engine, CyCamera camera, const float* points_xyz,
                                       uint32_t count, CyScreenPoint* out_points);
    /* [N F U] Point the camera's rig at a focus. Presentation only; ignored while resimulating. */
    CyResult (*camera_set_target)(CyEngine engine, CyCamera camera, const CyCameraTarget* target);
    /* [N F U] Override the rig with an explicit pose until `camera_clear_pose`. */
    CyResult (*camera_set_pose)(CyEngine engine, CyCamera camera, const CyPose* pose);
    /* [N F U] Hand the camera back to its rig. OK when no override was set. */
    CyResult (*camera_clear_pose)(CyEngine engine, CyCamera camera);

    /* --- 1.3: physics queries ---
     *
     * All four are const queries over the last completed physics step. UNAVAILABLE while the step
     * itself is running (CY_STAGE_PHYSICS), because the world is mid-solve. Safe from a job worker.
     * Deterministic in F: hits are ordered by distance, then by entity, then by body creation
     * order, so equal distances never come back in a different order. */

    /* [N F U] The nearest hit. `*out_has_hit` is false, and the call OK, when nothing is hit. */
    CyResult (*physics_raycast)(CyEngine engine, const CyRay* ray, const CyQueryFilter* filter,
                                CyPhysicsHit* out_hit, bool* out_has_hit);
    /* [N F U] Every hit, nearest first. The sizing pattern `world_chunks` uses: `*out_count` is
     * always the total; a null buffer asks for it; a buffer too small is filled with the nearest
     * `capacity` hits and answers BUFFER_TOO_SMALL. */
    CyResult (*physics_raycast_all)(CyEngine engine, const CyRay* ray, const CyQueryFilter* filter,
                                    CyPhysicsHit* out_hits, uint32_t capacity, uint32_t* out_count);
    /* [N F U] Sweep `shape` from `start` along `direction` (three floats, unit) for up to
     * `max_distance` metres; the first hit. The orientation is kept for the whole sweep. */
    CyResult (*physics_shape_cast)(CyEngine engine, const CyShape* shape, const CyPose* start,
                                   const float* direction, float max_distance,
                                   const CyQueryFilter* filter, CyPhysicsHit* out_hit,
                                   bool* out_has_hit);
    /* [N F U] Every entity whose body overlaps `shape` at `pose`, ordered by entity value, each
     * entity once. The same sizing pattern as `physics_raycast_all`. */
    CyResult (*physics_overlap)(CyEngine engine, const CyShape* shape, const CyPose* pose,
                                const CyQueryFilter* filter, CyEntity* out_entities,
                                uint32_t capacity, uint32_t* out_count);

    /* --- 1.3: navigation --- */

    /* [N F U] A path now. Writes up to `capacity` points (`capacity * 3` floats) of the
     * straightened path into `out_points_xyz` and fills `out_result`, whose `point_count` is the
     * whole path's; BUFFER_TOO_SMALL when it did not fit, having written the first `capacity`. A
     * path that cannot be found is OK with CY_NAV_PATH_FOUND clear, not an error. Deterministic;
     * safe from a job worker. */
    CyResult (*nav_find_path)(CyEngine engine, const CyNavPathRequest* request,
                              float* out_points_xyz, uint32_t capacity,
                              CyNavPathResult* out_result);
    /* [F] Queue a path search. It completes a fixed number of ticks later, whatever the load,
     * which is what makes completion deterministic. */
    CyResult (*nav_request_path)(CyEngine engine, const CyNavPathRequest* request,
                                 CyNavQuery* out_query);
    /* [F] A queued search's state. PENDING: nothing written. READY: the path is written as
     * `nav_find_path` writes it and the query is consumed — unless the buffer was too small, which
     * answers BUFFER_TOO_SMALL, writes nothing and leaves it READY so the caller can retry with
     * `point_count`. CANCELLED: retired. A consumed or unknown query is NOT_FOUND. */
    CyResult (*nav_poll_path)(CyEngine engine, CyNavQuery query, float* out_points_xyz,
                              uint32_t capacity, CyNavPathResult* out_result);
    /* [F] Cancel a queued search. NOT_FOUND once it was consumed. */
    CyResult (*nav_cancel_path)(CyEngine engine, CyNavQuery query);
    /* [N F] Make `entity` a crowd agent with these parameters, or update the ones it has.
     * Structural the first time: it adds the agent component. */
    CyResult (*nav_agent_configure)(CyEngine engine, CyEntity entity,
                                    const CyNavAgentParams* params);
    /* [F] Send an agent to `target_xyz` (three floats): a path is requested, the crowd steers it
     * along the path with avoidance, and the status moves COMPUTING, FOLLOWING, then ARRIVED or
     * FAILED. NOT_FOUND when `entity` is not an agent. */
    CyResult (*nav_agent_move_to)(CyEngine engine, CyEntity entity, const float* target_xyz);
    /* [F] Stop an agent where it is; its status becomes IDLE. */
    CyResult (*nav_agent_stop)(CyEngine engine, CyEntity entity);
    /* [N F U] An agent's status, motion and target. CY_NAV_AGENT_EVENT is set for the one tick
     * after it arrived or failed, so "has it arrived" is a flag test and not a distance guess. */
    CyResult (*nav_agent_state)(CyEngine engine, CyEntity entity, CyNavAgentState* out_state);

    /* --- 1.3: audio --- */

    /* [N F U] The cue authored under `name`, or NOT_FOUND. */
    CyResult (*audio_find_cue)(CyEngine engine, const char* name, CyAudioCue* out_cue);
    /* [N F U] Start a voice. `out_voice` may be null for fire-and-forget. While resimulating, this
     * is OK, plays nothing and writes a null voice. Voice exhaustion is not an error: the mixer's
     * priority rules decide, and a voice that lost is reported by `audio_voice_playing`. */
    CyResult (*audio_play)(CyEngine engine, const CyAudioPlay* play, CyAudioVoice* out_voice);
    /* [N F U] Stop a voice, fading over `fade_out_seconds` (zero: now). A voice that already ended
     * is OK: stopping is idempotent. */
    CyResult (*audio_stop)(CyEngine engine, CyAudioVoice voice, float fade_out_seconds);
    /* [N F U] Whether the voice is still audible. False for a null or ended voice. */
    bool (*audio_voice_playing)(CyEngine engine, CyAudioVoice voice);
    /* [N F U] The bus authored under `name` (e.g. "Music", "SFX"), or NOT_FOUND. */
    CyResult (*audio_find_bus)(CyEngine engine, const char* name, CyAudioBus* out_bus);
    /* [N F U] Set a bus's linear gain, ramping over `fade_seconds`. OUT_OF_RANGE below zero. */
    CyResult (*audio_set_bus_volume)(CyEngine engine, CyAudioBus bus, float volume,
                                     float fade_seconds);

    /* --- 1.3: spawning --- */

    /* [N F U] Resolve a prefab or scene asset by its content path. In N it may load the asset; in F
     * and U the asset must already be resident, and UNAVAILABLE says it is not — preload it at N.
     */
    CyResult (*spawn_resolve)(CyEngine engine, const char* asset, CyPrefab* out_prefab);
    /* [N F] Instantiate a prefab under `params->parent` at `params->pose`, returning its root.
     * Structural. The whole instance exists when this returns and its behaviours' `create` has
     * run; tree callbacks follow at the next pump. Deterministic in F: the same calls in the same
     * order produce the same entities. */
    CyResult (*spawn_instantiate)(CyEngine engine, CyPrefab prefab, const CySpawnParams* params,
                                  CyEntity* out_root);
    /* [N F] `count` instances under `parent`, one per pose, in one batch. `out_roots` has room for
     * `count`. The batch is created at once or not at all. */
    CyResult (*spawn_instantiate_many)(CyEngine engine, CyPrefab prefab, CyEntity parent,
                                       const CyPose* poses, uint32_t count, CyEntity* out_roots);
    /* [N F] Destroy `root` and its whole subtree, children first, running each node's exit and
     * destroy callbacks. NOT_FOUND for an entity that is not alive. */
    CyResult (*spawn_destroy)(CyEngine engine, CyEntity root);
    /* --- 1.4: live scene VFX instances ------------------------------------------------------- */

    /* `entity` is the Play entity carrying the authored effect. `emitter` is empty for a system
     * parameter and names the owning emitter for a local parameter. Names are borrowed, terminated
     * UTF-8. Only exposed parameters can cross this boundary. The value tag must match the
     * declaration's scalar or vector type; the engine copies the value during the call. */
    CyResult (*vfx_effect_parameter_set)(CyEngine engine, CyEntity entity, const char* emitter,
                                         const char* parameter, const CyVar* value);
    /* Returns the current typed value from that one playing instance. The value is inline and
     * carries no owned payload, so `var_release` is safe but unnecessary. */
    CyResult (*vfx_effect_parameter_get)(CyEngine engine, CyEntity entity, const char* emitter,
                                         const char* parameter, CyVar* out_value);

    /* --- 1.5: scheduled systems --- */

    /* [N] Schedule a module's system into `desc->stage`. The engine copies the access list,
     * resolves it into the scheduler's declaration and installs the system when the embedder next
     * installs script systems (`cy::abi::ScriptSystems`). Within its stage the system is ordered
     * against every other by its declared access, exactly as a native system is, and runs in that
     * stage's phase: F for CY_STAGE_PRE_SIMULATION to CY_STAGE_POST_SIMULATION, U for
     * CY_STAGE_FRAME to CY_STAGE_UI, N for CY_STAGE_RENDER. Deterministic in F: the schedule is
     * built from the declarations, ties broken by registration order, and a body sees the same
     * chunks in the same order.
     *
     * Registering a name again in the same generation replaces it. In a LATER generation (a hot
     * reload) the stage and the access must be unchanged, or the registration is refused with
     * UNSUPPORTED and the reload is refused with it: a scheduled system cannot be re-ordered in a
     * running schedule. INVALID_ARGUMENT for an unknown stage or mode, a component the bound world
     * does not have, or a component declared twice; UNAVAILABLE with no world bound. */
    CyResult (*register_system)(CyEngine engine, const CySystemDesc* desc);

    /* --- 1.5: the scene tree --- */

    /* [N F U] The node at `path`, relative to the node `from` (`Camera`, `../Camera`, `./Rig/Arm`)
     * or absolute from the tree's root (`/Level/Player`). NOT_FOUND when the path does not resolve
     * — which is how `@Node(path)` becomes nil rather than a trap. Deterministic: names are unique
     * among siblings, so a path names one node or none. */
    CyResult (*node_find)(CyEngine engine, CyEntity from, const char* path, CyEntity* out_entity);

    /* --- 1.5: rigid bodies ---
     *
     * Simulation writes on the body `entity` owns, through the embedder's entity-to-body map.
     * NOT_FOUND for an entity with no body; INVALID_ARGUMENT for a non-finite vector or for a
     * force, impulse or torque on a body that is not dynamic; UNAVAILABLE while the physics step
     * runs. Forces and torques accumulate until the next step and are cleared by it; impulses and
     * velocities apply at once. In F they take effect in call order, which is script order —
     * deterministic. */

    /* [N F] Add a force (newtons, world space) at the centre of mass for the next step. */
    CyResult (*physics_apply_force)(CyEngine engine, CyEntity entity, const float* force_xyz);
    /* [N F] Add an impulse (newton seconds, world space). `point_xyz` is a world-space point of
     * application, or null for the centre of mass. Wakes the body. */
    CyResult (*physics_apply_impulse)(CyEngine engine, CyEntity entity, const float* impulse_xyz,
                                      const float* point_xyz);
    /* [N F] Add a torque (newton metres, world space) for the next step. */
    CyResult (*physics_apply_torque)(CyEngine engine, CyEntity entity, const float* torque_xyz);
    /* [N F] Replace the body's velocity. Either pointer may be null, keeping that half. */
    CyResult (*physics_set_velocity)(CyEngine engine, CyEntity entity, const float* linear_xyz,
                                     const float* angular_xyz);
    /* [N F U] The body's linear and angular velocity after the last step and any writes since.
     * Either output may be null. */
    CyResult (*physics_get_velocity)(CyEngine engine, CyEntity entity, float* out_linear_xyz,
                                     float* out_angular_xyz);

    /* --- 1.5: character controllers ---
     *
     * `cy::physics::CharacterController`: a capsule moved by collide-and-slide over the server's
     * queries, identical over every backend. One per entity; the controller owns a kinematic body
     * carrying the entity, so a ray cast hits the character and names it. */

    /* [N F] Give `entity` a character. ALREADY_EXISTS when it has one. */
    CyResult (*character_create)(CyEngine engine, CyEntity entity, const CyCharacterDesc* desc);
    /* [N F] Remove it and its body. NOT_FOUND when it has none. */
    CyResult (*character_destroy)(CyEngine engine, CyEntity entity);
    /* [F] One fixed step of `CyTime.fixed_delta`: gravity, the slide, stairs, the platform. Applied
     * at the call, so the state reads back at once; characters moved in one tick move in call
     * order, which is script order — deterministic. UNAVAILABLE while the physics step runs. */
    CyResult (*character_move)(CyEngine engine, CyEntity entity, const CyCharacterInput* input);
    /* [N F U] What the last move produced. */
    CyResult (*character_state)(CyEngine engine, CyEntity entity, CyCharacterState* out_state);

    /* --- 1.6: the runtime interface ---
     *
     * Every entry is [N U]. UNAVAILABLE when the embedder bound no interface; NOT_FOUND for a
     * stale element or one this module did not create; INVALID_ARGUMENT for a write the element's
     * kind does not take, a non-finite number, or a malformed struct. */

    /* [N U] The screen's root: an ABSOLUTE container covering the window, which every module's
     * interface is built under. It cannot be written or destroyed. */
    CyResult (*ui_root)(CyEngine engine, CyUiElement* out_root);
    /* [N U] A new element, the last child of `parent` — drawn over its earlier siblings. Visible,
     * laid out by a zeroed `CyUiLayout`, drawing nothing until styled. */
    CyResult (*ui_create)(CyEngine engine, CyUiElement parent, const CyUiElementDesc* desc,
                          CyUiElement* out_element);
    /* [N U] Destroy an element and everything under it. Their handles answer NOT_FOUND after. */
    CyResult (*ui_destroy)(CyEngine engine, CyUiElement element);
    /* [N U] Replace the element's layout input. */
    CyResult (*ui_set_layout)(CyEngine engine, CyUiElement element, const CyUiLayout* layout);
    /* [N U] Replace what the element draws. */
    CyResult (*ui_set_style)(CyEngine engine, CyUiElement element, const CyUiStyle* style);
    /* [N U] A label's or a button's text, UTF-8, copied: one line in the built-in font at a whole
     * `pixel_scale` (zero is 1). The element is measured again. */
    CyResult (*ui_set_text)(CyEngine engine, CyUiElement element, const char* utf8, uint32_t colour,
                            uint32_t pixel_scale);
    /* [N U] An image's atlas page and its rectangle within it, x y width height in [0, 1]; a null
     * rectangle is the whole page. The embedder uploads the pages. */
    CyResult (*ui_set_image)(CyEngine engine, CyUiElement element, uint32_t atlas_page,
                             const float* uv_xywh);
    /* [N U] A progress bar's fraction, clamped to [0, 1]. */
    CyResult (*ui_set_progress)(CyEngine engine, CyUiElement element, float value);
    /* [N U] CyUiVisibility. */
    CyResult (*ui_set_visibility)(CyEngine engine, CyUiElement element, uint32_t visibility);
    /* [N U] The element's opacity in [0, 1], multiplied down the tree into every colour beneath. */
    CyResult (*ui_set_opacity)(CyEngine engine, CyUiElement element, float opacity);
    /* [N U] Where the last layout put the element: x y width height in window pixels. Zero before
     * the first layout after it was created. */
    CyResult (*ui_element_rect)(CyEngine engine, CyUiElement element, float* out_rect_xywh);
    /* [N U] The element of this module's under a window point, as the last layout placed it: the
     * topmost hit, or its nearest ancestor this module created. CY_UI_ELEMENT_NULL when the point
     * is over no such element — which is how a game tells a click on its HUD from one on the world.
     */
    CyResult (*ui_hit_test)(CyEngine engine, const float* position_xy, CyUiElement* out_element);
    /* [N U] The element with keyboard focus, if this module created it; null otherwise. */
    CyResult (*ui_focus)(CyEngine engine, CyUiElement* out_element);
    /* [N U] Move keyboard focus to a button, or clear it with CY_UI_ELEMENT_NULL. INVALID_ARGUMENT
     * for an element that does not take focus. */
    CyResult (*ui_set_focus)(CyEngine engine, CyUiElement element);
    /* --- 1.7: animation ---
     *
     * `cy::animation::AnimationSystem`, through `cy::abi::game::AnimationBackend`. NOT_FOUND for
     * an entity with no animator, an unknown rig, state, parameter or joint; INVALID_ARGUMENT for a
     * non-finite number, a negative duration or an unknown enumerator. Requests made in F take
     * effect at the tick's animation advance, in call order — deterministic. */

    /* [N F] Give `entity` an animator over the rig `desc->rig`. Structural. ALREADY_EXISTS when
     * it has one. The instance exists when the call returns, so the entity can be played at once.
     */
    CyResult (*animation_attach)(CyEngine engine, CyEntity entity, const CyAnimatorDesc* desc);
    /* [N F] Remove it. Structural. */
    CyResult (*animation_detach)(CyEngine engine, CyEntity entity);
    /* [N F] PLAY the state `state`: crossfade from where the machine is over `crossfade_seconds`,
     * or cut when it is zero, whatever the program's own transitions say. A requested blend runs
     * to completion; the program is consulted again from the state it lands in. */
    CyResult (*animation_play)(CyEngine engine, CyEntity entity, const char* state,
                               float crossfade_seconds);
    /* [N F] Blend back to the program's entry state over `blend_seconds`. */
    CyResult (*animation_stop)(CyEngine engine, CyEntity entity, float blend_seconds);
    /* [N F] Set a program parameter: a blend weight, a speed, a condition. */
    CyResult (*animation_set_float)(CyEngine engine, CyEntity entity, const char* parameter,
                                    float value);
    /* [N F] Set a condition parameter to 1 or 0. */
    CyResult (*animation_set_bool)(CyEngine engine, CyEntity entity, const char* parameter,
                                   bool value);
    /* [N F] A TRIGGER: the parameter reads 1 for exactly the next tick's advance, then 0. */
    CyResult (*animation_fire_trigger)(CyEngine engine, CyEntity entity, const char* parameter);
    /* [N F U] A parameter's value. */
    CyResult (*animation_get_float)(CyEngine engine, CyEntity entity, const char* parameter,
                                    float* out_value);
    /* [N F U] Where the state machine is. */
    CyResult (*animation_state)(CyEngine engine, CyEntity entity, CyAnimatorState* out_state);
    /* [N U] The events every animator's clips fired in the ticks since the previous frame, once
     * per frame: in tick, then rig, then instance order; empty in a frame that ran no tick. The
     * sizing pattern of `world_chunks`: `*out_count` is always the number there are, and
     * BUFFER_TOO_SMALL with nothing written when `capacity` is less. Null `out_events` with zero
     * capacity asks for the count. Events suppressed by CY_ANIMATOR_SUPPRESS_EVENTS are not here.
     */
    CyResult (*animation_events)(CyEngine engine, CyAnimationEvent* out_events, uint32_t capacity,
                                 uint32_t* out_count);
    /* [N F U] The last tick's root motion delta and the running total. */
    CyResult (*animation_root_motion)(CyEngine engine, CyEntity entity, CyRootMotion* out_motion);
    /* [F] Everything accumulated since the last take, composed in tick order, and clear it. For an
     * animator in CY_ROOT_MOTION_ACCUMULATE; NOT_FOUND in any other mode. */
    CyResult (*animation_take_root_motion)(CyEngine engine, CyEntity entity,
                                           CyRootMotion* out_motion);
    /* [N F] Choose where root motion goes, CyRootMotionMode. CHARACTER requires the entity to own
     * a character controller (`character_create`): NOT_FOUND otherwise. */
    CyResult (*animation_set_root_motion)(CyEngine engine, CyEntity entity, uint32_t mode);
    /* [N U] Joint `joint`'s placement in the WORLD, from the pose last evaluated: what a weapon or
     * an effect is attached to. Presentation, so not in F. */
    CyResult (*animation_joint_pose)(CyEngine engine, CyEntity entity, const char* joint,
                                     CyPose* out_pose);
    /* --- 1.8: deterministic math and the lockstep path ---
     *
     * The typed fast path for a CY_VAR_FIXED field, and the deterministic transcendentals, all
     * [N F U] and pure; then the lockstep session. */

    /* [N F U] A CY_VAR_FIXED field's raw value. INVALID_ARGUMENT for a field of another kind. */
    CyResult (*component_get_fixed)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                    uint32_t field, CyFixed* out_value);
    /* [N F U] Write a CY_VAR_FIXED field's raw value. INVALID_ARGUMENT for another kind. */
    CyResult (*component_set_fixed)(CyWorld world, CyEntity entity, CyComponentTypeId component,
                                    uint32_t field, CyFixed value);
    /* `cy::detmath::kKernelVersion`: a different number is a different kernel, and a replay or a
     * peer recorded with another is not this session's. */
    uint32_t (*detmath_kernel_version)(void);
    /* Each `cy::detmath` function of the same name; see CyDetmathFunction for the formats. */
    CyFixed (*detmath_sqrt)(CyFixed x);
    CyFixed (*detmath_sin)(CyAngle a);
    CyFixed (*detmath_cos)(CyAngle a);
    CyFixed (*detmath_tan)(CyAngle a);
    CyAngle (*detmath_atan)(CyFixed x);
    CyAngle (*detmath_atan2)(CyFixed y, CyFixed x);
    CyAngle (*detmath_asin)(CyFixed x);
    CyAngle (*detmath_acos)(CyFixed x);
    CyFixed (*detmath_exp2)(CyFixed x);
    CyFixed (*detmath_log2)(CyFixed x);
    CyFixed (*detmath_exp)(CyFixed x);
    CyFixed (*detmath_log)(CyFixed x);
    CyFixed (*detmath_pow)(CyFixed x, CyFixed y);
    /* `function` over `count` inputs: `out[i] = f(x[i])`, or `f(x[i], y[i])` for ATAN2 and POW,
     * which need `y`; `y` may be null for the others. INVALID_ARGUMENT for an unknown function or a
     * null span with a non-zero count, having written nothing. */
    CyResult (*detmath_evaluate)(uint32_t function, const CyFixed* x, const CyFixed* y,
                                 CyFixed* out, uint64_t count);
    /* [N] Enlist a unit in the lockstep session, in the order units are to be processed: before
     * the first tick only (PERMISSION_DENIED after it). `*out_unit` is its index, which is its
     * identity in the session's hash — the entity is presentation and is not hashed. */
    CyResult (*lockstep_enlist)(CyEngine engine, const CyLockstepUnitDesc* desc,
                                uint32_t* out_unit);
    /* [F U] Record an order for the session's next tick. NOT_FOUND for a group no unit is in. */
    CyResult (*lockstep_order)(CyEngine engine, const CyLockstepOrder* order);
    /* [N F U] A unit's authoritative state. NOT_FOUND for an index never enlisted. */
    CyResult (*lockstep_unit)(CyEngine engine, uint32_t unit, CyLockstepUnit* out_unit);
    /* [N F U] The session: its tick, its log, its hash and its digest. */
    CyResult (*lockstep_status)(CyEngine engine, CyLockstepStatus* out_status);
    /* --- 1.9: global illumination ---
     *
     * Every entry is [N U]: illumination is presentation (see CyGiMode's section). */

    /* [N U] The running world's GI settings. */
    CyResult (*gi_get_settings)(CyEngine engine, CyGiSettings* out_settings);
    /* [N U] Apply settings from the next frame on. A change of quality or tier places every probe
     * again; hardware ray tracing switches without a rebuild. INVALID_ARGUMENT for an enumerator
     * out of range or a budget that is not positive, having changed nothing. */
    CyResult (*gi_set_settings)(CyEngine engine, const CyGiSettings* settings);
    /* [N U] Place a GI volume. INVALID_ARGUMENT for an empty box, a weight outside [0, 1] or a set
     * parameter out of its range. */
    CyResult (*gi_volume_create)(CyEngine engine, const CyGiVolumeDesc* desc,
                                 CyGiVolume* out_volume);
    /* [N U] Replace a volume's description: move it, resize it, change what it sets. */
    CyResult (*gi_volume_update)(CyEngine engine, CyGiVolume volume, const CyGiVolumeDesc* desc);
    /* [N U] Remove a volume; its region is placed again at the global settings. */
    CyResult (*gi_volume_destroy)(CyEngine engine, CyGiVolume volume);
    /* [N U] An object's illumination class, CyGiClass; AUTOMATIC clears an override. NOT_FOUND for
     * an entity the host did not register with the GI. */
    CyResult (*gi_object_set_class)(CyEngine engine, CyEntity entity, uint32_t klass);
    /* [N U] The class an object has now, after any override: never AUTOMATIC. */
    CyResult (*gi_object_class)(CyEngine engine, CyEntity entity, uint32_t* out_class);
    /* [N U] What the GI makes of an object's or a material's emission, CyGiEmission. `entity`
     * CY_ENTITY_NULL is every object; `material` is CY_NAME_HASH of a material's name, or 0 for
     * every material. The most specific rule decides; AUTOMATIC removes the rule for exactly that
     * pair. NOT_FOUND for an entity or material the host did not register. */
    CyResult (*gi_emission_set)(CyEngine engine, CyEntity entity, uint64_t material, uint32_t mode);
    /* [N U] The indirect light arriving at `position_xyz` about `normal_xyz`, or, with a null
     * normal, the mean over the six axis directions: what a body standing there receives. */
    CyResult (*gi_sample_irradiance)(CyEngine engine, const float* position_xyz,
                                     const float* normal_xyz, CyGiIrradiance* out_irradiance);
    /* [N U] What the GI did on the last frame. */
    CyResult (*gi_status)(CyEngine engine, CyGiStatus* out_status);
    /* --- 1.10: fog of war ---
     *
     * `fog_set_vision` is [N F]; the questions are [N F U] (see CyFogCell's section). */

    /* [N F] Give `entity` a vision: its team's sight to `reveal_radius`, from the next update.
     * INVALID_ARGUMENT for a team past 32, a negative radius, an unknown flag or a nonzero
     * `reserved`, having changed nothing. NOT_FOUND for an entity the host does not place. */
    CyResult (*fog_set_vision)(CyEngine engine, CyEntity entity, const CyFogVision* vision);
    /* [N F U] The vision `entity` was given: its team, its reveal radius and its flags. NOT_FOUND
     * for an entity never given one. */
    CyResult (*fog_get_vision)(CyEngine engine, CyEntity entity, CyFogVision* out_vision);
    /* [N F U] How `team` knows the cell holding `position`, a CyFogCell. Off the grid, or team 0,
     * is UNEXPLORED. */
    CyResult (*fog_cell)(CyEngine engine, uint32_t team, const CyFixedVec2* position,
                         uint32_t* out_cell);
    /* [N F U] How `team` sees `entity`, a CyFogSight: what decides whether a renderer draws it, and
     * where. HIDDEN for an entity the fog has never placed. */
    CyResult (*fog_sight)(CyEngine engine, uint32_t team, CyEntity entity, uint32_t* out_sight);
    /* [N F U] The grid and its last update. */
    CyResult (*fog_status)(CyEngine engine, CyFogStatus* out_status);
    /* --- 1.11: lights ---
     *
     * Every entry is [N U] (see CyLightKind's section). */

    /* [N U] A light, from this frame. INVALID_ARGUMENT for a kind, a fade or a flag out of range, a
     * negative intensity, range, size or lifetime, or a nonzero reserved word, having made nothing.
     * OUT_OF_RANGE with every slot of the pool taken. */
    CyResult (*light_spawn)(CyEngine engine, const CyLightDesc* desc, CyLight* out_light);
    /* [N U] Replace a live light's description — move a torch, recolour a flash — keeping its age;
     * `lifetime` and `fade` are its spawn's and are not read. NOT_FOUND for a light that ended. */
    CyResult (*light_update)(CyEngine engine, CyLight light, const CyLightDesc* desc);
    /* [N U] End a light now. NOT_FOUND for one that already ended. */
    CyResult (*light_destroy)(CyEngine engine, CyLight light);
    /* [N U] Whether a light is alive: 1, or 0 for one that ended or never was. */
    CyResult (*light_alive)(CyEngine engine, CyLight light, uint32_t* out_alive);
    /* [N U] The pool. */
    CyResult (*light_status)(CyEngine engine, CyLightStatus* out_status);
    /* --- 1.12: the widget toolkit ---
     *
     * Every entry is [N U] (see CyUiWidgetKind's section). A widget a module did not make, or that
     * is gone, answers NOT_FOUND; a write the widget's kind does not take, INVALID_ARGUMENT. */

    /* [N U] Make a document's widgets under `parent` (a widget, or CY_UI_ELEMENT_NULL for the
     * toolkit's root). `*out_root` is its first top-level widget. INVALID_ARGUMENT for a document
     * that does not parse, naming its first mistake's line, having made nothing. */
    CyResult (*ui_load_document)(CyEngine engine, CyUiElement parent, const CyUiDocumentDesc* desc,
                                 CyUiElement* out_root);
    /* [N U] Replace the toolkit's style sheet — `.cyss`, over the built-in theme. INVALID_ARGUMENT
     * for a sheet with a mistake, naming it; what parsed still applies. */
    CyResult (*ui_load_stylesheet)(CyEngine engine, const char* utf8, uint64_t size);
    /* [N U] The first widget at or under `scope` (null: everything this module made) whose id is
     * `id`; CY_UI_ELEMENT_NULL when there is none. */
    CyResult (*ui_find)(CyEngine engine, CyUiElement scope, const char* id,
                        CyUiElement* out_widget);
    /* [N U] A widget, the last child of `parent` (a widget, or null for the toolkit's root). */
    CyResult (*ui_create_widget)(CyEngine engine, CyUiElement parent, const CyUiWidgetDesc* desc,
                                 CyUiElement* out_widget);
    /* [N U] A caption, a text field's text, a rich text's markup. */
    CyResult (*ui_widget_set_text)(CyEngine engine, CyUiElement widget, const char* utf8);
    /* [N U] A widget's text into `buffer`, NUL-terminated and cut at a code point when it does not
     * fit; `*out_size` is its whole length in bytes, without the NUL. A null buffer asks the size.
     */
    CyResult (*ui_widget_text)(CyEngine engine, CyUiElement widget, char* buffer, uint32_t capacity,
                               uint32_t* out_size);
    /* [N U] A slider's or progress bar's value, clamped to its range; a toggle's or a checkbox's
     * state, on at 0.5 and above. Finite. No CHANGED event: the game set it. */
    CyResult (*ui_widget_set_value)(CyEngine engine, CyUiElement widget, float value);
    CyResult (*ui_widget_value)(CyEngine engine, CyUiElement widget, float* out_value);
    /* [N U] A radio group's, drop-down's, list's or tabs' selected index, clamped to its items. */
    CyResult (*ui_widget_set_selected)(CyEngine engine, CyUiElement widget, uint32_t index);
    CyResult (*ui_widget_selected)(CyEngine engine, CyUiElement widget, uint32_t* out_index);
    /* [N U] A radio group's, drop-down's or list's items, each borrowed for the call and copied. A
     * null list is no items. */
    CyResult (*ui_widget_set_items)(CyEngine engine, CyUiElement widget, const char* const* items,
                                    uint32_t count);
    /* [N U] Taking input, or refusing it and drawn as `:disabled`. */
    CyResult (*ui_widget_set_enabled)(CyEngine engine, CyUiElement widget, uint32_t enabled);
    /* [N U] Space-separated classes, at most four, replacing the widget's: what a sheet's `.name`
     * selects, and how a variant is chosen. */
    CyResult (*ui_widget_set_classes)(CyEngine engine, CyUiElement widget, const char* classes);
    /* [N U] Open or close a drop-down's list or a dialog. A dialog opens modal: it takes focus and
     * the input until it closes, and focus goes back where it was. */
    CyResult (*ui_widget_open)(CyEngine engine, CyUiElement widget, uint32_t open);
    /* [N U] Start an animation of one of the widget's paint properties, replacing one of the same
     * property. INVALID_ARGUMENT for an unknown property or easing, or a negative or non-finite
     * time. */
    CyResult (*ui_widget_animate)(CyEngine engine, CyUiElement widget,
                                  const CyUiAnimation* animation);
    /* [N U] Deliver a CyUiAction — a game that maps its own input drives the focus with this.
     * `*out_consumed` is 1 when the interface took it, 0 when it is the game's. */
    CyResult (*ui_navigate)(CyEngine engine, uint32_t action, uint32_t* out_consumed);
    /* --- 1.13: the first-person camera and the weapon view ---
     *
     * Every entry is [N U] (see CyFirstPersonDesc's section). A camera that has no first-person
     * behaviours answers NOT_FOUND, except to `first_person_configure`, which gives it some. */

    /* [N U] Give `camera` first-person behaviours, or replace their settings keeping the state —
     * the look, the bob, the recoil, the sights. INVALID_ARGUMENT for a number out of its range, an
     * unknown flag or a null camera, having changed nothing. OUT_OF_RANGE with every slot taken. */
    CyResult (*first_person_configure)(CyEngine engine, CyCamera camera,
                                       const CyFirstPersonDesc* desc);
    /* [N U] This frame's input, added to what arrived since the last frame: the look deltas sum,
     * the speed and the flags are the latest. Applied when the host advances the camera. */
    CyResult (*first_person_input)(CyEngine engine, CyCamera camera,
                                   const CyFirstPersonInput* input);
    /* [N U] A shot's kick: an impulse into the recoil spring, radians a second up and right. */
    CyResult (*first_person_kick)(CyEngine engine, CyCamera camera, float pitch, float yaw);
    /* [N U] A cut: the look set, the bob, the recoil and the sights at rest. */
    CyResult (*first_person_reset)(CyEngine engine, CyCamera camera, float yaw, float pitch);
    /* [N U] What the camera's last frame produced. */
    CyResult (*first_person_state)(CyEngine engine, CyCamera camera, CyFirstPersonState* out_state);
    /* [N U] Take the behaviours, and the weapon view, away from `camera`. */
    CyResult (*first_person_release)(CyEngine engine, CyCamera camera);
    /* [N U] Set `camera`'s weapon view — `layers` zero turns it off. INVALID_ARGUMENT for a lens
     * out of range, an unknown flag or a nonzero reserved word, having changed nothing. NOT_FOUND
     * for a camera with no first-person behaviours. */
    CyResult (*viewmodel_configure)(CyEngine engine, CyCamera camera, const CyViewmodelDesc* desc);
    /* [N U] `camera`'s weapon view as it was set, `fov` the one it draws with now. */
    CyResult (*viewmodel_get)(CyEngine engine, CyCamera camera, CyViewmodelDesc* out_desc);
    /* --- 1.14: foot placement and aim (see CyFootPlacement's section) --- */

    /* [N F] Point the program's aim at a place in the WORLD: `target` names the aim node's
     * `target_parameter`, and the point is taken into the character's model space through the
     * entity's placement as the scene last propagated it, and written to `<target>.x`, `.y` and
     * `.z`. NOT_FOUND for an entity with no animator or a program without those parameters;
     * INVALID_ARGUMENT for a non-finite point. The aim's weight is its own parameter, set with
     * `animation_set_float`. */
    CyResult (*animation_aim_at)(CyEngine engine, CyEntity entity, const char* target,
                                 const float* point_xyz);
    /* [N U] What the program's foot placement `index` (in graph order, from 0) did when the pose
     * was last evaluated. NOT_FOUND for an entity with no animator or a program with no such foot
     * placement. Presentation, so not in F. */
    CyResult (*animation_foot_placement)(CyEngine engine, CyEntity entity, uint32_t index,
                                         CyFootPlacement* out_placement);
    /* --- 1.15: effects and decals (see CyEffectPlay's section) --- */

    /* [N F U] The effect the host registered under `name`. NOT_FOUND for a name it has not. */
    CyResult (*effect_find)(CyEngine engine, const char* name, CyEffect* out_effect);
    /* [N U] Play `effect` once, fire-and-forget. INVALID_ARGUMENT for a parameter count past
     * eight, a scale that is not positive, a number that is not finite or a nonzero reserved word,
     * having played nothing; NOT_FOUND for an effect the host did not hand out; OUT_OF_RANGE when
     * the simulation world has no room for another instance. */
    CyResult (*effect_play)(CyEngine engine, CyEffect effect, const CyEffectPlay* play);
    /* [N F U] The decal material the host registered under `name`. NOT_FOUND for one it has not. */
    CyResult (*decal_find_material)(CyEngine engine, const char* name,
                                    CyDecalMaterial* out_material);
    /* [N U] A decal, from this frame. INVALID_ARGUMENT for a half extent that is not positive, an
     * importance outside 0 to 1, a fade angle outside 0 to pi/2, a fade end not past its start, a
     * number that is not finite or a nonzero reserved word, having spawned nothing; NOT_FOUND for
     * a material the host did not hand out. A full budget evicts the least worth keeping:
     * `*out_decal` is CY_DECAL_NULL when that was the new one. */
    CyResult (*decal_spawn)(CyEngine engine, const CyDecalDesc* desc, CyDecal* out_decal);
    /* [N U] The decal budget. */
    CyResult (*decal_status)(CyEngine engine, CyDecalStatus* out_status);
    /* --- 1.16: combat in the lockstep path (see CyLockstepCombatDesc's section) --- */

    /* [N] Give unit `unit` health and a weapon, before the first tick. NOT_FOUND for a unit never
     * enlisted; INVALID_ARGUMENT for no health, no damage, no cooldown, a negative reach, an
     * unknown weapon, a ranged weapon with no projectile speed or a nonzero reserved word;
     * PERMISSION_DENIED once the session has ticked. */
    CyResult (*lockstep_arm)(CyEngine engine, uint32_t unit, const CyLockstepCombatDesc* desc);
    /* [N] Place a structure, before the first tick; `*out_structure` is its index. INVALID_ARGUMENT
     * for no health or no footprint; PERMISSION_DENIED once the session has ticked. */
    CyResult (*lockstep_fortify)(CyEngine engine, const CyLockstepStructureDesc* desc,
                                 uint32_t* out_structure);
    /* [N F U] Unit `unit`'s combat state. NOT_FOUND for a unit never enlisted. */
    CyResult (*lockstep_combatant)(CyEngine engine, uint32_t unit,
                                   CyLockstepCombatant* out_combatant);
    /* [N F U] Structure `structure`'s state. NOT_FOUND for one never fortified. */
    CyResult (*lockstep_structure)(CyEngine engine, uint32_t structure,
                                   CyLockstepStructure* out_structure);
    /* [N F U] The events numbered `first` and after, oldest first, up to `capacity`; a null
     * buffer with zero capacity asks for the count. The host keeps the most recent events of a
     * bounded history: a caller that falls further behind sees a gap in the numbers. */
    CyResult (*lockstep_events)(CyEngine engine, uint64_t first, CyLockstepEvent* out_events,
                                uint32_t capacity, uint32_t* out_count);
    /* [N F U] The projectiles in flight, in launch order, up to `capacity`; a null buffer with
     * zero capacity asks for the count. */
    CyResult (*lockstep_projectiles)(CyEngine engine, CyLockstepProjectile* out_projectiles,
                                     uint32_t capacity, uint32_t* out_count);
    /* --- 1.17: what a game's menus need ---
     *
     * Every entry is [N U] (see CyInputRebind's section). */

    /* [N U] Begin rebinding `action`'s `slot`-th control on `scheme` (CyInputScheme) for `user`,
     * replacing a rebind already listening. `conflict` is a CyInputConflict. INVALID_ARGUMENT for
     * a scheme or a policy out of range; NOT_FOUND for an action the server does not have;
     * OUT_OF_RANGE for a user it does not have. */
    CyResult (*input_rebind_begin)(CyEngine engine, uint32_t user, CyInputAction action,
                                   uint32_t scheme, uint32_t slot, uint32_t conflict);
    /* [N U] Where `user`'s rebind is: IDLE when none was begun. */
    CyResult (*input_rebind_poll)(CyEngine engine, uint32_t user, CyInputRebind* out_rebind);
    /* [N U] Cancel `user`'s rebind; OK when none is listening. */
    CyResult (*input_rebind_cancel)(CyEngine engine, uint32_t user);
    /* [N U] The control `action`'s `slot`-th binding on `scheme` has now for `user` — the player's
     * override, or the authored one — as a path into `buffer`, NUL-terminated; `*out_size` is its
     * length without the NUL; a null buffer asks the size. NOT_FOUND when the action has no such
     * binding on the user's contexts. */
    CyResult (*input_binding)(CyEngine engine, uint32_t user, CyInputAction action, uint32_t scheme,
                              uint32_t slot, char* buffer, uint32_t capacity, uint32_t* out_size);
    /* [N U] `user`'s profile — every override — as text into `buffer`, the sizing of
     * `input_binding`. Empty for a user who changed nothing. */
    CyResult (*input_profile_save)(CyEngine engine, uint32_t user, char* buffer, uint32_t capacity,
                                   uint32_t* out_size);
    /* [N U] Replace `user`'s overrides with a profile `input_profile_save` wrote, effective from
     * the next tick. INVALID_ARGUMENT naming the first line that does not read, having changed
     * nothing; a line naming an action or a control this build does not have is skipped. */
    CyResult (*input_profile_load)(CyEngine engine, uint32_t user, const char* text, uint64_t size);
    /* [N U] Drop every override of `user`: the authored bindings again. */
    CyResult (*input_profile_reset)(CyEngine engine, uint32_t user);
    /* [N U] The display settings in effect. */
    CyResult (*video_get_settings)(CyEngine engine, CyVideoSettings* out_settings);
    /* [N U] Apply display settings from the next frame. INVALID_ARGUMENT for a quality, an
     * upscaler or a flag out of range; NOT_FOUND for a resolution `video_modes` does not list;
     * having changed nothing either way. */
    CyResult (*video_set_settings)(CyEngine engine, const CyVideoSettings* settings);
    /* [N U] The resolutions the display offers, the sizing of `physics_raycast_all`. */
    CyResult (*video_modes)(CyEngine engine, CyVideoMode* out_modes, uint32_t capacity,
                            uint32_t* out_count);
    /* [N U] Write `size` bytes to the player's file `name` in the game's user-data directory,
     * replacing it whole. `name` is one plain file name: letters, digits, '.', '-' and '_'. */
    CyResult (*user_data_write)(CyEngine engine, const char* name, const void* bytes,
                                uint64_t size);
    /* [N U] Read the player's file `name` into `buffer`; `*out_size` is its whole size, a null
     * buffer asks it, and BUFFER_TOO_SMALL fills `capacity` bytes. NOT_FOUND when there is none. */
    CyResult (*user_data_read)(CyEngine engine, const char* name, void* buffer, uint64_t capacity,
                               uint64_t* out_size);
    /* [N U] The tooltip a widget shows when the pointer rests on it, or a gamepad focuses it
     * long enough; null or empty removes it. */
    CyResult (*ui_widget_set_tooltip)(CyEngine engine, CyUiElement widget, const char* utf8);
    /* --- 1.18: streamed music and ambience (see CyAudioStreamPlay's section) --- */

    /* [N F U] A track the host registered, by name. NOT_FOUND. */
    CyResult (*audio_find_track)(CyEngine engine, const char* name, CyAudioTrack* out_track);
    /* [N F U] Stream a track. INVALID_ARGUMENT for a negative or non-finite volume, fade or start;
     * NOT_FOUND for a track the host did not hand out; OUT_OF_RANGE for a start past the track's
     * end or no free stream; UNAVAILABLE when this build cannot decode the track's codec.
     * `*out_stream` is null during a resimulated tick. */
    CyResult (*audio_stream_play)(CyEngine engine, const CyAudioStreamPlay* play,
                                  CyAudioStream* out_stream);
    /* [N F U] Fade out over `fade_out_seconds` (zero: now) and stop. Idempotent: an ended stream is
     * OK. */
    CyResult (*audio_stream_stop)(CyEngine engine, CyAudioStream stream, float fade_out_seconds);
    /* [N F U] Jump to `seconds` in the track: the old position fades out over one mixer block and
     * the new one ramps in once the ring is primed. NOT_FOUND for an ended stream; OUT_OF_RANGE
     * past the end. */
    CyResult (*audio_stream_seek)(CyEngine engine, CyAudioStream stream, double seconds);
    /* [N F U] Ramp the stream's linear gain over `fade_seconds`. NOT_FOUND for an ended stream. */
    CyResult (*audio_stream_set_volume)(CyEngine engine, CyAudioStream stream, float volume,
                                        float fade_seconds);
    /* [N F U] A stream's position, length, gain and underruns. NOT_FOUND once it has ended. */
    CyResult (*audio_stream_state)(CyEngine engine, CyAudioStream stream,
                                   CyAudioStreamState* out_state);
} CyInterface;

/* THE ONE EXPORTED SYMBOL.
 *
 * Returns a table whose first entries match `requested_major.requested_minor` exactly, or null when
 * the engine cannot satisfy the request — a different major, or a minor the engine predates. The
 * loader reports both version numbers; `cy_get_last_error` carries the same sentence. */
const CyInterface* cy_get_interface(uint32_t requested_major, uint32_t requested_minor);

/* --- Module entry points ------------------------------------------------------------------------
 *
 * Initialisation levels, in order. The same four as `cy::config::ModuleLevel`, because a module
 * registering at `Scene` means the same thing whether it is a C++ module in the build or a Swift
 * module loaded from disk. */
typedef enum CyInitLevel {
    CY_INIT_LEVEL_CORE = 0,    /* before the display server exists */
    CY_INIT_LEVEL_SERVERS = 1, /* after the servers, before the world */
    CY_INIT_LEVEL_SCENE = 2,   /* after the world exists — where types are registered */
    CY_INIT_LEVEL_EDITOR = 3   /* tools builds only */
} CyInitLevel;

/* The stages of one frame, in execution order. `cy::ecs::Stage`'s own values.
 *
 * ADDED AT 1.1 FOR THE REASON `CySeverity` WAS. `CyberdyneKit`'s `SystemStage` was a hand-written
 * copy of this list with nothing to check it against, and its own comment said so: "there is no
 * `CyStage` in `cy_abi.h`, so nothing checks that this list still matches". Now there is, the
 * overlay generates it, and src/abi/src/interface.cpp asserts each enumerator against the engine's.
 *
 * The first four run on the fixed simulation step. That split is `cy::ecs::stage_is_fixed_step`'s
 * and it is a property of the ORDER rather than of a flag, which is why it needs no entry here: a
 * stage is fixed-step exactly when its value is at most CY_STAGE_POST_SIMULATION. */
typedef enum CyStage {
    CY_STAGE_PRE_SIMULATION = 0,
    CY_STAGE_PHYSICS = 1,
    CY_STAGE_SIMULATION = 2,
    CY_STAGE_POST_SIMULATION = 3,
    CY_STAGE_FRAME = 4,
    CY_STAGE_ANIMATION = 5,
    CY_STAGE_UI = 6,
    CY_STAGE_RENDER = 7
} CyStage;

/* What a module hands back from its entry point. `struct_size` is checked by the loader the same
 * way `table_size` is checked by the module, so this struct can grow too. */
typedef struct CyModuleInit {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved; /* explicit padding, so the layout is stated rather than inferred */
    void (*initialize)(CyEngine engine, CyInitLevel level, void* user_data);
    void (*shutdown)(CyEngine engine, CyInitLevel level, void* user_data);
    void* user_data;
} CyModuleInit;

/* The signature every module's entry symbol has. The name is declared in `module.toml`; it is
 * `cy_module_entry` by convention and by default.
 *
 * A module MUST check `iface->header.table_size >= sizeof(the CyInterface it was compiled against)`
 * and return false if it is smaller — that is the "older engine, newer module" case, and returning
 * false is how it is reported without aborting engine startup. */
typedef bool (*CyModuleEntryFn)(const CyInterface* iface, CyEngine engine, CyModuleInit* out_init);

/* Called on the old image immediately before a reload, after every instance it created has been
 * serialized and destroyed. Optional; a module that does not export it is simply not called. */
typedef void (*CyModuleShutdownFn)(void);

/* --- Layout assertions --------------------------------------------------------------------------
 *
 * Asserted here rather than only in the engine, so that a module compiled by a different compiler
 * fails at its own compile rather than at the engine's first call. `native-abi`: "its layout SHALL
 * be fixed-width, explicitly padded, and asserted with static_assert(sizeof(...)) on both sides".
 */
CY_ABI_STATIC_ASSERT(sizeof(CyVarPayload) == 16, "CyVarPayload is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyVar) == 32, "CyVar is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyFieldDesc) == 24, "CyFieldDesc is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyComponentTypeDesc) == 32, "CyComponentTypeDesc is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyBehaviourVTable) == 112, "CyBehaviourVTable is 112 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyBorrow) == 16, "CyBorrow is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyComponentInfo) == 24, "CyComponentInfo is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyChunk) == 40, "CyChunk is 40 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyServiceRequest) == 40, "CyServiceRequest is 40 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyServiceEvent) == 40, "CyServiceEvent is 40 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyInterfaceHeader) == 16, "CyInterfaceHeader is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyModuleInit) == 40, "CyModuleInit is 40 bytes");
/* 1.3 */
CY_ABI_STATIC_ASSERT(sizeof(CyPose) == 28, "CyPose is 28 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyRay) == 28, "CyRay is 28 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyTime) == 48, "CyTime is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyInputActionState) == 32, "CyInputActionState is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyInputPointer) == 48, "CyInputPointer is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyCameraView) == 68, "CyCameraView is 68 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyScreenPoint) == 16, "CyScreenPoint is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyCameraTarget) == 48, "CyCameraTarget is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyShape) == 24, "CyShape is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyQueryFilter) == 32, "CyQueryFilter is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyPhysicsHit) == 48, "CyPhysicsHit is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyNavPathRequest) == 64, "CyNavPathRequest is 64 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyNavPathResult) == 24, "CyNavPathResult is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyNavAgentParams) == 48, "CyNavAgentParams is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyNavAgentState) == 56, "CyNavAgentState is 56 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyAudioPlay) == 56, "CyAudioPlay is 56 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CySpawnParams) == 56, "CySpawnParams is 56 bytes");
/* 1.5 */
CY_ABI_STATIC_ASSERT(sizeof(CySystemAccess) == 8, "CySystemAccess is 8 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CySystemDesc) == 48, "CySystemDesc is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyCharacterDesc) == 76, "CyCharacterDesc is 76 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyCharacterInput) == 24, "CyCharacterInput is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyCharacterState) == 72, "CyCharacterState is 72 bytes");
/* 1.6 */
CY_ABI_STATIC_ASSERT(sizeof(CyUiEvent) == 40, "CyUiEvent is 40 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyUiElementDesc) == 24, "CyUiElementDesc is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyUiLayout) == 144, "CyUiLayout is 144 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyUiStyle) == 32, "CyUiStyle is 32 bytes");
/* 1.7 */
CY_ABI_STATIC_ASSERT(sizeof(CyAnimatorDesc) == 32, "CyAnimatorDesc is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyAnimatorState) == 32, "CyAnimatorState is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyAnimationEvent) == 24, "CyAnimationEvent is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyRootMotion) == 52, "CyRootMotion is 52 bytes");
/* 1.8 */
CY_ABI_STATIC_ASSERT(sizeof(CyFixedVec2) == 16, "CyFixedVec2 is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyFixedVec3) == 24, "CyFixedVec3 is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyFixedQuat) == 32, "CyFixedQuat is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepUnitDesc) == 48, "CyLockstepUnitDesc is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepOrder) == 32, "CyLockstepOrder is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepUnit) == 64, "CyLockstepUnit is 64 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepStatus) == 48, "CyLockstepStatus is 48 bytes");
/* 1.9 */
CY_ABI_STATIC_ASSERT(sizeof(CyGiSettings) == 24, "CyGiSettings is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyGiVolumeDesc) == 72, "CyGiVolumeDesc is 72 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyGiIrradiance) == 32, "CyGiIrradiance is 32 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyGiStatus) == 48, "CyGiStatus is 48 bytes");
/* 1.10 */
CY_ABI_STATIC_ASSERT(sizeof(CyFogVision) == 24, "CyFogVision is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyFogStatus) == 40, "CyFogStatus is 40 bytes");
/* 1.11 */
CY_ABI_STATIC_ASSERT(sizeof(CyLightDesc) == 96, "CyLightDesc is 96 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLightStatus) == 40, "CyLightStatus is 40 bytes");
/* 1.12 */
CY_ABI_STATIC_ASSERT(sizeof(CyUiWidgetDesc) == 96, "CyUiWidgetDesc is 96 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyUiDocumentDesc) == 40, "CyUiDocumentDesc is 40 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyUiAnimation) == 40, "CyUiAnimation is 40 bytes");
/* 1.13 */
CY_ABI_STATIC_ASSERT(sizeof(CyFirstPersonDesc) == 96, "CyFirstPersonDesc is 96 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyFirstPersonInput) == 24, "CyFirstPersonInput is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyFirstPersonState) == 96, "CyFirstPersonState is 96 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyViewmodelDesc) == 32, "CyViewmodelDesc is 32 bytes");
/* 1.14 */
CY_ABI_STATIC_ASSERT(sizeof(CyFootPlacement) == 68, "CyFootPlacement is 68 bytes");
/* 1.15 */
CY_ABI_STATIC_ASSERT(sizeof(CyEffectPlay) == 64, "CyEffectPlay is 64 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyDecalDesc) == 88, "CyDecalDesc is 88 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyDecalStatus) == 32, "CyDecalStatus is 32 bytes");

/* 1.16 */
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepCombatDesc) == 64, "CyLockstepCombatDesc is 64 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepStructureDesc) == 48, "CyLockstepStructureDesc is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepCombatant) == 48, "CyLockstepCombatant is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepStructure) == 48, "CyLockstepStructure is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepEvent) == 80, "CyLockstepEvent is 80 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyLockstepProjectile) == 80, "CyLockstepProjectile is 80 bytes");
/* 1.17 */
CY_ABI_STATIC_ASSERT(sizeof(CyInputRebind) == 116, "CyInputRebind is 116 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyVideoSettings) == 24, "CyVideoSettings is 24 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyVideoMode) == 8, "CyVideoMode is 8 bytes");
/* 1.18 */
CY_ABI_STATIC_ASSERT(sizeof(CyAudioStreamPlay) == 48, "CyAudioStreamPlay is 48 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyAudioStreamState) == 32, "CyAudioStreamState is 32 bytes");

#ifdef __cplusplus
}
#endif

/* NOLINTEND(modernize-macro-to-enum, modernize-redundant-void-arg) */
/* NOLINTEND(modernize-use-using, modernize-deprecated-headers) */
#endif /* CY_ABI_H */
