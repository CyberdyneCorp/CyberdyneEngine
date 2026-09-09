// samples/08a-authoring — the half of M8.a's artefact that MEASURES. Section 6, tasks 6.4 to 6.6.
//
// ================================================================================================
// WHY THE ARTEFACT NEEDS A PROGRAM AS WELL AS A DRIVER, AND WHAT THIS ONE IS FOR
// ================================================================================================
//
// `authoring.py` drives the real editor and a real runtime: it creates the box and the sphere,
// places one above the other, adds a body to each, presses play, stops, and undoes back to an
// empty world. What that arrangement CANNOT do is say how far the sphere fell, because the only
// place a play session's simulated placement appears is inside the runtime's own copy of the
// authored world, and the runtime's report counts sessions and ticks rather than metres.
//
// This program is where the fall is measured. It plays THE SAME AUTHORED WORLD — not a copy of it
// and not a fixture — and it obtains that world the way the runtime does: an empty `.cyworld` plus
// the editor's own committed transactions. `editor-documents-and-transactions` requires that "the
// journal SHALL be the same operation stream used by diff, live editing, and any future
// collaboration, rather than a separate representation", so a second consumer replaying it is what
// that sentence is for, and `cy::scene::serialization::apply_transaction` is the engine's decoder
// for it — the same call `samples/05b-editor-window/runtime` makes on every live edit.
//
// It needs no display, no graphics device and no editor. That is deliberate: it is the half of this
// artefact a hosted runner can judge.
//
// ================================================================================================
// WHAT IT CLAIMS, AND HOW EACH CLAIM IS KEPT
// ================================================================================================
//
//   the sphere falls          the dynamic body's height is sampled every tick, and the run reports
//                             where it started, where it came to rest, and how long that took
//   it lands ON the box       the resting height is compared against the contact height the two
//                             colliders imply — the static box's top plus the sphere's radius —
//                             which is a number read out of the authored world rather than a
//                             constant written here. A sphere that fell through reports a resting
//                             height metres below it and the run records a gap.
//   stop restores exactly     the file's bytes at `stop()` are compared with its bytes at
//                             `enter()`, by this program, rather than by reading
//                             `PlayReport::restored_exactly` and believing it
//   and it is repeatable      the whole session runs TWICE over one world. A second run that
//                             produced a different resting height would mean the first left
//                             something behind, which is exactly what task 5.2 forbids
//
// ================================================================================================
// THE OUTPUT IS PARSED, SO IT IS A GRAMMAR
// ================================================================================================
//
// Every measured line is `08a-authoring: <key> = <value>`, one per line, and `authoring.py` reads
// them. Prose goes on lines that do not contain " = ". The rule samples/02-headless-sim set and
// every artefact since has kept: a driver that scraped free text would break on a reworded
// sentence.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/world_transaction.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy_reflect_generated_scene.h>

#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cy;
namespace ser = cy::scene::serialization;

constexpr const char* kTag = "08a-authoring";

/// The component names the editor writes and this program reads back. They are the contract
/// `src/gameplay/play/src/session.cpp` and `editor/crates/cy-editor-services/src/bodies.rs` already
/// hold from their own ends; naming them here as well is what lets this program say WHICH node is
/// the sphere without a name, a tag or an index written down somewhere.
constexpr std::string_view kRigidBody = "RigidBody";
constexpr std::string_view kStaticBody = "StaticBody";
constexpr std::string_view kCollider = "Collider";
constexpr std::string_view kFieldShape = "shape";
constexpr std::string_view kFieldExtent = "extent";
constexpr std::string_view kFieldRadius = "radius";

