// The compiler: what it builds, and — the half that matters more — what it refuses.
//
// Every refusal case here is a scenario `sequencing-and-cinematics` states, and each asserts the
// DIAGNOSTIC CODE rather than only the failure, so a test cannot pass because compilation failed
// for some other reason.

#include "fixture.h"

#include <cy/sequencing/compile.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

/// A presentation cinematic: a light track with two sections and a marker. The base every case
/// below modifies in exactly one way, so the diagnostic it produces has one cause.
[[nodiscard]] SequenceSource base_sequence() noexcept {
    SequenceSource source(allocator());
    source.name = Name::intern("reveal");
    source.stable_id = 1;
    source.rate = Rate{24, 1};
    source.domain = ClockDomain::Presentation;
    source.duration = SequenceTime::from_frame(96);
    (void)source.bindings.push_back(binding_of(10, "lamp", BindingKind::Entity));
    (void)source.bindings.push_back(binding_of(11, "shot_camera", BindingKind::Camera));

    Track light = track_of("lamp intensity", TrackKind::Property, 100, 10);
    light.subsystem = SubsystemId::Light;
    Section first = section_over(0, 48, 1000);
    Channel intensity = scalar_channel("intensity", 1);
    add_key(intensity, 0, 0.0F);
    add_key(intensity, 24, 1.0F);
    add_key(intensity, 48, 0.25F);
    (void)first.channels.push_back(std::move(intensity));
    (void)light.sections.push_back(std::move(first));
    (void)source.tracks.push_back(std::move(light));

    MarkerDeclaration marker;
    marker.time = SequenceTime::from_frame(24);
    marker.name = Name::intern("reveal_beat");
    marker.stable_id = 7;
    (void)source.markers.push_back(marker);
    return source;
}

struct Compiled {
    explicit Compiled(Allocator& alloc) noexcept : program(alloc), report(alloc) {}
    Program program;
    CompileReport report;
};

}  // namespace

CY_TEST_CASE("sequence_compile: a timeline compiles to segments, an index and an event table") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    const SequenceSource source = base_sequence();

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));

    CY_CHECK_EQ(compiled.report.errors, 0U);
    CY_CHECK_EQ(compiled.report.segments, 1U);
    CY_CHECK_EQ(compiled.report.channels, 1U);
    CY_CHECK_EQ(compiled.report.markers, 1U);
    CY_CHECK_GT(compiled.program.bucket_count(), 0U);

    // The debug map keeps the authored identity — "a debug map back to authored identities".
    CY_REQUIRE_EQ(compiled.program.debug().size(), 1U);
    CY_CHECK_EQ(compiled.program.debug()[0].track_stable_id, 100U);
    CY_CHECK_EQ(compiled.program.debug()[0].section_stable_id, 1000U);

    // The property is an integer pair by now. THE PROGRAM HOLDS NO STRING PATH.
    CY_REQUIRE_EQ(compiled.program.channels().size(), 1U);
    CY_CHECK(compiled.program.channels()[0].target.valid());
    CY_CHECK_EQ(compiled.program.channels()[0].target.property, kIntensity);
}

CY_TEST_CASE("sequence_compile: an impossible combination is rejected") {
    // "**WHEN** a presentation-domain sequence contains an authoritative gameplay track **THEN**
    // compilation SHALL fail naming the track."
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    Track command = track_of("unlock the door", TrackKind::GameplayCommand, 200, 10);
    command.authority = AuthorityClass::AuthoritativeGameplay;
    command.command_stable_id = 4242;
    (void)command.sections.push_back(section_over(40, 41, 2000));
    (void)source.tracks.push_back(std::move(command));

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    const Diagnostic* diagnostic = compiled.report.first(DiagnosticCode::DomainForbidsAuthority);
    CY_REQUIRE(diagnostic != nullptr);
    CY_CHECK_EQ(diagnostic->track_stable_id, 200U);
    CY_CHECK_EQ(diagnostic->track, Name::intern("unlock the door"));

    // And the same sequence in the simulation domain compiles, which is what proves the refusal is
    // about the domain rather than about the track.
    source.domain = ClockDomain::Simulation;
    (void)source.required_outcomes.push_back(RequiredOutcome{200, 0});
    Compiled again(allocator());
    SequenceCompiler second(allocator(), registry.registry);
    CY_CHECK(second.compile(source, CompileOptions{}, again.program, again.report));
}

CY_TEST_CASE("sequence_compile: gameplay goes through gameplay") {
    // "Authoritative gameplay changes SHALL be expressed as commands and events, not as property
    // tracks writing authoritative component data."
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    source.domain = ClockDomain::Simulation;
    source.tracks[0].authority = AuthorityClass::AuthoritativeGameplay;

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    CY_CHECK(compiled.report.has(DiagnosticCode::AuthoritativeStateWrittenDirectly));
}

