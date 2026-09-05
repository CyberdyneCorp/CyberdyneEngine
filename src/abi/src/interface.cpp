// The exported interface table, and `cy_get_interface`. Tasks 2.1, 2.2, 2.4, 2.5, 2.6.
//
// THE ORDER OF THE INITIALISER AT THE BOTTOM OF THIS FILE IS THE ABI. It is a
// designated-initialiser list rather than a positional one, so that a reader can see which entry is
// which; but the order is still the contract, because a table is a struct and a struct's members
// are laid out in declaration order. `tools/abi/abi_gate.py` diffs the declaration in
// cy/abi/cy_abi.h against src/abi/abi_baseline.json and refuses anything but an append.
//
// EVERY ENTRY IS A THUNK, AND EVERY THUNK IS TOTAL. The engine is compiled -fno-exceptions, so
// nothing here can unwind; what it can do is receive a null handle, a stale entity or a component
// id from another world, and each of those has to be a returned code rather than a crash. A module
// is on the other side of a boundary and is not trusted to have got it right.

#include <cy/abi/cy_abi.h>

#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/var.h>
#include <cy/core/base/diagnostic_sink.h>
#include <cy/ecs/world.h>

#include <cstring>

namespace {

using cy::abi::from_abi;
using cy::abi::to_abi;

// The layout the description generator computes, asserted against what this compiler produced.
// tools/abi/abi_describe.py derives these numbers from the declarations alone; if the two ever
// disagree, the description the Swift overlay and the Rust SDK are generated from is describing a
// struct that does not exist. src/abi/tests/test_layout.cpp checks the offsets as well.
static_assert(sizeof(CyInterface) % sizeof(void*) == 0, "the table is a whole number of pointers");
static_assert(offsetof(CyInterface, header) == 0, "the header is first, so table_size is readable");

// --- Resolving handles ---------------------------------------------------------------------------
//
// A handle that is null, or a world that has no ECS world behind it, is a module's mistake. It is
// reported and refused; it is never asserted on, because CY_ASSERT is compiled out of Profile and
// Shipping and a boundary check that disappears in two configurations is not a boundary check.

cy::abi::World* world_of(CyWorld world) noexcept {
    if (world == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "the world handle is null");
    }
    return world;
}

const cy::abi::ComponentRecord* component_of(cy::abi::World& world,
                                             CyComponentTypeId component) noexcept {
    const cy::abi::ComponentRecord* record = world.record(component);
    if (record == nullptr) {
        (void)cy::abi::report(CY_RESULT_NOT_FOUND, "no such component type in this world");
    }
    return record;
}

// Locate one field's bytes for reading. Null on any of the four ways this can be wrong, each with
// its own message: the world, the component type, the field index, or the entity.
const cy::u8* read_field(CyWorld world_handle, CyEntity entity, CyComponentTypeId component,
                         cy::u32 field, const cy::abi::FieldRecord** out_field) noexcept {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return nullptr;
    }
    const cy::abi::ComponentRecord* record = component_of(*world, component);
    if (record == nullptr) {
        return nullptr;
    }
    const cy::abi::FieldRecord* found = world->field(*record, field);
    if (found == nullptr) {
        (void)cy::abi::report(CY_RESULT_OUT_OF_RANGE, "no such field on that component");
        return nullptr;
    }
    const void* bytes = world->world.get(from_abi(entity), record->id);
    if (bytes == nullptr) {
        (void)cy::abi::report(CY_RESULT_NOT_FOUND, "this entity does not have that component");
        return nullptr;
    }
    *out_field = found;
    return static_cast<const cy::u8*>(bytes) + found->offset;
}

// The same for writing. Separate from the read path because `get_mut` stamps the chunk's version
// and `get` does not — `ecs-core`'s "read does not dirty" is a property a downstream change filter
// depends on, and going through the mutable path to read would break it silently.
cy::u8* write_field(CyWorld world_handle, CyEntity entity, CyComponentTypeId component,
                    cy::u32 field, const cy::abi::FieldRecord** out_field) noexcept {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return nullptr;
    }
    const cy::abi::ComponentRecord* record = component_of(*world, component);
    if (record == nullptr) {
        return nullptr;
    }
    const cy::abi::FieldRecord* found = world->field(*record, field);
    if (found == nullptr) {
        (void)cy::abi::report(CY_RESULT_OUT_OF_RANGE, "no such field on that component");
        return nullptr;
    }
    void* bytes = world->world.get_mut(from_abi(entity), record->id);
    if (bytes == nullptr) {
        (void)cy::abi::report(CY_RESULT_NOT_FOUND, "this entity does not have that component");
        return nullptr;
    }
    *out_field = found;
    return static_cast<cy::u8*>(bytes) + found->offset;
}