struct Options {
    const char* world = "";
    const char* asset = "worlds/authored.cyworld";
    const char* journal = "";
    const char* write = "";
    const char* physics = "jolt";
    u64 ticks = 600;
    u32 rate = 60;
    f64 settle = 0.0005;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/08a-authoring — the authored world, played, measured and put back.\n"
        "\n"
        "  --world <path>      the .cyworld to play                     (required)\n"
        "  --asset <path>      the asset path the editor opened it by   "
        "(default worlds/authored.cyworld)\n"
        "  --journal <path>    a .cyjournal whose transactions are replayed over it first\n"
        "  --write <path>      write the world out after the replay, before play\n"
        "  --physics <name>    jolt or reference                        (default jolt)\n"
        "  --ticks <n>         the most fixed steps one session runs    (default 600)\n"
        "  --rate <hz>         the simulation rate                      (default 60)\n"
        "  --settle <metres>   movement per tick below which the body is at rest (default 0.0005)\n"
        "  --help              this\n",
        stdout);
}

[[nodiscard]] const char* value_of(int argc, char** argv, const char* key,
                                   const char* fallback) noexcept {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], key) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

[[nodiscard]] Options parse(int argc, char** argv) noexcept {
    Options options;
    options.world = value_of(argc, argv, "--world", options.world);
    options.asset = value_of(argc, argv, "--asset", options.asset);
    options.journal = value_of(argc, argv, "--journal", options.journal);
    options.write = value_of(argc, argv, "--write", options.write);
    options.physics = value_of(argc, argv, "--physics", options.physics);
    options.ticks = std::strtoull(value_of(argc, argv, "--ticks", "600"), nullptr, 10);
    options.rate =
        static_cast<u32>(std::strtoul(value_of(argc, argv, "--rate", "60"), nullptr, 10));
    options.settle = std::strtod(value_of(argc, argv, "--settle", "0.0005"), nullptr);
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--help") == 0) {
            options.help = true;
        }
    }
    return options;
}

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// The whole of a file, in one read.
///
/// Sized first and read once rather than looped over in blocks: a loop that calls `fread` after the
/// stream has reached its end leaves the position indeterminate, which the static analyser is right
/// about even though the loop would work.
[[nodiscard]] bool read_file(const char* path, std::string& out) noexcept {
    std::FILE* handle = std::fopen(path, "rb");
    if (handle == nullptr) {
        return false;
    }
    bool ok = std::fseek(handle, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(handle) : -1L;
    ok = ok && size >= 0 && std::fseek(handle, 0, SEEK_SET) == 0;
    if (ok && size > 0) {
        out.resize(static_cast<usize>(size));
        ok =
            std::fread(out.data(), 1, static_cast<usize>(size), handle) == static_cast<usize>(size);
    }
    return std::fclose(handle) == 0 && ok;
}

[[nodiscard]] bool write_file(const char* path, std::string_view text) noexcept {
    std::FILE* handle = std::fopen(path, "wb");
    if (handle == nullptr) {
        return false;
    }
    const bool wrote = std::fwrite(text.data(), 1, text.size(), handle) == text.size();
    return std::fclose(handle) == 0 && wrote;
}

// --- The editor's journal ------------------------------------------------------------------------
//
// `editor/crates/cy-editor-documents/src/journal.rs` is the other half of this: eight magic bytes,
// a little-endian format version, and then records of `u32 length, u32 FNV-1a-32 of the payload,
// payload`. The checksum is verified rather than skipped, because the whole point of the framing is
// that a file which stopped mid-write is DETECTED — a replay that decoded a half-written tail would
// report a world nobody authored.

constexpr std::string_view kJournalMagic{"CYJRNL\x00\x01", 8};
constexpr u32 kJournalVersion = 1;

[[nodiscard]] u32 journal_checksum(const u8* bytes, usize count) noexcept {
    u32 hash = 0x811C'9DC5U;
    for (usize index = 0; index < count; ++index) {
        hash ^= static_cast<u32>(bytes[index]);
        hash *= 0x0100'0193U;
    }
    return hash;
}

[[nodiscard]] u32 read_u32(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8U) |
           (static_cast<u32>(bytes[2]) << 16U) | (static_cast<u32>(bytes[3]) << 24U);
}