CY_TEST_CASE("sequence_compile: a camera is driven through the stack, never written") {
    // M8.c's exit criterion, as a compile error. A transform track bound to a camera is the way
    // this is normally got wrong, and it does not compile.
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    Track transform = track_of("camera transform", TrackKind::Transform, 300, 11);
    Section section = section_over(0, 48, 3000);
    CY_REQUIRE(add_transform_channels(section, 500));
    (void)transform.sections.push_back(std::move(section));
    (void)source.tracks.push_back(std::move(transform));

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    const Diagnostic* diagnostic = compiled.report.first(DiagnosticCode::CameraTransformWritten);
    CY_REQUIRE(diagnostic != nullptr);
    CY_CHECK_EQ(diagnostic->track_stable_id, 300U);

    // The same shot expressed as a CAMERA track — rig selection with a blend — compiles.
    source.tracks[source.tracks.size() - 1] = track_of("shot A", TrackKind::Camera, 300, 11);
    Section shot = section_over(0, 48, 3000);
    shot.blend_in_seconds = 1.0F;
    (void)source.tracks[source.tracks.size() - 1].sections.push_back(std::move(shot));
    Compiled again(allocator());
    SequenceCompiler second(allocator(), registry.registry);
    CY_CHECK(second.compile(source, CompileOptions{}, again.program, again.report));
}

CY_TEST_CASE("sequence_compile: undeclared consequence is caught") {
    // "**WHEN** a sequence with authoritative tracks is marked skippable without declaring outcomes
    // **THEN** compilation SHALL fail."
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    source.domain = ClockDomain::Simulation;
    source.skip = SkipPolicy::ApplyRequiredOutcomes;
    Track command = track_of("unlock", TrackKind::GameplayCommand, 200, 10);
    command.authority = AuthorityClass::AuthoritativeGameplay;
    command.command_stable_id = 4242;
    (void)command.sections.push_back(section_over(40, 41, 2000));
    (void)source.tracks.push_back(std::move(command));

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    CY_CHECK(compiled.report.has(DiagnosticCode::SkippableWithoutOutcomes));

    // Declare the outcome and it compiles — the door will open even when the player skips.
    (void)source.required_outcomes.push_back(RequiredOutcome{200, 0});
    Compiled again(allocator());
    SequenceCompiler second(allocator(), registry.registry);
    CY_CHECK(second.compile(source, CompileOptions{}, again.program, again.report));
    CY_CHECK_EQ(again.program.required_outcomes().size(), 1U);
}

CY_TEST_CASE("sequence_compile: an unsuitable track is rejected, naming the property") {
    // "**WHEN** a non-deterministic track is used in a deterministic sequence **THEN** compilation
    // SHALL fail naming the track and the property that disqualified it."
    Registry registry(allocator());
    CY_REQUIRE(registry.build(true, /*light_deterministic=*/false));
    SequenceSource source = base_sequence();
    source.deterministic_profile = true;

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    const Diagnostic* diagnostic = compiled.report.first(DiagnosticCode::AdapterNotDeterministic);
    CY_REQUIRE(diagnostic != nullptr);
    CY_CHECK_EQ(diagnostic->detail, Name::intern("light"));
}

CY_TEST_CASE("sequence_compile: a section that declares restoration needs an adapter that can") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build(/*light_can_restore=*/false));
    SequenceSource source = base_sequence();
    source.tracks[0].sections[0].completion = CompletionPolicy::Restore;

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    CY_CHECK(compiled.report.has(DiagnosticCode::AdapterCannotRestore));
}

CY_TEST_CASE("sequence_compile: an unknown property and a mistyped one are different diagnoses") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    Channel unknown = scalar_channel("brightness", 2);
    add_key(unknown, 0, 1.0F);
    (void)source.tracks[0].sections[0].channels.push_back(std::move(unknown));

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    const Diagnostic* diagnostic = compiled.report.first(DiagnosticCode::UnknownProperty);
    CY_REQUIRE(diagnostic != nullptr);
    CY_CHECK_EQ(diagnostic->detail, Name::intern("brightness"));

    // The same name at the wrong type is a different message, because the fix is different.
    SequenceSource mistyped = base_sequence();
    Channel colour(allocator());
    colour.property = Name::intern("colour");
    colour.type = ChannelType::Vector;  // declared as Color
    (void)colour.keys.push_back(Key{});
    (void)mistyped.tracks[0].sections[0].channels.push_back(std::move(colour));
    Compiled second_compiled(allocator());
    SequenceCompiler second(allocator(), registry.registry);
    CY_CHECK_FALSE(second.compile(mistyped, CompileOptions{}, second_compiled.program,
                                  second_compiled.report));
    CY_CHECK(second_compiled.report.has(DiagnosticCode::PropertyTypeMismatch));
}