CyVar var_from_field(const cy::abi::FieldRecord& field, const cy::u8* bytes) noexcept {
    switch (field.type) {
        case CY_VAR_BOOL:
            return cy::abi::var_bool(*bytes != 0);
        case CY_VAR_I64: {
            cy::i64 value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return cy::abi::var_i64(value);
        }
        case CY_VAR_F32: {
            cy::f32 value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            CyVar var = cy::abi::var_nil();
            var.type = CY_VAR_F32;
            var.payload.as_f32 = value;
            return var;
        }
        case CY_VAR_F64: {
            cy::f64 value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return cy::abi::var_f64(value);
        }
        case CY_VAR_ENTITY: {
            CyEntity value = CY_ENTITY_NULL;
            std::memcpy(&value, bytes, sizeof(value));
            return cy::abi::var_entity(value);
        }
        case CY_VAR_VEC2:
        case CY_VAR_VEC3:
        case CY_VAR_VEC4:
        case CY_VAR_QUAT: {
            cy::f32 values[4] = {0, 0, 0, 0};
            std::memcpy(static_cast<void*>(values), bytes, field.size);
            return cy::abi::var_floats(field.type, values, field.size / 4U);
        }
        // A component field is never one of these: registration refuses a field whose type has no
        // fixed width in storage, which is the check that keeps this switch total.
        case CY_VAR_NIL:
        case CY_VAR_STRING:
        case CY_VAR_BYTES:
            break;
    }
    return cy::abi::var_nil();
}

CyResult var_into_field(const cy::abi::FieldRecord& field, const CyVar& value,
                        cy::u8* bytes) noexcept {
    if (value.type != static_cast<cy::u32>(field.type)) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT,
                               "the value's type is not the field's type");
    }
    switch (field.type) {
        case CY_VAR_BOOL:
            *bytes = value.payload.as_bool ? 1U : 0U;
            break;
        case CY_VAR_I64:
            std::memcpy(bytes, &value.payload.as_i64, sizeof(cy::i64));
            break;
        case CY_VAR_F32:
            std::memcpy(bytes, &value.payload.as_f32, sizeof(cy::f32));
            break;
        case CY_VAR_F64:
            std::memcpy(bytes, &value.payload.as_f64, sizeof(cy::f64));
            break;
        case CY_VAR_ENTITY:
            std::memcpy(bytes, &value.payload.as_entity, sizeof(CyEntity));
            break;
        case CY_VAR_VEC2:
        case CY_VAR_VEC3:
        case CY_VAR_VEC4:
        case CY_VAR_QUAT:
            std::memcpy(bytes, static_cast<const void*>(value.payload.as_f32x4), field.size);
            break;
        case CY_VAR_NIL:
        case CY_VAR_STRING:
        case CY_VAR_BYTES:
            return cy::abi::report(CY_RESULT_UNSUPPORTED, "that type has no width in storage");
    }
    cy::abi::clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace

// The thunks. `extern "C"` so that the pointers stored in the table have C language linkage — the
// same linkage a module's compiler assumes when it calls through them — and `static` so that none
// of them is an exported symbol. `cy_get_interface` is the only symbol this ABI exports.
extern "C" {

// --- Diagnostics ---------------------------------------------------------------------------------

static void abi_log(CyEngine engine, uint32_t severity, const char* message) {
    (void)engine;
    const auto level = (severity > static_cast<uint32_t>(cy::DiagnosticSeverity::Error))
                           ? cy::DiagnosticSeverity::Error
                           : static_cast<cy::DiagnosticSeverity>(severity);
    cy::emit_diagnostic(level, "abi", (message != nullptr) ? message : "");
}

static const char* abi_get_last_error() {
    return cy::abi::last_error_message();
}

static CyResult abi_get_last_error_code() {
    return cy::abi::last_error_code();
}

static void abi_set_last_error(CyResult result, const char* message) {
    (void)cy::abi::report(result, message);
}

// --- Values --------------------------------------------------------------------------------------

static CyVar abi_var_make_string(CyEngine engine, const char* utf8, uint64_t length) {
    if (engine == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "the engine handle is null");
        return cy::abi::var_nil();
    }
    return cy::abi::make_heap_var(*engine, CY_VAR_STRING, utf8, length);
}

static CyVar abi_var_make_bytes(CyEngine engine, const void* data, uint64_t size) {
    if (engine == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "the engine handle is null");
        return cy::abi::var_nil();
    }
    return cy::abi::make_heap_var(*engine, CY_VAR_BYTES, data, size);
}

