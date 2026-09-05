// The interface table: discovery, versioning, and the entries a module actually calls.
// Tasks 2.1, 2.2, 2.4, 2.5.
//
// Every case here goes THROUGH the table rather than calling the engine directly, because that is
// what a module can do and nothing else is. A test that called `cy::abi::World::register_component`
// would be testing the implementation; these test the boundary.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/var.h>
#include <cy/core/base/diagnostic_sink.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

// The component a module would register: three floats and a scalar, laid out the way a module's own
// struct is, and described field by field so the generic and typed paths have something to reach.
struct Probe {
    float position[3] = {0.0F, 0.0F, 0.0F};
    float speed = 0.0F;
    cy::i64 ticks = 0;
};

const CyFieldDesc kProbeFields[] = {
    {sizeof(CyFieldDesc), CY_VAR_VEC3, 0, 12, "position"},
    {sizeof(CyFieldDesc), CY_VAR_F32, 12, 4, "speed"},
    {sizeof(CyFieldDesc), CY_VAR_I64, 16, 8, "ticks"},
};

CyComponentTypeDesc probe_desc(const char* name = "Probe") noexcept {
    CyComponentTypeDesc desc{};
    desc.struct_size = sizeof(CyComponentTypeDesc);
    desc.size = sizeof(Probe);
    desc.alignment = alignof(Probe);
    desc.field_count = 3;
    desc.name = name;
    desc.fields = kProbeFields;
    return desc;
}

// A world, its ABI binding and a host, brought up together. Not a shared fixture object because
// each case wants its own world: a component registry is per world, and a case that inherited
// another's registrations would pass for the wrong reason.
struct Bound {
    cy::ecs::World world{allocator()};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};

    Bound() {
        CY_REQUIRE(world.initialize().has_value());
        host.bind_world(&binding);
    }
};

// A behaviour vtable a module would register. The entries are C functions with C linkage, because
// that is what crosses the boundary; they count their own calls so a test can see that the engine
// reached the module's code and not something that merely looked like it.
extern "C" {

int g_created = 0;
int g_destroyed = 0;

CyInstance probe_create(CyEngine engine, CyEntity entity, void* user_data) {
    (void)engine;
    (void)entity;
    *static_cast<int*>(user_data) += 1;
    ++g_created;
    return user_data;  // any non-null value; the engine never dereferences it
}

void probe_destroy(CyInstance self, void* user_data) {
    (void)self;
    (void)user_data;
    ++g_destroyed;
}

}  // extern "C"

CyBehaviourVTable probe_vtable(void* user_data) noexcept {
    CyBehaviourVTable vtable{};
    vtable.struct_size = sizeof(CyBehaviourVTable);
    vtable.schema_version = 3;
    vtable.create = &probe_create;
    vtable.destroy = &probe_destroy;
    vtable.user_data = user_data;
    return vtable;
}

}  // namespace

CY_TEST_CASE("cy_get_interface is the only symbol a module needs") {
    const CyInterface& iface = table();
    CY_CHECK_EQ(iface.header.abi_major, CY_ABI_MAJOR);
    CY_CHECK_EQ(iface.header.abi_minor, CY_ABI_MINOR);
    // `table_size` is what a module compares with its own sizeof. It must be the real size and not
    // a number somebody kept up to date by hand.
    CY_CHECK_EQ(iface.header.table_size, sizeof(CyInterface));
    CY_CHECK(iface.log != nullptr);
    CY_CHECK(iface.behaviour_generation != nullptr);
}

CY_TEST_CASE("a newer engine serves an older module the same pointer") {
    // `native-abi`'s "Newer engine, older module": a module built against 1.0 asks for 1.0, and the
    // first entries of whatever this engine exports are that table exactly. Requesting the current
    // minor and requesting zero must therefore give the same table.
    const CyInterface* current = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    const CyInterface* older = cy_get_interface(CY_ABI_MAJOR, 0);
    CY_CHECK(current != nullptr);
    CY_CHECK_EQ(current, older);
}

