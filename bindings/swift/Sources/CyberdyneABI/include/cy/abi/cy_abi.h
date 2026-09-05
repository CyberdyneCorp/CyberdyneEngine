/* GENERATED COPY — DO NOT EDIT.
 *
 * `tools/gen/swift/overlay_gen.py` copies src/abi/include/cy/abi/cy_abi.h here so that the Swift
 * package is self-contained: `swift build` in bindings/swift works in a checkout with no configured
 * CMake tree, and SwiftPM will not accept a header search path that leaves the package.
 *
 * Edit the original. `just generate-swift --check` fails when this copy is stale, which is the same
 * gate that catches a stale overlay.
 */
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
 * lets a module built against 1.2 keep running against 1.7 with no recompilation.
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
#define CY_ABI_MINOR 0u
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
    CY_VAR_ENTITY = 11
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
CY_ABI_STATIC_ASSERT(sizeof(CyBehaviourVTable) == 56, "CyBehaviourVTable is 56 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyBorrow) == 16, "CyBorrow is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyInterfaceHeader) == 16, "CyInterfaceHeader is 16 bytes");
CY_ABI_STATIC_ASSERT(sizeof(CyModuleInit) == 40, "CyModuleInit is 40 bytes");

#ifdef __cplusplus
}
#endif

/* NOLINTEND(modernize-macro-to-enum, modernize-redundant-void-arg) */
/* NOLINTEND(modernize-use-using, modernize-deprecated-headers) */
#endif /* CY_ABI_H */
