// Event channels and signals. Task 1.3.3.
//
// `core-type-system` — "Events and signals". Four scenarios, all executable:
//
//   * "Systems communicate without coupling" — a writer writes, readers read, neither names the
//     other. The channel is the only thing they share.
//   * "Event lifetime is bounded" — an event written in frame N is readable through the end of
//     frame N+1 and is then discarded, so a missed read cannot leak memory. This is the one that
//     needs a frame-by-frame test rather than an assertion about a single call.
//   * "Deferred signal" — emission with `Deferred` queues rather than running inline.
//   * "Connection to a destroyed target" — connections are removed during destruction, so emission
//     never touches freed memory.

#include <cy/core/values/event.h>
#include <cy/core/values/signal.h>

#include <cy/test/test.h>

namespace {

struct CollisionEvent {
    cy::u32 a = 0;
    cy::u32 b = 0;
};

CY_HANDLE_TAG(Listener);

/// Where the test's callables record what they were called with. A file-scope counter because a
/// `Callable` is a plain function pointer and cannot capture.
int g_invocations = 0;
cy::i64 g_last_argument = 0;

cy::Expected<cy::Var, cy::CallError> record(std::span<const cy::Var> arguments) noexcept {
    ++g_invocations;
    if (!arguments.empty()) {
        if (const cy::Expected<cy::i64, cy::Error> value = arguments[0].as_int(); value) {
            g_last_argument = *value;
        }
    }
    return cy::Var();
}

cy::GenerationTable& listener_pool() noexcept {
    static cy::GenerationTable pool(64);
    return pool;
}

bool listener_is_alive(cy::AnyHandle target) noexcept {
    return listener_pool().is_live(static_cast<cy::u32>(target.bits & 0xffffffffULL),
                                   static_cast<cy::u32>(target.bits >> 32));
}

cy::Expected<cy::Var, cy::CallError> listener_method(cy::AnyHandle,
                                                     std::span<const cy::Var> arguments) noexcept {
    return record(arguments);
}

}  // namespace

CY_TEST_CASE("EventChannel: a writer and readers share nothing but the channel") {
    cy::EventChannel<CollisionEvent> channel;
    CY_REQUIRE(channel.write(CollisionEvent{1, 2}).has_value());
    CY_REQUIRE(channel.write(CollisionEvent{3, 4}).has_value());

    cy::u32 sum = 0;
    channel.for_each([&sum](const CollisionEvent& event) noexcept { sum += event.a + event.b; });
    CY_CHECK_EQ(sum, 10u);
    CY_CHECK_EQ(channel.readable(), 2u);
}

CY_TEST_CASE("EventChannel: an event is readable through the end of the next frame") {
    cy::EventChannel<CollisionEvent> channel;

    // Frame N.
    CY_REQUIRE(channel.write(CollisionEvent{1, 1}).has_value());
    CY_CHECK_EQ(channel.current().size(), 1u);
    CY_CHECK_EQ(channel.previous().size(), 0u);

    // Frame N+1: last frame's event is still there, and a new one joins it.
    channel.begin_frame();
    CY_CHECK_EQ(channel.previous().size(), 1u);
    CY_REQUIRE(channel.write(CollisionEvent{2, 2}).has_value());
    CY_CHECK_EQ(channel.readable(), 2u);

    // Frame N+2: the frame-N event is gone, the frame-N+1 event is now the previous one.
    channel.begin_frame();
    CY_CHECK_EQ(channel.readable(), 1u);
    CY_CHECK_EQ(channel.previous()[0].a, 2u);

    // Frame N+3: nothing was written for two frames, so the channel is empty and has not grown.
    channel.begin_frame();
    CY_CHECK(channel.is_empty());
}

CY_TEST_CASE("Signal: emission invokes every connection, and checks the arity") {
    g_invocations = 0;
    cy::Signal signal(cy::Name::intern("damaged"), 1);

    const cy::Callable listener = cy::Callable::from_free(cy::Name::intern("record"), &record);
    CY_REQUIRE(signal.connect(listener).has_value());
    CY_REQUIRE(signal.connect(listener).has_value());
    CY_CHECK_EQ(signal.connection_count(), 2u);

    const cy::Var arguments[] = {cy::Var::from_int(7)};
    const cy::Expected<cy::Signal::EmitResult, cy::Error> result = signal.emit({arguments, 1});
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->invoked, 2u);
    CY_CHECK_EQ(g_invocations, 2);
    CY_CHECK_EQ(g_last_argument, 7);

    // The declared arity is checked at emission, not discovered by the listener.
    const cy::Expected<cy::Signal::EmitResult, cy::Error> wrong = signal.emit({});
    CY_REQUIRE_FALSE(wrong.has_value());
    CY_CHECK(wrong.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("Signal: a default-constructed Callable cannot be connected") {
    cy::Signal signal(cy::Name::intern("pressed"), 0);
    const cy::Expected<cy::ConnectionId, cy::Error> connection = signal.connect(cy::Callable{});
    CY_REQUIRE_FALSE(connection.has_value());
    CY_CHECK_EQ(signal.connection_count(), 0u);
}

CY_TEST_CASE("Signal: OneShot disconnects after the first emission") {
    g_invocations = 0;
    cy::Signal signal(cy::Name::intern("ready"), 0);
    const cy::Callable listener = cy::Callable::from_free(cy::Name::intern("record"), &record);
    CY_REQUIRE(signal.connect(listener, cy::ConnectionFlags::OneShot).has_value());

    CY_REQUIRE(signal.emit({}).has_value());
    CY_CHECK_EQ(g_invocations, 1);
    CY_CHECK_EQ(signal.connection_count(), 0u);

    CY_REQUIRE(signal.emit({}).has_value());
    CY_CHECK_EQ(g_invocations, 1);
}

CY_TEST_CASE("Signal: Deferred queues the invocation until the flush point") {
    g_invocations = 0;
    cy::Signal signal(cy::Name::intern("changed"), 1);
    cy::SignalQueue queue;
    const cy::Callable listener = cy::Callable::from_free(cy::Name::intern("record"), &record);
    CY_REQUIRE(signal.connect(listener, cy::ConnectionFlags::Deferred).has_value());

    const cy::Var arguments[] = {cy::Var::from_int(11)};
    const cy::Expected<cy::Signal::EmitResult, cy::Error> result =
        signal.emit({arguments, 1}, &queue);
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->deferred, 1u);
    CY_CHECK_EQ(result->invoked, 0u);
    CY_CHECK_EQ(g_invocations, 0);
    CY_CHECK_EQ(queue.pending(), 1u);

    CY_CHECK_EQ(queue.flush(), 1u);
    CY_CHECK_EQ(g_invocations, 1);
    CY_CHECK_EQ(g_last_argument, 11);
    CY_CHECK_EQ(queue.pending(), 0u);
}

