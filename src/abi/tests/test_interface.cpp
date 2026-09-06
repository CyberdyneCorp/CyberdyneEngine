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
#include <cy/ecs/system.h>
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
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "1.1") != nullptr);

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
    //
    // THE ID IS DELIBERATELY ONE NO WORLD HAS. It used to be 0, and 0 stopped meaning "not a
    // component" at ABI 1.1: `record_or_import` reaches the engine's own registry now, and every
    // world registers `cy::ecs::Parent` at index 0 before anybody asks. Component 0 therefore
    // resolves and the failure moves to the FIELD, which is OUT_OF_RANGE and a different sentence.
    // A component id past `kMaxComponentTypes` cannot be registered by anything, so it says what
    // this case means rather than what it used to happen to say.
    CyVar out = cy::abi::var_i64(4242);
    const CyResult result = iface.component_get_var(world, entity, 4096, 0, &out);
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

// --- ABI 1.1 ------------------------------------------------------------------------------------
//
// The entries M5's task 1.2 appended, and the two enums it appended beside them. Every case here is
// still through the table: an editor is a module in this respect — it did not build the world, it
// has to discover what is in it.

CY_TEST_CASE("the appended enums carry the engine's own values") {
    // The static assertions in src/abi/src/interface.cpp already make a mismatch a compile error.
    // This is the other half: that the NUMBERS a consumer reads out of the header are the numbers
    // the engine puts on the wire. `Log.info` arriving as `[error]` for a whole milestone is what
    // happens when only one of those two is checked.
    Bound bound;
    const CyInterface& iface = table();

    static cy::DiagnosticSeverity observed = cy::DiagnosticSeverity::Error;
    cy::DiagnosticSink previous =
        cy::set_diagnostic_sink([](cy::DiagnosticSeverity severity, const char*, const char*,
                                   void*) { observed = severity; },
                                nullptr);
    iface.log(&bound.host, CY_SEVERITY_INFO, "informational");
    (void)cy::set_diagnostic_sink(previous, nullptr);
    CY_CHECK_EQ(observed, cy::DiagnosticSeverity::Info);

    CY_CHECK_EQ(static_cast<cy::u32>(CY_STAGE_PRE_SIMULATION),
                static_cast<cy::u32>(cy::ecs::Stage::PreSimulation));
    CY_CHECK_EQ(static_cast<cy::u32>(CY_STAGE_RENDER),
                static_cast<cy::u32>(cy::ecs::Stage::Render));
    CY_CHECK_EQ(cy::ecs::kStageCount, static_cast<cy::u32>(CY_STAGE_RENDER) + 1U);
}