/// Split a journal file into the transactions that check out, in order.
///
/// Returns false only when the file is not a journal this build can read at all. A truncated tail
/// is reported through `truncated` and everything before it is returned, which is the same rule the
/// editor's own recovery applies.
[[nodiscard]] bool split_journal(std::string_view file, std::vector<std::string_view>& out,
                                 bool& truncated) noexcept {
    truncated = false;
    if (file.size() < kJournalMagic.size() + 4 || !file.starts_with(kJournalMagic)) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const u8*>(file.data());
    if (read_u32(bytes + kJournalMagic.size()) != kJournalVersion) {
        return false;
    }
    usize at = kJournalMagic.size() + 4;
    while (at < file.size()) {
        if (file.size() - at < 8) {
            truncated = true;
            return true;
        }
        const usize length = read_u32(bytes + at);
        const u32 expected = read_u32(bytes + at + 4);
        at += 8;
        if (file.size() - at < length) {
            truncated = true;
            return true;
        }
        if (journal_checksum(bytes + at, length) != expected) {
            truncated = true;
            return true;
        }
        out.emplace_back(file.data() + at, length);
        at += length;
    }
    return true;
}

// --- Reading the authored world ------------------------------------------------------------------

/// Everything the artefact needs to know about one authored node's physics.
struct Body {
    u32 index = 0;
    u64 identity = 0;
    bool found = false;
    /// `Collider.shape`, as the file spells it.
    std::string shape;
    /// A box's half extents and a sphere's radius, whichever the shape declares.
    f32 half_height = 0.0F;
    f32 radius = 0.0F;
};

[[nodiscard]] const ser::WorldTypeDecl* type_named(const ser::World& world,
                                                   std::string_view name) noexcept {
    for (const ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) == name) {
            return &declared;
        }
    }
    return nullptr;
}

[[nodiscard]] const ser::WorldField* field_named(const ser::World& world,
                                                 const ser::WorldTypeDecl& declared,
                                                 const ser::WorldComponent& component,
                                                 std::string_view name) noexcept {
    for (const ser::WorldFieldDecl& candidate : declared.fields()) {
        if (world.text(candidate.name) == name) {
            return component.find(candidate.file_field);
        }
    }
    return nullptr;
}