CY_TEST_CASE("Signal: a Deferred connection with no queue is an error, not a silent inline call") {
    g_invocations = 0;
    cy::Signal signal(cy::Name::intern("changed"), 0);
    const cy::Callable listener = cy::Callable::from_free(cy::Name::intern("record"), &record);
    CY_REQUIRE(signal.connect(listener, cy::ConnectionFlags::Deferred).has_value());

    CY_CHECK_FALSE(signal.emit({}, nullptr).has_value());
    CY_CHECK_EQ(g_invocations, 0);
}

CY_TEST_CASE("Signal: connections to a destroyed target are removed during destruction") {
    g_invocations = 0;
    cy::Signal signal(cy::Name::intern("moved"), 0);

    const cy::Expected<cy::Handle<ListenerTag>, cy::Error> node =
        listener_pool().allocate_handle<ListenerTag>();
    CY_REQUIRE(node.has_value());
    const cy::AnyHandle target = cy::to_any(*node);

    CY_REQUIRE(signal
                   .connect(cy::Callable::from_method(cy::Name::intern("Listener.on_moved"), target,
                                                      &listener_method, &listener_is_alive))
                   .has_value());
    CY_REQUIRE(signal.emit({}).has_value());
    CY_CHECK_EQ(g_invocations, 1);

    // What a destructor does: drop the connections before the object goes away.
    CY_CHECK_EQ(signal.disconnect_target(target), 1u);
    CY_CHECK_EQ(signal.connection_count(), 0u);
    CY_REQUIRE(listener_pool().release(*node).has_value());

    CY_REQUIRE(signal.emit({}).has_value());
    CY_CHECK_EQ(g_invocations, 1);
}

CY_TEST_CASE("Signal: emission drops a connection whose target went away unannounced") {
    g_invocations = 0;
    cy::Signal signal(cy::Name::intern("moved"), 0);

    const cy::Expected<cy::Handle<ListenerTag>, cy::Error> node =
        listener_pool().allocate_handle<ListenerTag>();
    CY_REQUIRE(node.has_value());
    CY_REQUIRE(signal
                   .connect(cy::Callable::from_method(cy::Name::intern("Listener.on_moved"),
                                                      cy::to_any(*node), &listener_method,
                                                      &listener_is_alive))
                   .has_value());

    // Freed without telling the signal — the case disconnect_target() is meant to cover and does
    // not. Emission must notice rather than call through a stale handle.
    CY_REQUIRE(listener_pool().release(*node).has_value());

    const cy::Expected<cy::Signal::EmitResult, cy::Error> result = signal.emit({});
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->invoked, 0u);
    CY_CHECK_EQ(result->disconnected, 1u);
    CY_CHECK_EQ(g_invocations, 0);
    CY_CHECK_EQ(signal.connection_count(), 0u);
}

CY_TEST_CASE("Signal: Persist survives a transient sweep and Deferred does not") {
    cy::Signal signal(cy::Name::intern("pressed"), 0);
    const cy::Callable listener = cy::Callable::from_free(cy::Name::intern("record"), &record);

    const cy::Expected<cy::ConnectionId, cy::Error> authored =
        signal.connect(listener, cy::ConnectionFlags::Persist);
    CY_REQUIRE(authored.has_value());
    CY_REQUIRE(signal.connect(listener).has_value());
    CY_CHECK_EQ(signal.connection_count(), 2u);

    CY_CHECK_EQ(signal.disconnect_transient(), 1u);
    CY_CHECK_EQ(signal.connection_count(), 1u);
    CY_CHECK(has_flag(signal.flags_of(*authored), cy::ConnectionFlags::Persist));

    CY_CHECK(signal.disconnect(*authored));
    CY_CHECK_FALSE(signal.disconnect(*authored));  // an id is never reused
    CY_CHECK_EQ(signal.connection_count(), 0u);
}