CY_TEST_CASE("a caller can enumerate and describe every component in a world it did not build") {
    // THE INSPECTOR'S WHOLE READ PATH. Before 1.1 the table could only describe what a module had
    // itself registered, so an editor attached to a running engine could enumerate nothing.
    Bound bound;
    const CyInterface& iface = table();
    const CyComponentTypeDesc desc = probe_desc();
    const CyComponentTypeId probe = iface.world_register_component(&bound.binding, &desc);
    CY_REQUIRE(probe != CY_COMPONENT_TYPE_INVALID);

    // Every id in the world, not only the one this caller registered: the count is the ECS
    // registry's, and the ids are 0 .. count - 1.
    const cy::u32 count = iface.world_component_count(&bound.binding);
    CY_CHECK_GE(count, 1U);
    CY_CHECK_LT(probe, count);

    CyComponentInfo info{};
    info.struct_size = sizeof(CyComponentInfo);
    CY_CHECK_EQ(iface.world_component_info(&bound.binding, probe, &info), CY_RESULT_OK);
    CY_CHECK_EQ(info.size, static_cast<cy::u32>(sizeof(Probe)));
    CY_CHECK_EQ(info.alignment, static_cast<cy::u32>(alignof(Probe)));
    CY_CHECK_EQ(info.field_count, 3U);
    CY_CHECK(std::strcmp(info.name, "Probe") == 0);

    CyFieldDesc field{};
    CY_CHECK_EQ(iface.world_component_field(&bound.binding, probe, 1, &field), CY_RESULT_OK);
    CY_CHECK(std::strcmp(field.name, "speed") == 0);
    CY_CHECK_EQ(field.type, static_cast<cy::u32>(CY_VAR_F32));
    CY_CHECK_EQ(field.offset, 12U);
    CY_CHECK_EQ(field.size, 4U);

    // Out of range is reported, not guessed at.
    CY_CHECK_EQ(iface.world_component_field(&bound.binding, probe, 3, &field),
                CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(iface.world_component_info(&bound.binding, 4096, &info), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("a component the engine registered is describable and readable through the table") {
    // The case that only exists because of `record_or_import`. `register_builtin` is the route the
    // ECS's own structural components take; a reflected engine component takes the same route with
    // a `TypeInfo` behind it, and this is what the table can say about one without reflection.
    Bound bound;
    const CyInterface& iface = table();
    cy::Expected<cy::ecs::ComponentTypeId, cy::Error> registered =
        bound.world.components().register_builtin("cy::test::Engine", 8, 8);
    CY_REQUIRE(registered.has_value());

    // Found by name through the table, which it was not before 1.1: `world_find_component` only saw
    // what a module had described.
    const CyComponentTypeId found = iface.world_find_component(&bound.binding, "cy::test::Engine");
    CY_CHECK_EQ(found, registered.value());

    CyComponentInfo info{};
    info.struct_size = sizeof(CyComponentInfo);
    CY_CHECK_EQ(iface.world_component_info(&bound.binding, found, &info), CY_RESULT_OK);
    CY_CHECK_EQ(info.size, 8U);
    // Zero DESCRIBABLE fields, which is the honest answer for a component with no reflection
    // metadata rather than an error: the engine knows the type is there and cannot say what is in
    // it.
    CY_CHECK_EQ(info.field_count, 0U);
}

CY_TEST_CASE("the hierarchy is readable and writable through the table") {
    Bound bound;
    const CyInterface& iface = table();

    const CyEntity parent = iface.world_create_entity(&bound.binding);
    const CyEntity first = iface.world_create_entity(&bound.binding);
    const CyEntity second = iface.world_create_entity(&bound.binding);
    CY_REQUIRE(parent != CY_ENTITY_NULL);

    CY_CHECK_EQ(iface.world_parent(&bound.binding, first), CY_ENTITY_NULL);
    CY_CHECK_EQ(iface.world_set_parent(&bound.binding, first, parent), CY_RESULT_OK);
    CY_CHECK_EQ(iface.world_set_parent(&bound.binding, second, parent), CY_RESULT_OK);

    CY_CHECK_EQ(iface.world_parent(&bound.binding, first), parent);
    CY_CHECK_EQ(iface.world_child_count(&bound.binding, parent), 2U);
    // The order is the ECS's, so the assertion is on the SET rather than on the sequence — the
    // header says so at the entry, and a test that pinned the order would be pinning something
    // `ecs-core` explicitly leaves unspecified.
    const CyEntity zero = iface.world_child(&bound.binding, parent, 0);
    const CyEntity one = iface.world_child(&bound.binding, parent, 1);
    const bool both_present = (zero == first && one == second) || (zero == second && one == first);
    CY_CHECK(both_present);
    CY_CHECK_EQ(iface.world_child(&bound.binding, parent, 2), CY_ENTITY_NULL);

    // Reparenting to null makes a root again.
    CY_CHECK_EQ(iface.world_set_parent(&bound.binding, first, CY_ENTITY_NULL), CY_RESULT_OK);
    CY_CHECK_EQ(iface.world_parent(&bound.binding, first), CY_ENTITY_NULL);
    CY_CHECK_EQ(iface.world_child_count(&bound.binding, parent), 1U);
}

CY_TEST_CASE("chunks come back as columns, sized in two calls") {
    // The entry a Swift system's inner loop and an editor's bulk read both wanted and 1.0 did not
    // have. `Systems.swift` said so in as many words: "CyInterface at 1.0 has thirty entries and
    // NONE of them hands a module a chunk".
    Bound bound;
    const CyInterface& iface = table();
    const CyComponentTypeDesc desc = probe_desc();
    const CyComponentTypeId probe = iface.world_register_component(&bound.binding, &desc);
    CY_REQUIRE(probe != CY_COMPONENT_TYPE_INVALID);

    for (cy::u32 index = 0; index < 8; ++index) {
        const CyEntity entity = iface.world_create_entity(&bound.binding);
        CY_REQUIRE(entity != CY_ENTITY_NULL);
        Probe initial;
        initial.speed = static_cast<float>(index);
        CY_REQUIRE(iface.world_add_component(&bound.binding, entity, probe, &initial) ==
                   CY_RESULT_OK);
    }

    // The sizing call: no buffer, no capacity, and the count comes back anyway.
    cy::u32 total = 0;
    CY_CHECK_EQ(iface.world_chunks(&bound.binding, probe, nullptr, 0, &total), CY_RESULT_OK);
    CY_CHECK_EQ(total, 1U);

    CyChunk chunks[4] = {};
    cy::u32 filled = 0;
    CY_CHECK_EQ(iface.world_chunks(&bound.binding, probe, chunks, 4, &filled), CY_RESULT_OK);
    CY_REQUIRE(filled == 1U);
    CY_CHECK_EQ(chunks[0].entity_count, 8U);
    CY_CHECK_EQ(chunks[0].stride, static_cast<cy::u32>(sizeof(Probe)));
    CY_REQUIRE(chunks[0].data != nullptr);
    CY_REQUIRE(chunks[0].entities != nullptr);

    // THE COLUMN IS THE STORAGE, not a copy: the values written one entity at a time above are
    // contiguous here, which is the whole reason the entry exists.
    float speeds = 0.0F;
    for (cy::u32 row = 0; row < chunks[0].entity_count; ++row) {
        const auto* probes = static_cast<const Probe*>(chunks[0].data);
        speeds += probes[row].speed;
        CY_CHECK(iface.world_entity_alive(&bound.binding, chunks[0].entities[row]));
    }
    CY_CHECK_EQ(speeds, 28.0F);  // 0 + 1 + ... + 7

    // And the borrow carries the epoch, so a structural change makes it detectably stale.
    const CyBorrow borrow{chunks[0].data, chunks[0].epoch};
    CY_CHECK(iface.borrow_valid(&bound.binding, borrow));
    (void)iface.world_create_entity(&bound.binding);
    CY_CHECK_FALSE(iface.borrow_valid(&bound.binding, borrow));
}

CY_TEST_CASE("a narrow integer field round-trips at its own width and refuses what will not fit") {
    // ABI 1.1's seven appended `CyVarType`s. Before them the only integer was 64 bits wide, and
    // every reflected engine component disagrees with that — `cy::scene::ChildOrder` is a u32 and
    // `cy::scene::NodeState` a u8.
    struct Narrow {
        cy::u8 flags = 0;
        cy::i16 offset = 0;
        cy::u32 index = 0;
    };
    static const CyFieldDesc fields[] = {
        {sizeof(CyFieldDesc), CY_VAR_U8, offsetof(Narrow, flags), 1, "flags"},
        {sizeof(CyFieldDesc), CY_VAR_I16, offsetof(Narrow, offset), 2, "offset"},
        {sizeof(CyFieldDesc), CY_VAR_U32, offsetof(Narrow, index), 4, "index"},
    };
    Bound bound;
    const CyInterface& iface = table();
    CyComponentTypeDesc desc{};
    desc.struct_size = sizeof(CyComponentTypeDesc);
    desc.size = sizeof(Narrow);
    desc.alignment = alignof(Narrow);
    desc.field_count = 3;
    desc.name = "Narrow";
    desc.fields = fields;

    const CyComponentTypeId narrow = iface.world_register_component(&bound.binding, &desc);
    CY_REQUIRE(narrow != CY_COMPONENT_TYPE_INVALID);
    const CyEntity entity = iface.world_create_entity(&bound.binding);
    CY_REQUIRE(iface.world_add_component(&bound.binding, entity, narrow, nullptr) == CY_RESULT_OK);

    CyVar value = cy::abi::var_i64(200);
    value.type = CY_VAR_U8;
    CY_CHECK_EQ(iface.component_set_var(&bound.binding, entity, narrow, 0, &value), CY_RESULT_OK);

    // NEGATIVE, so that sign extension is exercised rather than assumed: a 16-bit -3 read back as
    // 65533 is exactly the bug the width tag exists to prevent.
    CyVar negative = cy::abi::var_i64(-3);
    negative.type = CY_VAR_I16;
    CY_CHECK_EQ(iface.component_set_var(&bound.binding, entity, narrow, 1, &negative),
                CY_RESULT_OK);

    CyVar wide = cy::abi::var_i64(4000000000LL);
    wide.type = CY_VAR_U32;
    CY_CHECK_EQ(iface.component_set_var(&bound.binding, entity, narrow, 2, &wide), CY_RESULT_OK);

    CyVar read{};
    CY_CHECK_EQ(iface.component_get_var(&bound.binding, entity, narrow, 0, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.type, static_cast<cy::u32>(CY_VAR_U8));
    CY_CHECK_EQ(read.payload.as_i64, 200);
    CY_CHECK_EQ(iface.component_get_var(&bound.binding, entity, narrow, 1, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.payload.as_i64, -3);
    CY_CHECK_EQ(iface.component_get_var(&bound.binding, entity, narrow, 2, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.payload.as_i64, 4000000000LL);

    // TRUNCATION IS REFUSED RATHER THAN PERFORMED. Storing 300 in a u8 as 44 is the kind of defect
    // that is found in a save file weeks later.
    CyVar overflow = cy::abi::var_i64(300);
    overflow.type = CY_VAR_U8;
    CY_CHECK_EQ(iface.component_set_var(&bound.binding, entity, narrow, 0, &overflow),
                CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(iface.component_get_var(&bound.binding, entity, narrow, 0, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.payload.as_i64, 200);  // unchanged by the refused write
}