/// Read a node's collider geometry into `body`, from the file's own `Collider` declaration.
void read_collider(const ser::World& world, const ser::WorldNode& node, Body& body) noexcept {
    const ser::WorldTypeDecl* declared = type_named(world, kCollider);
    if (declared == nullptr) {
        return;
    }
    const ser::WorldComponent* component = node.find(declared->file_type);
    if (component == nullptr) {
        return;
    }
    if (const ser::WorldField* shape = field_named(world, *declared, *component, kFieldShape)) {
        const Span<const u8> bytes = world.blob(shape->value);
        body.shape.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    if (const ser::WorldField* extent = field_named(world, *declared, *component, kFieldExtent)) {
        body.half_height = extent->value.lanes[1];
    }
    if (const ser::WorldField* radius = field_named(world, *declared, *component, kFieldRadius)) {
        body.radius = radius->value.lanes[0];
    }
}

/// The one live node carrying `component`, and its collider. Refuses to guess when there are two.
[[nodiscard]] Body body_carrying(const ser::World& world, std::string_view component) noexcept {
    Body body;
    const ser::WorldTypeDecl* declared = type_named(world, component);
    if (declared == nullptr) {
        return body;
    }
    u32 index = 0;
    for (const ser::WorldNode& node : world.nodes()) {
        if (node.live && node.find(declared->file_type) != nullptr) {
            if (body.found) {
                body.found = false;  // two of them: this artefact's world has exactly one of each
                return body;
            }
            body.index = index;
            body.identity = node.identity;
            body.found = true;
            read_collider(world, node, body);
        }
        index += 1;
    }
    return body;
}

[[nodiscard]] f32 height_of(const ser::World& world, u32 index) noexcept {
    Transform placement;
    if (index >= world.nodes().size() ||
        !ser::transform_of(world, world.nodes()[index], placement)) {
        return 0.0F;
    }
    return placement.translation.y;
}

[[nodiscard]] std::string bytes_of(const ser::World& world) noexcept {
    Array<char> out(allocator());
    if (!ser::write_world(world, out)) {
        return {};
    }
    return {out.data(), out.size()};
}

// --- The solver
// ------------------------------------------------------------------------------------

/// A physics server, chosen the way `samples/05b-editor-window/runtime` chooses one.
///
/// **Jolt where the build has it**, because the reference backend declares that it does not resolve
/// contacts, so a sphere dropped on a box goes through it. `--physics reference` asks for that
/// deliberately, and the run then reports the sphere as having fallen through — which is the honest
/// answer for a backend that says it does not do this, not a defect in the bridge.
class Solver {
public:
    explicit Solver(const char* wanted) noexcept {
        const bool ask_reference = std::strcmp(wanted, "reference") == 0;
#if defined(CY_PHYSICS)
        if (!ask_reference) {
            const Expected<physics::PhysicsServer*, Error> made =
                physics::jolt::create_server(allocator(), nullptr);
            if (made && (*made)->initialize()) {
                server_ = *made;
                chosen_ = "jolt";
                return;
            }
            if (made) {
                physics::jolt::destroy_server(*made, allocator());
            }
        }
#else
        (void)ask_reference;
#endif
        const Expected<physics::PhysicsServer*, Error> made =
            physics::reference::create_server(allocator());
        if (made && (*made)->initialize()) {
            server_ = *made;
            chosen_ = "reference";
            return;
        }
        if (made) {
            physics::reference::destroy_server(*made, allocator());
        }
    }

    ~Solver() {
        if (server_ == nullptr) {
            return;
        }
        server_->shutdown();
#if defined(CY_PHYSICS)
        if (std::strcmp(chosen_, "jolt") == 0) {
            physics::jolt::destroy_server(server_, allocator());
            return;
        }
#endif
        physics::reference::destroy_server(server_, allocator());
    }

    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;
    Solver(Solver&&) = delete;
    Solver& operator=(Solver&&) = delete;

    [[nodiscard]] physics::PhysicsServer* server() const noexcept { return server_; }
    [[nodiscard]] const char* chosen() const noexcept { return chosen_; }

private:
    physics::PhysicsServer* server_ = nullptr;
    const char* chosen_ = "none";
};

/// What one play session did to the sphere.
struct Fall {
    bool ran = false;
    f32 start = 0.0F;
    f32 rest = 0.0F;
    f32 lowest = 0.0F;
    u64 ticks = 0;
    u64 ticks_to_rest = 0;
    u32 bodies = 0;
    u32 entities = 0;
    /// This program's own comparison of the file's bytes before and after.
    bool restored_exactly = false;
    /// What the session CLAIMED, which is a different statement and is cross-checked against the
    /// line above: a report that graded its own homework is the defect M6's gate found four of.
    bool session_claimed_exact = false;
    /// Set when the session's own comparison failed and it re-read its snapshot over the world.
    bool total_restore = false;
    u64 restored_difference = 0;
};

/// Run one session over `world`, sampling the sphere every tick.
///
/// `before` is the world's bytes at entry, compared here rather than in the caller so that the
/// comparison happens while the session is the only thing that could have changed them.
[[nodiscard]] Fall play_once(ser::World& world, const ser::AuthoringSchema& schema,
                             physics::PhysicsServer* server, const Body& sphere,
                             const Options& options) noexcept {
    Fall fall;
    const std::string before = bytes_of(world);

    gameplay::PlaySession session(allocator(), world);
    gameplay::PlayConfiguration configuration;
    configuration.physics = server;
    configuration.schema = &schema;
    configuration.body_capacity = 64;
    configuration.rate = determinism::TickRate{options.rate, 1};
    if (!session.enter(configuration)) {
        return fall;
    }
    fall.ran = true;
    fall.entities = session.report().entities;
    fall.bodies = session.report().bodies;
    fall.start = height_of(world, sphere.index);
    fall.lowest = fall.start;

    f32 previous = fall.start;
    u64 still = 0;
    for (u64 tick = 0; tick < options.ticks; ++tick) {
        if (!session.tick()) {
            break;
        }
        fall.ticks = tick + 1;
        const f32 now = height_of(world, sphere.index);
        fall.lowest = (now < fall.lowest) ? now : fall.lowest;
        const f32 moved = (now > previous) ? now - previous : previous - now;
        previous = now;
        // At rest for a tenth of a second of simulated time, so that a body passing through the
        // top of its bounce is not mistaken for one that has stopped.
        still = (moved < static_cast<f32>(options.settle)) ? still + 1 : 0;
        if (still * 10 >= static_cast<u64>(options.rate)) {
            fall.ticks_to_rest = fall.ticks;
            break;
        }
    }
    fall.rest = height_of(world, sphere.index);

    if (!session.stop()) {
        return fall;
    }
    const std::string after = bytes_of(world);
    fall.restored_exactly = !before.empty() && before == after;
    fall.session_claimed_exact = session.report().restored_exactly;
    fall.total_restore = session.report().total_restore;
    // ZERO WHEN THEY MATCHED, rather than what the report holds. `PlayReport::restored_difference`
    // is documented as "the first byte at which they differed, when they did", and on a session
    // that restored exactly it comes back as the file's LENGTH — the offset the scan reached. A
    // number that reads as "they differ at the very end" beside "restored exactly: yes" is a
    // contradiction a reader has to resolve, so this reports the field only when it means
    // something. src/gameplay/play/src/session.cpp is where it would be cleared.
    fall.restored_difference = fall.restored_exactly ? 0 : session.report().restored_difference;
    return fall;
}

void report_number(const char* key, f64 value) noexcept {
    std::printf("%s: %s = %.4f\n", kTag, key, value);
}

void report_count(const char* key, unsigned long long value) noexcept {
    std::printf("%s: %s = %llu\n", kTag, key, value);
}

void report_text(const char* key, const char* value) noexcept {
    std::printf("%s: %s = %s\n", kTag, key, value);
}

/// Read the world, replay the journal over it, and resolve it. Returns false having said why.
[[nodiscard]] bool load_authored(const Options& options, ser::World& world,
                                 const ser::AuthoringSchema& schema, std::string& text) noexcept {
    if (!read_file(options.world, text)) {
        std::printf("%s: cannot read the world at %s\n", kTag, options.world);
        return false;
    }
    const Expected<ser::WorldReadReport, Error> read = ser::read_world(text, options.asset, world);
    if (!read) {
        std::printf("%s: %s is not a world this build can read: %s\n", kTag, options.world,
                    read.error().message);
        return false;
    }
    if (!ser::resolve_against(world, schema)) {
        std::printf("%s: the world could not be resolved against this build's schema\n", kTag);
        return false;
    }
    report_count("nodes-in-file", world.nodes().size());
    report_count("types-in-file", world.types().size());

    if (options.journal[0] == '\0') {
        report_count("journal-transactions", 0);
        return true;
    }

    std::string journal;
    if (!read_file(options.journal, journal)) {
        std::printf("%s: cannot read the journal at %s\n", kTag, options.journal);
        return false;
    }
    std::vector<std::string_view> records;
    bool truncated = false;
    if (!split_journal(journal, records, truncated)) {
        std::printf("%s: %s is not a journal this build can read\n", kTag, options.journal);
        return false;
    }
    u32 applied = 0;
    u32 created = 0;
    for (const std::string_view record : records) {
        ser::TransactionReport report;
        const Span<const u8> bytes{reinterpret_cast<const u8*>(record.data()), record.size()};
        if (!ser::apply_transaction(world, bytes, report)) {
            std::printf("%s: a journalled transaction was refused\n", kTag);
            return false;
        }
        applied += report.applied;
        created += report.created;
    }
    report_count("journal-transactions", records.size());
    report_count("journal-operations-applied", applied);
    report_count("journal-nodes-created", created);
    report_text("journal-truncated", truncated ? "yes" : "no");
    report_count("nodes-after-replay", world.nodes().size());
    return true;
}

/// Where a sphere resting on a box has its centre, from the two colliders the world declares.
[[nodiscard]] f32 contact_height(const ser::World& world, const Body& box,
                                 const Body& sphere) noexcept {
    return height_of(world, box.index) + box.half_height + sphere.radius;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    if (options.help || options.world[0] == '\0') {
        print_usage();
        return options.help ? 0 : 2;
    }

    reflect::TypeRegistry registry;
    ser::AuthoringSchema schema(allocator());
    if (!reflect::register_scene_types(registry) ||
        !ser::build_authoring_schema(registry, schema)) {
        std::printf("%s: the engine's own scene schema could not be built\n", kTag);
        return 1;
    }

    ser::World world(allocator());
    std::string text;
    if (!load_authored(options, world, schema, text)) {
        return 1;
    }

    if (options.write[0] != '\0' && !write_file(options.write, bytes_of(world))) {
        std::printf("%s: cannot write the authored world to %s\n", kTag, options.write);
        return 1;
    }

    const Body sphere = body_carrying(world, kRigidBody);
    const Body box = body_carrying(world, kStaticBody);
    if (!sphere.found || !box.found) {
        std::printf("%s: this world does not hold exactly one %.*s and one %.*s\n", kTag,
                    static_cast<int>(kRigidBody.size()), kRigidBody.data(),
                    static_cast<int>(kStaticBody.size()), kStaticBody.data());
        return 1;
    }
    report_text("dynamic-collider", sphere.shape.c_str());
    report_text("static-collider", box.shape.c_str());
    report_number("contact-height", contact_height(world, box, sphere));

    Solver solver(options.physics);
    if (solver.server() == nullptr) {
        std::printf("%s: no physics backend could be started\n", kTag);
        return 1;
    }
    report_text("physics", solver.chosen());

    // TWICE OVER ONE WORLD, and the second run is the claim rather than a repetition: a session
    // that left a body, a velocity or a nudged placement behind would make the second fall start
    // somewhere else or end somewhere else, and the two heights below would differ.
    const Fall first = play_once(world, schema, solver.server(), sphere, options);
    if (!first.ran) {
        std::printf("%s: the world would not enter play\n", kTag);
        return 1;
    }
    const Fall second = play_once(world, schema, solver.server(), sphere, options);

    report_count("entities", first.entities);
    report_count("bodies", first.bodies);
    report_number("start-height", first.start);
    report_number("rest-height", first.rest);
    report_number("lowest-height", first.lowest);
    report_count("ticks", first.ticks);
    report_count("ticks-to-rest", first.ticks_to_rest);
    report_number("fell", first.start - first.rest);
    report_text("restored-exactly", first.restored_exactly ? "yes" : "no");
    report_text("session-claimed-exact", first.session_claimed_exact ? "yes" : "no");
    report_text("total-restore", first.total_restore ? "yes" : "no");
    report_count("restored-difference", first.restored_difference);
    report_number("second-rest-height", second.rest);
    report_count("second-ticks-to-rest", second.ticks_to_rest);
    report_text("second-restored-exactly", second.restored_exactly ? "yes" : "no");
    report_number("height-after-stop", height_of(world, sphere.index));
    return 0;
}