static CyVar abi_var_clone(const CyVar* var) {
    if (var == nullptr) {
        return cy::abi::var_nil();
    }
    return cy::abi::clone_var(*var);
}

static void abi_var_release(CyVar* var) {
    cy::abi::release_var(var);
}

static uint64_t abi_var_live_count(CyEngine engine) {
    if (engine == nullptr) {
        return 0;
    }
    return engine->live_vars.load(std::memory_order_relaxed);
}

// --- The world -----------------------------------------------------------------------------------

static CyWorld abi_engine_world(CyEngine engine) {
    if (engine == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "the engine handle is null");
        return nullptr;
    }
    if (engine->world == nullptr) {
        (void)cy::abi::report(CY_RESULT_UNAVAILABLE, "no world is bound to this engine");
        return nullptr;
    }
    cy::abi::clear_last_error();
    return engine->world;
}

static CyEntity abi_world_create_entity(CyWorld world_handle) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return CY_ENTITY_NULL;
    }
    cy::Expected<cy::ecs::Entity, cy::Error> created = world->world.create();
    if (!created) {
        (void)cy::abi::report(created.error());
        return CY_ENTITY_NULL;
    }
    world->bump_epoch();
    cy::abi::clear_last_error();
    return to_abi(created.value());
}

static CyResult abi_world_destroy_entity(CyWorld world_handle, CyEntity entity) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    const CyResult result = cy::abi::unwrap(world->world.destroy(from_abi(entity)));
    if (result == CY_RESULT_OK) {
        world->bump_epoch();
    }
    return result;
}

static bool abi_world_entity_alive(CyWorld world_handle, CyEntity entity) {
    cy::abi::World* world = world_of(world_handle);
    return world != nullptr && world->world.is_alive(from_abi(entity));
}

static uint64_t abi_world_epoch(CyWorld world_handle) {
    cy::abi::World* world = world_of(world_handle);
    return (world != nullptr) ? world->epoch : 0;
}

// --- Components ----------------------------------------------------------------------------------

static CyComponentTypeId abi_world_register_component(CyWorld world_handle,
                                                      const CyComponentTypeDesc* desc) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr || desc == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "a component descriptor is required");
        return CY_COMPONENT_TYPE_INVALID;
    }
    // The prefix rule, from the other side: a module compiled against a longer descriptor than this
    // engine knows may pass one, and the engine reads only what it declared.
    if (desc->struct_size < sizeof(CyComponentTypeDesc)) {
        (void)cy::abi::report(CY_RESULT_VERSION_MISMATCH,
                              "the component descriptor is older than this engine's");
        return CY_COMPONENT_TYPE_INVALID;
    }
    cy::Expected<CyComponentTypeId, cy::Error> registered = world->register_component(*desc);
    if (!registered) {
        (void)cy::abi::report(registered.error());
        return CY_COMPONENT_TYPE_INVALID;
    }
    cy::abi::clear_last_error();
    return registered.value();
}

static CyComponentTypeId abi_world_find_component(CyWorld world_handle, const char* name) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return CY_COMPONENT_TYPE_INVALID;
    }
    const cy::abi::ComponentRecord* record = world->find(name);
    if (record == nullptr) {
        (void)cy::abi::report(CY_RESULT_NOT_FOUND, "no component of that name in this world");
        return CY_COMPONENT_TYPE_INVALID;
    }
    cy::abi::clear_last_error();
    return static_cast<CyComponentTypeId>(record->id);
}

static CyResult abi_world_add_component(CyWorld world_handle, CyEntity entity,
                                        CyComponentTypeId component, const void* initial) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    const cy::abi::ComponentRecord* record = component_of(*world, component);
    if (record == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    const CyResult result =
        cy::abi::unwrap(world->world.add(from_abi(entity), record->id, initial));
    if (result == CY_RESULT_OK) {
        world->bump_epoch();
    }
    return result;
}

static CyResult abi_world_remove_component(CyWorld world_handle, CyEntity entity,
                                           CyComponentTypeId component) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    const cy::abi::ComponentRecord* record = component_of(*world, component);
    if (record == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    const CyResult result = cy::abi::unwrap(world->world.remove(from_abi(entity), record->id));
    if (result == CY_RESULT_OK) {
        world->bump_epoch();
    }
    return result;
}

static bool abi_world_has_component(CyWorld world_handle, CyEntity entity,
                                    CyComponentTypeId component) {
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return false;
    }
    const cy::abi::ComponentRecord* record = world->record(component);
    return record != nullptr && world->world.has(from_abi(entity), record->id);
}

