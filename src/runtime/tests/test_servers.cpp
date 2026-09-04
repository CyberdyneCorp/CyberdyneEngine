// Backend selection, the null fallback, and reverse-order shutdown. Task 4.1.1.
//
// `engine-architecture`'s "Backend is selectable" scenario is the whole subject: the registered
// factory constructs it, "falling back to a documented default and finally to a null implementation
// that keeps handle bookkeeping valid".

#include <cy/core/memory/system_allocator.h>
#include <cy/runtime/servers.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::runtime;

/// A backend that records what happened to it, so that "brought up in this order, torn down in the
/// reverse" is an assertion rather than an inspection.
class FakeServer final : public Server {
public:
    FakeServer(const char* name, bool fails, Array<const char*>& journal) noexcept
        : name_(name), fails_(fails), journal_(&journal) {}

    [[nodiscard]] const char* backend_name() const noexcept override { return name_; }
    [[nodiscard]] Status initialize() noexcept override {
        if (fails_) {
            return fail(ErrorCode::Unavailable, "no device");
        }
        (void)journal_->push_back(name_);
        return ok();
    }
    void shutdown() noexcept override { (void)journal_->push_back(name_); }

private:
    const char* name_;
    bool fails_;
    Array<const char*>* journal_;
};

/// The factories are function pointers with no captures, so each backend is a file-scope object and
/// the factory hands its address over. That is also how a real module would do it: the server is
/// the module's, and the registry stores a pointer it never deletes.
struct Fixture {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Array<const char*> journal{allocator};
    FakeServer vulkan{"vulkan", false, journal};
    FakeServer software{"software", false, journal};
    FakeServer broken{"broken", true, journal};
    FakeServer jolt{"jolt", false, journal};
};

Expected<Server*, Error> make(Allocator& /*allocator*/, void* user) noexcept {
    return static_cast<Server*>(user);
}

}  // namespace

CY_TEST_CASE("a slot with no backend runs the null implementation") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    ServerRegistry registry(allocator);
    CY_REQUIRE(static_cast<bool>(registry.select_all(nullptr, 0)));

    for (u32 index = 0; index < kServerCount; ++index) {
        const auto kind = static_cast<ServerKind>(index);
        // Never null: a caller has no null branch to forget, and a dedicated server build takes the
        // same code path a client build does.
        CY_CHECK(registry.get(kind).is_null_backend());
        CY_CHECK(std::string_view(registry.get(kind).backend_name()) == "null");
        CY_CHECK(registry.outcome(kind) == BackendOutcome::NotRequested);
    }
    CY_CHECK_EQ(registry.live_backends(), 0U);
}

CY_TEST_CASE("the requested backend is chosen") {
    Fixture fixture;
    ServerRegistry registry(fixture.allocator);
    CY_REQUIRE(static_cast<bool>(
        registry.register_backend(ServerKind::Render, "vulkan", make, &fixture.vulkan)));
    CY_REQUIRE(static_cast<bool>(registry.register_backend(
        ServerKind::Render, "software", make, &fixture.software, /*is_default=*/true)));

    const char* wanted[kServerCount] = {};
    wanted[static_cast<u32>(ServerKind::Render)] = "vulkan";
    CY_REQUIRE(static_cast<bool>(registry.select_all(wanted, kServerCount)));

    CY_CHECK(std::string_view(registry.get(ServerKind::Render).backend_name()) == "vulkan");
    CY_CHECK(registry.outcome(ServerKind::Render) == BackendOutcome::Requested);
    CY_CHECK_EQ(registry.live_backends(), 1U);
}

CY_TEST_CASE("an unknown backend falls back to the documented default") {
    Fixture fixture;
    ServerRegistry registry(fixture.allocator);
    CY_REQUIRE(static_cast<bool>(registry.register_backend(
        ServerKind::Render, "software", make, &fixture.software, /*is_default=*/true)));

    const char* wanted[kServerCount] = {};
    wanted[static_cast<u32>(ServerKind::Render)] = "metal";
    CY_REQUIRE(static_cast<bool>(registry.select_all(wanted, kServerCount)));

    CY_CHECK(std::string_view(registry.get(ServerKind::Render).backend_name()) == "software");
    CY_CHECK(registry.outcome(ServerKind::Render) == BackendOutcome::Default);
    // What was asked for is kept, so the diagnostic can say "asked for metal, ran software".
    CY_CHECK(std::string_view(registry.slot(ServerKind::Render).requested) == "metal");
}

