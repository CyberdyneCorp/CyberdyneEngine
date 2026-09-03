// `Callable` — the four kinds, and the four failures. Task 1.3.4.
//
// `core-type-system` — "Callable". Its named scenario is "Script callable survives reload": after a
// hot reload, a callable referring to a script function is re-resolved by name, or reported invalid
// if the function no longer exists. Both halves are exercised here against a fake `ScriptHost`,
// because the real one is M5 and the seam is what M1 owes it.

#include <cy/core/values/callable.h>

#include <cy/test/test.h>

#include <string>

namespace {

CY_HANDLE_TAG(Node);

cy::Expected<cy::Var, cy::CallError> add_two(std::span<const cy::Var> arguments) noexcept {
    if (arguments.size() != 2) {
        return cy::call_failed(cy::CallErrorKind::WrongArgumentCount, cy::Name{}, "add takes two");
    }
    const cy::Expected<cy::i64, cy::Error> a = arguments[0].as_int();
    const cy::Expected<cy::i64, cy::Error> b = arguments[1].as_int();
    if (!a || !b) {
        return cy::call_failed(cy::CallErrorKind::WrongArgumentType, cy::Name{}, "add takes Ints");
    }
    return cy::Var::from_int(*a + *b);
}

/// The pool a method callable's target lives in. A file-scope table so the probe, which is a plain
/// function pointer, can reach it — which is exactly the shape a server's probe has.
cy::GenerationTable& node_pool() noexcept {
    static cy::GenerationTable pool(64);
    return pool;
}

bool node_is_alive(cy::AnyHandle target) noexcept {
    return node_pool().is_live(static_cast<cy::u32>(target.bits & 0xffffffffULL),
                               static_cast<cy::u32>(target.bits >> 32));
}

cy::Expected<cy::Var, cy::CallError> node_identity(cy::AnyHandle target,
                                                   std::span<const cy::Var> arguments) noexcept {
    if (!arguments.empty()) {
        return cy::call_failed(cy::CallErrorKind::WrongArgumentCount);
    }
    return cy::Var::from_handle(target);
}

/// A script module that can be reloaded, and can drop a function while it is at it.
class FakeScriptHost final : public cy::ScriptHost {
public:
    [[nodiscard]] cy::FreeCallFn resolve(cy::Name function) const noexcept override {
        if (!exports_ || function != cy::Name::intern("script.add")) {
            return nullptr;
        }
        return &add_two;
    }
    [[nodiscard]] cy::u64 revision() const noexcept override { return revision_; }

    void reload(bool exports) noexcept {
        exports_ = exports;
        ++revision_;
    }

private:
    bool exports_ = true;
    cy::u64 revision_ = 1;
};

/// Installs a host for the duration of a test and restores whatever was there. Tests share a
/// process, so a host left installed would leak into the next one.
class ScopedScriptHost {
public:
    explicit ScopedScriptHost(cy::ScriptHost* host) noexcept
        : previous_(cy::set_script_host(host)) {}
    ~ScopedScriptHost() { cy::set_script_host(previous_); }

    ScopedScriptHost(const ScopedScriptHost&) = delete;
    ScopedScriptHost& operator=(const ScopedScriptHost&) = delete;

private:
    cy::ScriptHost* previous_;
};

}  // namespace

CY_TEST_CASE("Callable: the default one is not callable, and says so") {
    const cy::Callable unset;
    CY_CHECK_EQ(unset.kind(), cy::CallableKind::Invalid);
    CY_CHECK_FALSE(unset.is_valid());

    const cy::Expected<cy::Var, cy::CallError> result = unset.invoke();
    CY_REQUIRE_FALSE(result.has_value());
    CY_CHECK_EQ(result.error().kind, cy::CallErrorKind::NotCallable);
}

CY_TEST_CASE("Callable: a free function is invoked with Var arguments") {
    const cy::Callable add = cy::Callable::from_free(cy::Name::intern("add"), &add_two);
    CY_CHECK(add.is_valid());

    const cy::Var arguments[] = {cy::Var::from_int(2), cy::Var::from_int(40)};
    const cy::Expected<cy::Var, cy::CallError> result = add.invoke({arguments, 2});
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(*result->as_int(), 42);
}

CY_TEST_CASE("Callable: the four failures are distinguishable") {
    const cy::Callable add = cy::Callable::from_free(cy::Name::intern("add"), &add_two);

    const cy::Var one[] = {cy::Var::from_int(1)};
    const cy::Expected<cy::Var, cy::CallError> arity = add.invoke({one, 1});
    CY_REQUIRE_FALSE(arity.has_value());
    CY_CHECK_EQ(arity.error().kind, cy::CallErrorKind::WrongArgumentCount);

    const cy::Var wrong[] = {cy::Var::from_int(1), cy::Var::from_string("two")};
    const cy::Expected<cy::Var, cy::CallError> typed = add.invoke({wrong, 2});
    CY_REQUIRE_FALSE(typed.has_value());
    CY_CHECK_EQ(typed.error().kind, cy::CallErrorKind::WrongArgumentType);

    CY_CHECK_EQ(std::string(cy::call_error_kind_name(cy::CallErrorKind::TargetInvalid)),
                std::string("TargetInvalid"));
}