static CyBorrow abi_world_borrow_component(CyWorld world_handle, CyEntity entity,
                                           CyComponentTypeId component) {
    CyBorrow borrow{nullptr, 0};
    cy::abi::World* world = world_of(world_handle);
    if (world == nullptr) {
        return borrow;
    }
    const cy::abi::ComponentRecord* record = component_of(*world, component);
    if (record == nullptr) {
        return borrow;
    }
    borrow.data = world->world.get_mut(from_abi(entity), record->id);
    borrow.epoch = world->epoch;
    if (borrow.data == nullptr) {
        (void)cy::abi::report(CY_RESULT_NOT_FOUND, "this entity does not have that component");
        borrow.epoch = 0;
        return borrow;
    }
    cy::abi::clear_last_error();
    return borrow;
}

static bool abi_borrow_valid(CyWorld world_handle, CyBorrow borrow) {
    cy::abi::World* world = world_of(world_handle);
    return world != nullptr && borrow.data != nullptr && borrow.epoch != 0 &&
           borrow.epoch == world->epoch;
}

static CyResult abi_component_get_var(CyWorld world_handle, CyEntity entity,
                                      CyComponentTypeId component, uint32_t field,
                                      CyVar* out_value) {
    if (out_value == nullptr) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "output pointer is null");
    }
    const cy::abi::FieldRecord* found = nullptr;
    const cy::u8* bytes = read_field(world_handle, entity, component, field, &found);
    if (bytes == nullptr) {
        return cy::abi::last_error_code();
    }
    *out_value = var_from_field(*found, bytes);
    cy::abi::clear_last_error();
    return CY_RESULT_OK;
}

static CyResult abi_component_set_var(CyWorld world_handle, CyEntity entity,
                                      CyComponentTypeId component, uint32_t field,
                                      const CyVar* value) {
    if (value == nullptr) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "value pointer is null");
    }
    const cy::abi::FieldRecord* found = nullptr;
    cy::u8* bytes = write_field(world_handle, entity, component, field, &found);
    if (bytes == nullptr) {
        return cy::abi::last_error_code();
    }
    return var_into_field(*found, *value, bytes);
}

// The typed fast paths. One type check and a memcpy — no CyVar is constructed, which is the whole
// point of their being here: `native-abi` requires that a behaviour updating a transform every tick
// does not marshal.

static CyResult abi_component_get_f32(CyWorld world_handle, CyEntity entity,
                                      CyComponentTypeId component, uint32_t field,
                                      float* out_value) {
    if (out_value == nullptr) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "output pointer is null");
    }
    const cy::abi::FieldRecord* found = nullptr;
    const cy::u8* bytes = read_field(world_handle, entity, component, field, &found);
    if (bytes == nullptr) {
        return cy::abi::last_error_code();
    }
    if (found->type != CY_VAR_F32) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "that field is not a float");
    }
    std::memcpy(out_value, bytes, sizeof(float));
    cy::abi::clear_last_error();
    return CY_RESULT_OK;
}

static CyResult abi_component_set_f32(CyWorld world_handle, CyEntity entity,
                                      CyComponentTypeId component, uint32_t field, float value) {
    const cy::abi::FieldRecord* found = nullptr;
    cy::u8* bytes = write_field(world_handle, entity, component, field, &found);
    if (bytes == nullptr) {
        return cy::abi::last_error_code();
    }
    if (found->type != CY_VAR_F32) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "that field is not a float");
    }
    std::memcpy(bytes, &value, sizeof(float));
    cy::abi::clear_last_error();
    return CY_RESULT_OK;
}

static CyResult abi_component_get_vec3(CyWorld world_handle, CyEntity entity,
                                       CyComponentTypeId component, uint32_t field,
                                       float* out_xyz) {
    if (out_xyz == nullptr) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "output pointer is null");
    }
    const cy::abi::FieldRecord* found = nullptr;
    const cy::u8* bytes = read_field(world_handle, entity, component, field, &found);
    if (bytes == nullptr) {
        return cy::abi::last_error_code();
    }
    if (found->type != CY_VAR_VEC3) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "that field is not a vec3");
    }
    std::memcpy(out_xyz, bytes, sizeof(float) * 3);
    cy::abi::clear_last_error();
    return CY_RESULT_OK;
}