CY_TEST_CASE("a backend that fails to initialise is unavailable, and the default is still tried") {
    Fixture fixture;
    ServerRegistry registry(fixture.allocator);
    CY_REQUIRE(static_cast<bool>(
        registry.register_backend(ServerKind::Render, "broken", make, &fixture.broken)));
    CY_REQUIRE(static_cast<bool>(registry.register_backend(
        ServerKind::Render, "software", make, &fixture.software, /*is_default=*/true)));

    const char* wanted[kServerCount] = {};
    wanted[static_cast<u32>(ServerKind::Render)] = "broken";
    CY_REQUIRE(static_cast<bool>(registry.select_all(wanted, kServerCount)));

    CY_CHECK(std::string_view(registry.get(ServerKind::Render).backend_name()) == "software");
    CY_CHECK(registry.outcome(ServerKind::Render) == BackendOutcome::Default);
}

CY_TEST_CASE("asking for a backend and getting none is not the same as asking for none") {
    Fixture fixture;
    ServerRegistry registry(fixture.allocator);
    const char* wanted[kServerCount] = {};
    wanted[static_cast<u32>(ServerKind::Audio)] = "wasapi";
    CY_REQUIRE(static_cast<bool>(registry.select_all(wanted, kServerCount)));

    CY_CHECK(registry.get(ServerKind::Audio).is_null_backend());
    CY_CHECK(registry.outcome(ServerKind::Audio) == BackendOutcome::NullFallback);
    CY_CHECK(registry.outcome(ServerKind::Render) == BackendOutcome::NotRequested);
}

CY_TEST_CASE("servers are torn down in the exact reverse of bring-up") {
    Fixture fixture;
    ServerRegistry registry(fixture.allocator);
    CY_REQUIRE(static_cast<bool>(registry.register_backend(ServerKind::Render, "vulkan", make,
                                                           &fixture.vulkan, /*is_default=*/true)));
    CY_REQUIRE(static_cast<bool>(registry.register_backend(ServerKind::Physics, "jolt", make,
                                                           &fixture.jolt, /*is_default=*/true)));

    CY_REQUIRE(static_cast<bool>(registry.select_all(nullptr, 0)));
    CY_REQUIRE_EQ(fixture.journal.size(), usize{2});
    // Render is ServerKind 2 and Physics is 4, so bring-up order is render then physics.
    CY_CHECK(std::string_view(fixture.journal[0]) == "vulkan");
    CY_CHECK(std::string_view(fixture.journal[1]) == "jolt");

    registry.shutdown_all();
    CY_REQUIRE_EQ(fixture.journal.size(), usize{4});
    CY_CHECK(std::string_view(fixture.journal[2]) == "jolt");
    CY_CHECK(std::string_view(fixture.journal[3]) == "vulkan");

    // And the slots are back to their null backends, so a shut-down registry is still safe to ask.
    CY_CHECK(registry.get(ServerKind::Render).is_null_backend());
    CY_CHECK_EQ(registry.live_backends(), 0U);
}

CY_TEST_CASE("a registration that would make selection ambiguous is refused") {
    Fixture fixture;
    ServerRegistry registry(fixture.allocator);
    CY_REQUIRE(static_cast<bool>(registry.register_backend(ServerKind::Render, "vulkan", make,
                                                           &fixture.vulkan, /*is_default=*/true)));
    // A duplicate name within one kind.
    CY_CHECK_FALSE(static_cast<bool>(
        registry.register_backend(ServerKind::Render, "vulkan", make, &fixture.software)));
    // A second default: "the documented default" is singular, and two would make which one runs a
    // function of registration order.
    CY_CHECK_FALSE(static_cast<bool>(registry.register_backend(
        ServerKind::Render, "software", make, &fixture.software, /*is_default=*/true)));
    // The same name under a different kind is fine — a "null" physics and a "null" audio are two
    // backends.
    CY_CHECK(static_cast<bool>(
        registry.register_backend(ServerKind::Physics, "vulkan", make, &fixture.jolt)));

    CY_CHECK_FALSE(static_cast<bool>(registry.register_backend(ServerKind::Count, "x", make)));
    CY_CHECK_FALSE(static_cast<bool>(registry.register_backend(ServerKind::Render, "y", nullptr)));
    CY_CHECK_FALSE(static_cast<bool>(registry.register_backend(ServerKind::Render, "", make)));
}

CY_TEST_CASE("every kind and outcome has a name") {
    for (u32 index = 0; index < kServerCount; ++index) {
        CY_CHECK(std::string_view(server_kind_name(static_cast<ServerKind>(index))) != "unknown");
    }
    CY_CHECK(std::string_view(backend_outcome_name(BackendOutcome::NullFallback)) ==
             "null-fallback");
}