CY_TEST_CASE("Callable: a method reports TargetInvalid once its target is freed") {
    const cy::Expected<cy::Handle<NodeTag>, cy::Error> node =
        node_pool().allocate_handle<NodeTag>();
    CY_REQUIRE(node.has_value());

    const cy::Callable method = cy::Callable::from_method(
        cy::Name::intern("Node.identity"), cy::to_any(*node), &node_identity, &node_is_alive);
    CY_CHECK(method.is_valid());
    CY_CHECK(method.invoke().has_value());

    CY_REQUIRE(node_pool().release(*node).has_value());

    CY_CHECK_FALSE(method.is_valid());
    const cy::Expected<cy::Var, cy::CallError> after = method.invoke();
    CY_REQUIRE_FALSE(after.has_value());
    CY_CHECK_EQ(after.error().kind, cy::CallErrorKind::TargetInvalid);
}

CY_TEST_CASE("Callable: a script callable is re-resolved after a reload") {
    FakeScriptHost host;
    const ScopedScriptHost installed(&host);

    const cy::Callable script = cy::Callable::from_script(cy::Name::intern("script.add"));
    CY_CHECK(script.is_valid());

    const cy::Var arguments[] = {cy::Var::from_int(1), cy::Var::from_int(2)};
    CY_CHECK_EQ(*script.invoke({arguments, 2})->as_int(), 3);

    // Reloaded, still exporting: the same callable resolves against the new module.
    host.reload(/*exports=*/true);
    CY_CHECK(script.is_valid());
    CY_CHECK_EQ(*script.invoke({arguments, 2})->as_int(), 3);

    // Reloaded without the function: reported invalid rather than calling into what used to be
    // there.
    host.reload(/*exports=*/false);
    CY_CHECK_FALSE(script.is_valid());
    const cy::Expected<cy::Var, cy::CallError> gone = script.invoke({arguments, 2});
    CY_REQUIRE_FALSE(gone.has_value());
    CY_CHECK_EQ(gone.error().kind, cy::CallErrorKind::NoSuchMethod);
}

CY_TEST_CASE("Callable: with no script host at all, a script callable is invalid") {
    const ScopedScriptHost none(nullptr);
    const cy::Callable script = cy::Callable::from_script(cy::Name::intern("script.add"));
    CY_CHECK_FALSE(script.is_valid());
    const cy::Expected<cy::Var, cy::CallError> result = script.invoke();
    CY_REQUIRE_FALSE(result.has_value());
    CY_CHECK_EQ(result.error().kind, cy::CallErrorKind::TargetInvalid);
}

CY_TEST_CASE("Callable: bound arguments are prepended") {
    const cy::Callable add = cy::Callable::from_free(cy::Name::intern("add"), &add_two);
    const cy::Var bound[] = {cy::Var::from_int(10)};
    const cy::Callable add_ten = cy::Callable::bind(add, {bound, 1});

    CY_CHECK_EQ(add_ten.kind(), cy::CallableKind::Bound);
    CY_CHECK_EQ(add_ten.bound_argument_count(), 1u);
    CY_CHECK(add_ten.is_valid());

    const cy::Var rest[] = {cy::Var::from_int(5)};
    const cy::Expected<cy::Var, cy::CallError> result = add_ten.invoke({rest, 1});
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(*result->as_int(), 15);
}

CY_TEST_CASE("Callable: equality is identity, so a connection can be found again") {
    const cy::Name name = cy::Name::intern("add");
    const cy::Callable a = cy::Callable::from_free(name, &add_two);
    const cy::Callable b = cy::Callable::from_free(name, &add_two);
    const cy::Callable other = cy::Callable::from_free(cy::Name::intern("other"), &add_two);

    CY_CHECK(a == b);
    CY_CHECK(a != other);

    const cy::Var bound[] = {cy::Var::from_int(1)};
    const cy::Callable first_binding = cy::Callable::bind(a, {bound, 1});
    const cy::Callable second_binding = cy::Callable::bind(a, {bound, 1});
    const cy::Callable copy = first_binding;
    CY_CHECK(first_binding != second_binding);  // two separate bindings, not one
    CY_CHECK(first_binding == copy);
}

CY_TEST_CASE("Callable: a Var can carry one across a boundary") {
    const cy::Callable add = cy::Callable::from_free(cy::Name::intern("add"), &add_two);
    const cy::Var boxed = cy::Var::from_callable(add);

    CY_REQUIRE_EQ(boxed.type(), cy::VarType::Callable);
    CY_REQUIRE(boxed.callable() != nullptr);
    CY_CHECK(*boxed.callable() == add);

    const cy::Var arguments[] = {cy::Var::from_int(3), cy::Var::from_int(4)};
    CY_CHECK_EQ(*boxed.callable()->invoke({arguments, 2})->as_int(), 7);
}