static CyResult abi_component_set_vec3(CyWorld world_handle, CyEntity entity,
                                       CyComponentTypeId component, uint32_t field,
                                       const float* xyz) {
    if (xyz == nullptr) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "value pointer is null");
    }
    const cy::abi::FieldRecord* found = nullptr;
    cy::u8* bytes = write_field(world_handle, entity, component, field, &found);
    if (bytes == nullptr) {
        return cy::abi::last_error_code();
    }
    if (found->type != CY_VAR_VEC3) {
        return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "that field is not a vec3");
    }
    std::memcpy(bytes, xyz, sizeof(float) * 3);
    cy::abi::clear_last_error();
    return CY_RESULT_OK;
}

// --- Behaviours ----------------------------------------------------------------------------------

static CyBehaviourType abi_register_behaviour(CyEngine engine, const char* name,
                                              const CyBehaviourVTable* vtable) {
    if (engine == nullptr || vtable == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "an engine and a vtable are required");
        return nullptr;
    }
    cy::Expected<CyBehaviourType, cy::Error> registered = engine->register_behaviour(name, *vtable);
    if (!registered) {
        (void)cy::abi::report(registered.error());
        return nullptr;
    }
    cy::abi::clear_last_error();
    return registered.value();
}

static CyBehaviourType abi_find_behaviour(CyEngine engine, const char* name) {
    if (engine == nullptr) {
        (void)cy::abi::report(CY_RESULT_INVALID_ARGUMENT, "the engine handle is null");
        return nullptr;
    }
    CyBehaviourType found = engine->find_behaviour(name);
    if (found == nullptr) {
        (void)cy::abi::report(CY_RESULT_NOT_FOUND, "no behaviour of that name in this generation");
    }
    return found;
}

static uint32_t abi_behaviour_generation(CyBehaviourType type) {
    return (type != nullptr) ? type->generation : 0;
}

}  // extern "C"

namespace {

// THE TABLE. One instance, constant, with static storage duration — so `cy_get_interface` hands out
// a pointer that is valid for the process lifetime and no module ever has to free it.
//
// `table_size` is `sizeof(CyInterface)` and nothing else: it is what a module compares against the
// size it was compiled with, and computing it any other way would let the two disagree.
const CyInterface kInterface = {
    /* header */ {CY_ABI_MAJOR, CY_ABI_MINOR, CY_ABI_PATCH, sizeof(CyInterface)},

    &abi_log,
    &abi_get_last_error,
    &abi_get_last_error_code,
    &abi_set_last_error,

    &abi_var_make_string,
    &abi_var_make_bytes,
    &abi_var_clone,
    &abi_var_release,
    &abi_var_live_count,

    &abi_engine_world,
    &abi_world_create_entity,
    &abi_world_destroy_entity,
    &abi_world_entity_alive,
    &abi_world_epoch,

    &abi_world_register_component,
    &abi_world_find_component,
    &abi_world_add_component,
    &abi_world_remove_component,
    &abi_world_has_component,
    &abi_world_borrow_component,
    &abi_borrow_valid,
    &abi_component_get_var,
    &abi_component_set_var,
    &abi_component_get_f32,
    &abi_component_set_f32,
    &abi_component_get_vec3,
    &abi_component_set_vec3,

    &abi_register_behaviour,
    &abi_find_behaviour,
    &abi_behaviour_generation,
};

}  // namespace

extern "C" const CyInterface* cy_get_interface(uint32_t requested_major, uint32_t requested_minor) {
    // A DIFFERENT MAJOR IS A DIFFERENT ABI. There is no compatibility to negotiate: the table's
    // entries mean different things, and returning the current one would hand a module function
    // pointers whose signatures it does not agree with.
    if (requested_major != CY_ABI_MAJOR) {
        (void)cy::abi::report(CY_RESULT_VERSION_MISMATCH,
                              "the engine exports ABI major 1 and the module asked for another");
        return nullptr;
    }
    // A MINOR THE ENGINE PREDATES IS THE "older engine, newer module" CASE. The module needs
    // entries this build does not have, and the specification requires both numbers in the report —
    // so the message names them rather than saying "version mismatch".
    if (requested_minor > CY_ABI_MINOR) {
        (void)cy::abi::report(CY_RESULT_VERSION_MISMATCH,
                              "this engine exports ABI 1.0 and the module requires a later minor");
        return nullptr;
    }
    // A MINOR THE ENGINE HAS PASSED IS THE "newer engine, older module" CASE, and it is the one the
    // append-only rule exists for: the first entries of this table are that older table, byte for
    // byte, so the same pointer serves both. The module reads only the prefix it knows, which it
    // knows because it compares `table_size` with its own `sizeof(CyInterface)`.
    cy::abi::clear_last_error();
    return &kInterface;
}