CY_TEST_CASE("an older engine refuses a newer module, naming both versions") {
    // `native-abi`'s "Older engine, newer module": null, and the loader can report both numbers.
    CY_CHECK(cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR + 1) == nullptr);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_VERSION_MISMATCH);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "1.0") != nullptr);

    // A different major is a different ABI and there is nothing to negotiate.
    CY_CHECK(cy_get_interface(CY_ABI_MAJOR + 1, 0) == nullptr);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_VERSION_MISMATCH);
}

CY_TEST_CASE("a failure is a returned code and an untouched output") {
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = iface.engine_world(&bound.host);
    CY_REQUIRE(world != nullptr);

    const CyEntity entity = iface.world_create_entity(world);
    CY_REQUIRE(entity != CY_ENTITY_NULL);

    // `native-abi`'s "Failure is reported by return value": NOT_FOUND, and the output is left
    // alone.
    CyVar out = cy::abi::var_i64(4242);
    const CyResult result = iface.component_get_var(world, entity, 0, 0, &out);
    CY_CHECK_EQ(result, CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(out.type, static_cast<cy::u32>(CY_VAR_I64));
    CY_CHECK_EQ(out.payload.as_i64, 4242);
    CY_CHECK(std::strlen(iface.get_last_error()) > 0);
}

CY_TEST_CASE("a module registers a component and the world holds it") {
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = iface.engine_world(&bound.host);

    const CyComponentTypeDesc desc = probe_desc();
    const CyComponentTypeId probe = iface.world_register_component(world, &desc);
    CY_REQUIRE(probe != CY_COMPONENT_TYPE_INVALID);
    CY_CHECK_EQ(iface.world_find_component(world, "Probe"), probe);

    // Registering the same shape again is the reload case, and it must be idempotent rather than an
    // error: step (f) of a reload has the module register its types into the new generation.
    CY_CHECK_EQ(iface.world_register_component(world, &desc), probe);

    // A different type wearing a used name is refused, which is the half that makes the idempotence
    // safe rather than merely convenient.
    CyComponentTypeDesc conflicting = probe_desc();
    conflicting.size = sizeof(Probe) + 8;
    CY_CHECK_EQ(iface.world_register_component(world, &conflicting), CY_COMPONENT_TYPE_INVALID);
    CY_CHECK_EQ(iface.get_last_error_code(), CY_RESULT_ALREADY_EXISTS);
}

CY_TEST_CASE("a field crosses as a CyVar and comes back the same") {
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = iface.engine_world(&bound.host);
    const CyComponentTypeDesc desc = probe_desc();
    const CyComponentTypeId probe = iface.world_register_component(world, &desc);

    const CyEntity entity = iface.world_create_entity(world);
    CY_REQUIRE_EQ(iface.world_add_component(world, entity, probe, nullptr), CY_RESULT_OK);
    CY_CHECK(iface.world_has_component(world, entity, probe));

    CyVar ticks = cy::abi::var_i64(7);
    CY_REQUIRE_EQ(iface.component_set_var(world, entity, probe, 2, &ticks), CY_RESULT_OK);
    CyVar read = cy::abi::var_nil();
    CY_REQUIRE_EQ(iface.component_get_var(world, entity, probe, 2, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.type, static_cast<cy::u32>(CY_VAR_I64));
    CY_CHECK_EQ(read.payload.as_i64, 7);

    // The type check is what stops a module writing a float over an integer field.
    CyVar wrong = cy::abi::var_f64(1.5);
    CY_CHECK_EQ(iface.component_set_var(world, entity, probe, 2, &wrong),
                CY_RESULT_INVALID_ARGUMENT);
    // And an out-of-range field index is OUT_OF_RANGE rather than a read past the component.
    CY_CHECK_EQ(iface.component_get_var(world, entity, probe, 99, &read), CY_RESULT_OUT_OF_RANGE);
}

CY_TEST_CASE("the typed fast paths read and write the same bytes as the generic one") {
    // `native-abi`'s "Hot path avoids CyVar". The point of the typed entries is that they are the
    // same storage seen without marshalling — if they were not, a behaviour updating a transform
    // every tick and a tool inspecting it would disagree.
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = iface.engine_world(&bound.host);
    const CyComponentTypeDesc desc = probe_desc();
    const CyComponentTypeId probe = iface.world_register_component(world, &desc);
    const CyEntity entity = iface.world_create_entity(world);
    CY_REQUIRE_EQ(iface.world_add_component(world, entity, probe, nullptr), CY_RESULT_OK);

    const float position[3] = {1.0F, 2.0F, 3.0F};
    CY_REQUIRE_EQ(iface.component_set_vec3(world, entity, probe, 0, position), CY_RESULT_OK);
    CY_REQUIRE_EQ(iface.component_set_f32(world, entity, probe, 1, 9.5F), CY_RESULT_OK);

    float read[3] = {0.0F, 0.0F, 0.0F};
    CY_REQUIRE_EQ(iface.component_get_vec3(world, entity, probe, 0, read), CY_RESULT_OK);
    CY_CHECK_EQ(read[0], 1.0F);
    CY_CHECK_EQ(read[2], 3.0F);

    float speed = 0.0F;
    CY_REQUIRE_EQ(iface.component_get_f32(world, entity, probe, 1, &speed), CY_RESULT_OK);
    CY_CHECK_EQ(speed, 9.5F);

    // The generic path sees what the typed path wrote.
    CyVar as_var = cy::abi::var_nil();
    CY_REQUIRE_EQ(iface.component_get_var(world, entity, probe, 0, &as_var), CY_RESULT_OK);
    CY_CHECK_EQ(as_var.type, static_cast<cy::u32>(CY_VAR_VEC3));
    CY_CHECK_EQ(as_var.payload.as_f32x4[1], 2.0F);

    // A typed entry aimed at the wrong field is refused rather than reinterpreting the bytes.
    CY_CHECK_EQ(iface.component_get_f32(world, entity, probe, 0, &speed),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a borrowed pointer is detectably stale after a structural change") {
    // `native-abi`'s "Borrowed pointer is scoped". The requirement is that the pointer's validity
    // ends at the next structural change and that use past it is detected — so the check is that
    // the borrow REPORTS itself invalid, not that dereferencing it happens to crash.
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = iface.engine_world(&bound.host);
    const CyComponentTypeDesc desc = probe_desc();
    const CyComponentTypeId probe = iface.world_register_component(world, &desc);
    const CyEntity entity = iface.world_create_entity(world);
    CY_REQUIRE_EQ(iface.world_add_component(world, entity, probe, nullptr), CY_RESULT_OK);

    const CyBorrow borrow = iface.world_borrow_component(world, entity, probe);
    CY_REQUIRE(borrow.data != nullptr);
    CY_CHECK(iface.borrow_valid(world, borrow));

    // Any structural change, on any entity, ends it: chunk storage moves for reasons that have
    // nothing to do with the entity whose component was borrowed.
    const CyEntity other = iface.world_create_entity(world);
    CY_REQUIRE(other != CY_ENTITY_NULL);
    CY_CHECK_FALSE(iface.borrow_valid(world, borrow));

    // A zeroed borrow is never valid, so a module that forgot to take one does not get a pass.
    CY_CHECK_FALSE(iface.borrow_valid(world, CyBorrow{nullptr, 0}));
}

CY_TEST_CASE("an owned value is counted until it is released") {
    // `native-abi`'s "Ownership is documented and checked": development builds detect leaks of
    // returned values. The counter is what a leak check reads, and it is live in every profile.
    Bound bound;
    const CyInterface& iface = table();
    CY_CHECK_EQ(iface.var_live_count(&bound.host), 0U);

    CyVar text = iface.var_make_string(&bound.host, "hello", 5);
    CY_REQUIRE_EQ(text.type, static_cast<cy::u32>(CY_VAR_STRING));
    CY_CHECK((text.flags & CY_VAR_FLAG_OWNED) != 0U);
    CY_CHECK_EQ(text.length, 5U);
    CY_CHECK_EQ(iface.var_live_count(&bound.host), 1U);
    CY_CHECK_EQ(std::memcmp(text.payload.as_bytes, "hello", 5), 0);

    // A clone is a second reference to the same payload, not a second allocation.
    CyVar second = iface.var_clone(&text);
    CY_CHECK_EQ(second.payload.as_bytes, text.payload.as_bytes);
    CY_CHECK_EQ(iface.var_live_count(&bound.host), 1U);

    iface.var_release(&text);
    CY_CHECK_EQ(text.type, static_cast<cy::u32>(CY_VAR_NIL));
    CY_CHECK_EQ(iface.var_live_count(&bound.host), 1U);
    iface.var_release(&second);
    CY_CHECK_EQ(iface.var_live_count(&bound.host), 0U);

    // Releasing twice, and releasing something that never owned anything, are both no-ops. That is
    // what lets a generated overlay release unconditionally in a `deinit`.
    iface.var_release(&second);
    CyVar inline_value = cy::abi::var_i64(3);
    iface.var_release(&inline_value);
    CY_CHECK_EQ(iface.var_live_count(&bound.host), 0U);
}

CY_TEST_CASE("a null handle is refused rather than dereferenced") {
    // Everything here is a module's mistake arriving from the other side of a boundary. None of it
    // may assert, because CY_ASSERT is compiled out of Profile and Shipping.
    const CyInterface& iface = table();
    CY_CHECK(iface.engine_world(nullptr) == nullptr);
    CY_CHECK_EQ(iface.world_create_entity(nullptr), CY_ENTITY_NULL);
    CY_CHECK_EQ(iface.world_destroy_entity(nullptr, 1), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_FALSE(iface.world_entity_alive(nullptr, 1));
    CY_CHECK_EQ(iface.world_epoch(nullptr), 0U);
    CY_CHECK_EQ(iface.world_register_component(nullptr, nullptr), CY_COMPONENT_TYPE_INVALID);
    CY_CHECK_EQ(iface.var_live_count(nullptr), 0U);
    CY_CHECK(iface.register_behaviour(nullptr, "X", nullptr) == nullptr);
    CY_CHECK_EQ(iface.behaviour_generation(nullptr), 0U);
    CY_CHECK_EQ(iface.var_clone(nullptr).type, static_cast<cy::u32>(CY_VAR_NIL));
    iface.var_release(nullptr);
}

CY_TEST_CASE("the error mapping is a conversion in both directions") {
    // The cast is asserted at compile time in errors.cpp; this is the round trip a caller sees,
    // including the ABI-only codes that have no engine equivalent.
    CY_CHECK_EQ(cy::abi::to_result(cy::ErrorCode::NotFound), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(cy::abi::to_result(cy::ErrorCode::None), CY_RESULT_OK);
    CY_CHECK_EQ(cy::abi::to_error_code(CY_RESULT_OUT_OF_MEMORY), cy::ErrorCode::OutOfMemory);
    CY_CHECK_EQ(cy::abi::to_error_code(CY_RESULT_SCHEMA_TOO_NEW), cy::ErrorCode::Unsupported);
    CY_CHECK(std::strcmp(cy_result_name(CY_RESULT_SCHEMA_TOO_NEW), "CY_RESULT_SCHEMA_TOO_NEW") ==
             0);

    // A module reporting its own failure reads back through the same thread-local record the engine
    // writes, which is what makes one `cy_get_last_error` enough for both sides.
    const CyInterface& iface = table();
    iface.set_last_error(CY_RESULT_TIMEOUT, "the module gave up waiting");
    CY_CHECK_EQ(iface.get_last_error_code(), CY_RESULT_TIMEOUT);
    CY_CHECK(std::strcmp(iface.get_last_error(), "the module gave up waiting") == 0);

    // A message longer than the buffer is truncated rather than dropped or allocated for.
    char long_message[cy::abi::kLastErrorCapacity + 64];
    std::memset(long_message, 'x', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    iface.set_last_error(CY_RESULT_IO, long_message);
    CY_CHECK_EQ(std::strlen(iface.get_last_error()), cy::abi::kLastErrorCapacity - 1);
}

CY_TEST_CASE("a module registers a behaviour type and the engine calls back into it") {
    // `native-abi`'s "Callbacks into modules": the engine invokes module code ONLY through function
    // pointers the module registered, each carrying the module's own `user_data`. So the test
    // checks that the engine reached this vtable and handed back this `user_data`, rather than that
    // a call happened at all.
    Bound bound;
    const CyInterface& iface = table();

    int calls = 0;
    const CyBehaviourVTable vtable = probe_vtable(&calls);
    CyBehaviourType type = iface.register_behaviour(&bound.host, "Probe", &vtable);
    CY_REQUIRE(type != nullptr);
    CY_CHECK_EQ(iface.find_behaviour(&bound.host, "Probe"), type);
    CY_CHECK_EQ(iface.behaviour_generation(type), 0U);

    const int created_before = g_created;
    CyInstance instance = type->vtable.create(&bound.host, 7, type->vtable.user_data);
    CY_CHECK_EQ(g_created, created_before + 1);
    CY_CHECK_EQ(calls, 1);
    CY_CHECK_EQ(instance, static_cast<CyInstance>(&calls));

    const int destroyed_before = g_destroyed;
    type->vtable.destroy(instance, type->vtable.user_data);
    CY_CHECK_EQ(g_destroyed, destroyed_before + 1);

    // A name nothing registered is NOT_FOUND rather than a null nobody explains.
    CY_CHECK(iface.find_behaviour(&bound.host, "Absent") == nullptr);
    CY_CHECK_EQ(iface.get_last_error_code(), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("only the prefix both sides agree on is copied out of a vtable") {
    // The append-only rule from the module's side. A module compiled against a LONGER vtable than
    // this engine knows may pass one; the engine must copy `min(theirs, mine)` and ignore the rest,
    // rather than read a field it has no name for. Measured in the spike, in both directions.
    Bound bound;
    const CyInterface& iface = table();
    int calls = 0;

    CyBehaviourVTable longer = probe_vtable(&calls);
    longer.struct_size = sizeof(CyBehaviourVTable) + 64;  // a newer module's, with entries appended
    CyBehaviourType from_newer = iface.register_behaviour(&bound.host, "Newer", &longer);
    CY_REQUIRE(from_newer != nullptr);
    CY_CHECK_EQ(from_newer->vtable.struct_size, sizeof(CyBehaviourVTable));
    CY_CHECK_EQ(from_newer->vtable.schema_version, 3U);

    // And the other direction: an older module's shorter vtable leaves the entries it never had
    // null, rather than filled with whatever was next in the caller's memory.
    CyBehaviourVTable shorter = probe_vtable(&calls);
    shorter.struct_size = offsetof(CyBehaviourVTable, fixed_update);
    CyBehaviourType from_older = iface.register_behaviour(&bound.host, "Older", &shorter);
    CY_REQUIRE(from_older != nullptr);
    CY_CHECK(from_older->vtable.create != nullptr);
    CY_CHECK(from_older->vtable.destroy != nullptr);
    CY_CHECK(from_older->vtable.fixed_update == nullptr);
    CY_CHECK(from_older->vtable.serialize == nullptr);

    // A vtable with no create or destroy is refused: the engine would have no way to end an
    // instance's life, and "it registered but nothing works" is the worst available outcome.
    CyBehaviourVTable empty{};
    empty.struct_size = sizeof(CyBehaviourVTable);
    CY_CHECK(iface.register_behaviour(&bound.host, "Empty", &empty) == nullptr);
    CY_CHECK_EQ(iface.get_last_error_code(), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a module's log line reaches the engine's diagnostic sink") {
    // The engine's sink, not stderr: a module's diagnostic has to land in the same trace timeline
    // as everything else, or it is a diagnostic nobody reading a trace will find.
    Bound bound;
    const CyInterface& iface = table();

    static int seen = 0;
    static char last[128] = {};
    cy::DiagnosticSink previous = cy::set_diagnostic_sink(
        [](cy::DiagnosticSeverity severity, const char* category, const char* message, void*) {
            (void)severity;
            (void)category;
            ++seen;
            std::strncpy(last, message, sizeof(last) - 1);
        },
        nullptr);

    iface.log(&bound.host, static_cast<cy::u32>(cy::DiagnosticSeverity::Warning), "from a module");
    (void)cy::set_diagnostic_sink(previous, nullptr);

    CY_CHECK_EQ(seen, 1);
    CY_CHECK(std::strcmp(last, "from a module") == 0);
}