CY_TEST_CASE("sequence_compile: a boolean channel holds rather than interpolating") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    Channel flag(allocator());
    flag.property = Name::intern("intensity");
    flag.type = ChannelType::Boolean;
    Key key;
    key.interpolation = Interpolation::Linear;  // the mistake
    (void)flag.keys.push_back(key);
    (void)source.tracks[0].sections[0].channels.push_back(std::move(flag));

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    CY_CHECK(compiled.report.has(DiagnosticCode::DiscreteChannelInterpolated));
}

CY_TEST_CASE("sequence_compile: a transform channel names the three that replace it") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    Channel transform(allocator());
    transform.property = Name::intern("intensity");
    transform.type = ChannelType::Transform;
    (void)transform.keys.push_back(Key{});
    (void)source.tracks[0].sections[0].channels.push_back(std::move(transform));

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, CompileOptions{}, compiled.program, compiled.report));
    CY_CHECK(compiled.report.has(DiagnosticCode::TransformChannelUnsupported));
}

CY_TEST_CASE("sequence_compile: a cycle in nesting is rejected at author time") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    Track nested = track_of("itself", TrackKind::NestedSequence, 400, 0);
    nested.nested_sequence = 1;  // its own stable id
    (void)source.tracks.push_back(std::move(nested));

    CompileOptions options;
    options.nested_user = &source;
    options.nested_resolver = [](u64 id, void* user) noexcept -> const SequenceSource* {
        const auto* root = static_cast<const SequenceSource*>(user);
        return (root->stable_id == id) ? root : nullptr;
    };

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_CHECK_FALSE(compiler.compile(source, options, compiled.program, compiled.report));
    CY_CHECK(compiled.report.has(DiagnosticCode::NestedCycle));
}

CY_TEST_CASE("sequence_compile: a nested sequence flattens and keeps its provenance") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource child = base_sequence();
    child.stable_id = 2;
    child.name = Name::intern("flicker");

    SequenceSource parent(allocator());
    parent.name = Name::intern("act");
    parent.stable_id = 1;
    parent.rate = Rate{24, 1};
    parent.duration = SequenceTime::from_frame(200);
    (void)parent.bindings.push_back(binding_of(10, "lamp", BindingKind::Entity));
    Track nested = track_of("flicker", TrackKind::NestedSequence, 400, 0);
    nested.nested_sequence = 2;
    nested.nested_offset = SequenceTime::from_frame(100);
    (void)parent.tracks.push_back(std::move(nested));

    CompileOptions options;
    options.nested_user = &child;
    options.nested_resolver = [](u64 id, void* user) noexcept -> const SequenceSource* {
        const auto* source = static_cast<const SequenceSource*>(user);
        return (source->stable_id == id) ? source : nullptr;
    };

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(parent, options, compiled.program, compiled.report));
    CY_CHECK_EQ(compiled.report.nested_flattened, 1U);
    CY_REQUIRE_EQ(compiled.program.segments().size(), 1U);
    // Offset applied, and the debugger can still say which nested sequence authored it.
    CY_CHECK_EQ(compiled.program.segments()[0].start, SequenceTime::from_frame(100).ticks());
    CY_CHECK_EQ(compiled.program.debug()[0].nested_sequence, 2U);
}

CY_TEST_CASE("sequence_compile: compression reports the error it actually achieved") {
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    SequenceSource source = base_sequence();
    // A ramp with a redundant key in the middle of it, and a second channel that is constant.
    Channel ramp = scalar_channel("intensity", 3);
    ramp.keys.clear();
    add_key(ramp, 0, 0.0F);
    add_key(ramp, 12, 0.5F);  // exactly on the line between its neighbours
    add_key(ramp, 24, 1.0F);
    source.tracks[0].sections[0].channels[0] = std::move(ramp);

    CompileOptions options;
    options.compression.remove_redundant_keys = true;
    options.compression.fold_constants = true;
    options.compression.tolerance = 0.001F;

    Compiled compiled(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(source, options, compiled.program, compiled.report));
    CY_CHECK_EQ(compiled.report.compression.keys_in, 3U);
    CY_CHECK_EQ(compiled.report.compression.keys_out, 2U);
    CY_CHECK_EQ(compiled.report.compression.keys_removed, 1U);
    // MEASURED, not assumed: the removed key was on the line, so the error is zero.
    CY_CHECK_LT(compiled.report.compression.max_error, 1e-6F);

    // And a curve that is NOT a straight line keeps its key, because the tolerance is real.
    SequenceSource curved = base_sequence();
    Channel bend = scalar_channel("intensity", 4);
    add_key(bend, 0, 0.0F);
    add_key(bend, 12, 0.9F);
    add_key(bend, 24, 1.0F);
    curved.tracks[0].sections[0].channels[0] = std::move(bend);
    Compiled second_compiled(allocator());
    SequenceCompiler second(allocator(), registry.registry);
    CY_REQUIRE(second.compile(curved, options, second_compiled.program, second_compiled.report));
    CY_CHECK_EQ(second_compiled.report.compression.keys_removed, 0U);
}
