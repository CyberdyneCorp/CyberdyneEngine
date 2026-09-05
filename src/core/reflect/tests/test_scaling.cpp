// THE COMPLEXITY CONTRACT, MEASURED. M2 tasks 1.1 and 1.2.
//
// The M2 spec delta to `core-type-system` adds a complexity clause to a requirement that until now
// was only about *where* reflection may be called from:
//
//     Lookup complexity is part of the contract. Type and field lookup SHALL NOT be linear in the
//     number of registered types or fields. A record decode SHALL be linear in the size of the
//     record, not in the product of its field count and the type's field count.
//
//     The reflected path that asset loading and scene instantiation take SHALL be measured against
//     a type set representative of a real project, and the measurement recorded, rather than
//     assumed to be off the hot path because the specification says so.
//
// This file is both halves. It carries M1's implementations — a linear registry scan and a
// find_field() per field per record — as **reference implementations**, exactly as
// src/core/math/ carries a scalar reference for its SIMD paths, and measures the shipping path
// against them on the same corpus in the same process. That is what makes the numbers in
// src/core/reflect/README.md reproducible by anybody who runs the suite rather than a claim about
// one machine on one afternoon.
//
// TWO KINDS OF ASSERTION, AND WHY BOTH.
//
//   Structural — `longest_probe()` on the registry and on a FieldIndex is the longest chain any
//   successful lookup can walk. A linear scan's worst case is the entry count; a table sized for a
//   load factor of one half has a worst case that does not move when the corpus grows by two orders
//   of magnitude. That assertion is deterministic: it holds on a loaded build machine, under a
//   sanitizer, and in every profile, because it is a property of the data structure rather than of
//   the clock.
//
//   Comparative — the timed assertions compare the shipping path against the reference *on the same
//   corpus in the same run*, and compare ratios rather than absolute times. A machine that is twice
//   as slow is twice as slow at both. The bounds are deliberately far looser than the measured
//   margins (see the recorded numbers): this suite must fail when the complexity regresses, not
//   when the build machine is busy.
//
// THE CORPUS IS SYNTHETIC, AND THAT IS CORRECT. A TypeInfo is plain data — the generator emits
// exactly this, as constexpr — so a descriptor built here is the same input to the registry and the
// serializer as a generated one. Reflecting five hundred real types would need five hundred entries
// in identity/manifest.toml, which would make this test's corpus part of the engine's permanent
// identity space to measure a property that has nothing to do with identity.

#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/reflect/registry.h>
#include <cy/core/reflect/serialize.h>
#include <cy/core/reflect/type_info.h>

#include <cy/test/test.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using cy::f64;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::reflect::FieldId;
using cy::reflect::FieldIndex;
using cy::reflect::FieldInfo;
using cy::reflect::FieldKind;
using cy::reflect::TypeId;
using cy::reflect::TypeInfo;
using cy::reflect::TypeRegistry;

// --- The corpus
// -------------------------------------------------------------------------------------

/// A reflected type set of a size a real project reaches, owned by the test.
///
/// Identifiers are assigned the way the manifest assigns them — a counter from one, never derived
/// from a name — so the index sees the same key distribution it will see in production. That
/// matters: consecutive counters are the case a hash function is most likely to cluster on, which
/// is why the table hashes them rather than masking them.
class TypeCorpus {
public:
    TypeCorpus(u32 type_count, u32 narrowest, u32 widest) {
        names_.reserve(type_count);
        field_names_.reserve(static_cast<usize>(widest));
        for (u32 index = 0; index < widest; ++index) {
            field_names_.push_back("field_" + std::to_string(index));
        }

        types_.reserve(type_count);
        fields_.reserve(type_count);
        for (u32 index = 0; index < type_count; ++index) {
            const u32 field_count = narrowest + (index % (widest - narrowest + 1));
            names_.push_back("cy::corpus::Type" + std::to_string(index));

            std::vector<FieldInfo> fields;
            fields.reserve(field_count);
            for (u32 field = 0; field < field_count; ++field) {
                FieldInfo info;
                info.name = field_names_[field].c_str();
                info.id = FieldId{field + 1};
                info.kind = FieldKind::U32;
                info.offset = field * 4u;
                info.size = 4;
                fields.push_back(info);
            }
            fields_.push_back(std::move(fields));

            TypeInfo type;
            type.name = names_[index].c_str();
            type.id = TypeId{index + 1};
            type.size = field_count * 4u;
            type.alignment = 4;
            type.trivially_relocatable = true;
            type.field_count = field_count;
            type.header = "tests/test_scaling.cpp";
            type.module = "core-reflect";
            types_.push_back(type);
        }
        // The field pointers are bound after both vectors have stopped growing: a pointer into a
        // vector that later reallocates is the classic way a corpus builder invalidates itself.
        for (u32 index = 0; index < type_count; ++index) {
            types_[index].fields = fields_[index].data();
        }
    }

    [[nodiscard]] const TypeInfo& type(u32 index) const { return types_[index]; }
    [[nodiscard]] u32 size() const { return static_cast<u32>(types_.size()); }

    [[nodiscard]] cy::Status register_all(TypeRegistry& registry) const {
        for (const TypeInfo& type : types_) {
            if (auto added = registry.add(type); !added) {
                return added;
            }
        }
        return cy::ok();
    }

private:
    std::vector<std::string> names_;
    std::vector<std::string> field_names_;
    std::vector<std::vector<FieldInfo>> fields_;
    std::vector<TypeInfo> types_;
};

/// A reproducible spread, so a failure is reproducible from the test name alone. An LCG rather than
/// cy::Random because this module sits below core-math and must not link it to measure itself.
class Sequence {
public:
    explicit Sequence(u64 seed) noexcept : state_(seed) {}
    u32 next(u32 bound) noexcept {
        state_ = (state_ * 6364136223846793005ULL) + 1442695040888963407ULL;
        return static_cast<u32>((state_ >> 33u) % bound);
    }

private:
    u64 state_;
};

// --- M1's implementations, kept as the reference
// ------------------------------------------------------

/// What TypeRegistry::find(TypeId) was at M1: a scan of the entry table.
const TypeInfo* scanning_find(const TypeRegistry& registry, TypeId wanted) noexcept {
    for (const TypeInfo* entry : registry) {
        if (entry->id == wanted) {
            return entry;
        }
    }
    return nullptr;
}

/// What read_record() was at M1: TypeInfo::find_field() — itself a scan — once per field in the
/// record. The record format is the one serialize.h documents, decoded here rather than called
/// through, so that this reference cannot quietly become the implementation it is measuring.
cy::Status scanning_read_record(const TypeInfo& type, const u8* data, usize size, void* object) {
    auto header = cy::reflect::peek_record(data, size);
    if (!header) {
        return cy::Unexpected<cy::Error>(header.error());
    }
    const u8* payload = data + cy::reflect::RecordHeader::header_size;
    usize position = 0;
    for (u32 index = 0; index < static_cast<u32>(header->field_count); ++index) {
        const u8* entry = payload + position;
        const FieldId id{static_cast<u32>(entry[0]) | (static_cast<u32>(entry[1]) << 8u) |
                         (static_cast<u32>(entry[2]) << 16u) | (static_cast<u32>(entry[3]) << 24u)};
        const usize width = static_cast<usize>(entry[6]) | (static_cast<usize>(entry[7]) << 8u);
        position += 8;
        if (const FieldInfo* field = type.find_field(id); field != nullptr) {
            std::memcpy(static_cast<u8*>(object) + field->offset, payload + position, width);
        }
        position += width;
    }
    return cy::ok();
}

// --- Measurement
// ------------------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

f64 elapsed_ns(Clock::time_point started) {
    return static_cast<f64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

/// One line of the recorded measurement, in the shape README.md carries.
std::string report(const char* what, f64 reference_ns, f64 indexed_ns, u64 operations) {
    char line[256];
    std::snprintf(line, sizeof(line), "%-34s scan %9.1f ns/op   indexed %7.1f ns/op   %5.1fx", what,
                  reference_ns / static_cast<f64>(operations),
                  indexed_ns / static_cast<f64>(operations), reference_ns / indexed_ns);
    return {line};
}

/// Encode every type in the corpus once, so the decode measurements read real records.
std::vector<cy::reflect::ByteBuffer> encode_one_of_each(const TypeCorpus& corpus) {
    std::vector<cy::reflect::ByteBuffer> records(corpus.size());
    std::vector<u8> object(4096, 0x5A);
    for (u32 index = 0; index < corpus.size(); ++index) {
        const cy::Status written =
            cy::reflect::write_record(corpus.type(index), object.data(), records[index]);
        CY_REQUIRE(static_cast<bool>(written));
    }
    return records;
}

}  // namespace

// --- Scenario: lookup does not degrade with the registry
// -----------------------------------------------

CY_TEST_CASE("reflection scaling: type lookup does not degrade as the registry grows") {
    // "WHEN the number of registered types grows by an order of magnitude THEN type and field
    // lookup cost SHALL be substantially unchanged." Two orders of magnitude, to leave no doubt.
    const u32 sizes[] = {32, 320, 3200};
    f64 scan_ns[3] = {};
    f64 index_ns[3] = {};
    u32 probes[3] = {};

    for (u32 step = 0; step < 3; ++step) {
        TypeCorpus corpus(sizes[step], 4, 24);
        TypeRegistry registry;
        CY_REQUIRE(static_cast<bool>(corpus.register_all(registry)));
        CY_REQUIRE_EQ(registry.size(), sizes[step]);
        probes[step] = registry.longest_probe();

        // The same identifiers in the same order for every size, so the three measurements differ
        // only in how much data the lookup has to see past.
        constexpr u32 lookups = 20000;
        Sequence sequence(0xC0FFEEULL);
        std::vector<TypeId> wanted(lookups);
        for (u32 index = 0; index < lookups; ++index) {
            wanted[index] = TypeId{sequence.next(sizes[step]) + 1};
        }

        usize found = 0;
        Clock::time_point started = Clock::now();
        for (const TypeId id : wanted) {
            found += (scanning_find(registry, id) != nullptr) ? 1u : 0u;
        }
        scan_ns[step] = elapsed_ns(started);

        started = Clock::now();
        for (const TypeId id : wanted) {
            found += (registry.find(id) != nullptr) ? 1u : 0u;
        }
        index_ns[step] = elapsed_ns(started);
        CY_CHECK_EQ(found, static_cast<usize>(lookups) * 2u);

        // Built into a local first: CY_TEST_MESSAGE streams its argument, and `<<` binds tighter
        // than `+`, so a concatenation written inline would try to append to the stream builder.
        const std::string label = "registry find, " + std::to_string(sizes[step]) + " types";
        const std::string line = report(label.c_str(), scan_ns[step], index_ns[step], lookups);
        CY_TEST_MESSAGE(line);
    }

    // The structural assertion, which owes nothing to the clock: a table at a load factor of one
    // half has a bounded worst-case chain, and it does not grow with the corpus.
    const std::string chains = "longest probe chain: " + std::to_string(probes[0]) + ", " +
                               std::to_string(probes[1]) + ", " + std::to_string(probes[2]);
    CY_TEST_MESSAGE(chains);
    //
    // Linear probing at a load factor of one half has an expected *longest* chain of O(log n) and
    // an expected chain of O(1). So the claim is not that the longest chain never moves — it does,
    // and the message above records it — but that it stays a small constant where a scan's worst
    // case is the entry count itself. The second assertion is the sharp one: the worst chain over
    // three thousand types is shorter than a scan of the *smallest* corpus in this test.
    CY_CHECK_LE(probes[2], 32u);
    CY_CHECK_LT(probes[2], sizes[0]);

    // The timed assertion, as a ratio of ratios. A hundredfold corpus must not cost the indexed
    // path anything like a hundredfold, and the scan — which is the thing being replaced — must
    // show the degradation this test would otherwise be unable to detect.
    const f64 index_growth = index_ns[2] / index_ns[0];
    const f64 scan_growth = scan_ns[2] / scan_ns[0];
    const std::string growth = "growth over 100x more types - scan " + std::to_string(scan_growth) +
                               "x, indexed " + std::to_string(index_growth) + "x";
    CY_TEST_MESSAGE(growth);
    CY_CHECK_LT(index_growth, 8.0);
    CY_CHECK_GT(scan_growth, index_growth * 4.0);
}

// --- Scenario: decode is linear in the record
// ----------------------------------------------------------

CY_TEST_CASE("reflection scaling: field lookup does not degrade as a type widens") {
    const u32 widths[] = {4, 40, 400};
    for (const u32 width : widths) {
        TypeCorpus corpus(1, width, width);
        FieldIndex index;
        CY_REQUIRE(static_cast<bool>(index.build(corpus.type(0))));
        CY_CHECK_LE(index.longest_probe(), 16u);
        const std::string line = "fields " + std::to_string(width) + ": longest probe " +
                                 std::to_string(index.longest_probe());
        CY_TEST_MESSAGE(line);

        // Every field is found, and found at the descriptor the scan would have returned. A faster
        // lookup that answers differently is not a faster lookup.
        for (u32 field = 0; field < width; ++field) {
            const FieldId id{field + 1};
            CY_REQUIRE_EQ(index.find(id), corpus.type(0).find_field(id));
            CY_REQUIRE_EQ(index.find(corpus.type(0).fields[field].name),
                          corpus.type(0).find_field(corpus.type(0).fields[field].name));
        }
        // And a field the type does not have is absent rather than a wrong answer or a wrap.
        CY_CHECK_EQ(index.find(FieldId{width + 1}), nullptr);
        CY_CHECK_EQ(index.find("not_a_field"), nullptr);
    }
}

CY_TEST_CASE("reflection scaling: a record decode is linear in the record") {
    // "WHEN a record carrying many fields is decoded THEN the cost SHALL be linear in the record's
    // size rather than quadratic in its field count."
    constexpr u32 width = 256;
    constexpr u32 records = 2000;
    TypeCorpus corpus(1, width, width);
    const TypeInfo& type = corpus.type(0);

    std::vector<u8> source(type.size);
    for (u32 index = 0; index < type.size; ++index) {
        source[index] = static_cast<u8>(index * 7u);
    }
    cy::reflect::ByteBuffer record;
    CY_REQUIRE(static_cast<bool>(cy::reflect::write_record(type, source.data(), record)));

    FieldIndex fields;
    CY_REQUIRE(static_cast<bool>(fields.build(type)));

    std::vector<u8> scanned(type.size, 0);
    std::vector<u8> indexed(type.size, 0);

    // Nothing is asserted inside a timed loop. A doctest assertion costs more than the decode it
    // would be checking, so an assertion per iteration would measure doctest on both sides and
    // hide the difference under it — which it did, at a cost of one confusing afternoon.
    bool decoded = true;

    Clock::time_point started = Clock::now();
    for (u32 index = 0; index < records; ++index) {
        decoded = decoded && static_cast<bool>(scanning_read_record(type, record.data(),
                                                                    record.size(), scanned.data()));
    }
    const f64 scan_ns = elapsed_ns(started);

    started = Clock::now();
    for (u32 index = 0; index < records; ++index) {
        decoded = decoded && static_cast<bool>(cy::reflect::read_record(
                                 fields, record.data(), record.size(), indexed.data()));
    }
    const f64 index_ns = elapsed_ns(started);
    CY_REQUIRE(decoded);

    // Same answer first. The measurement is only interesting if both paths decoded the record.
    CY_REQUIRE_EQ(std::memcmp(scanned.data(), indexed.data(), type.size), 0);
    CY_REQUIRE_EQ(std::memcmp(scanned.data(), source.data(), type.size), 0);

    const std::string line = report("decode, 256-field record", scan_ns, index_ns, records);
    CY_TEST_MESSAGE(line);
    // The quadratic path does 256 x 128 comparisons per record and the linear one does 256 probes.
    // Four is a floor far below the measured margin: this fails when the complexity comes back, not
    // when the machine is busy.
    CY_CHECK_GT(scan_ns, index_ns * 4.0);
}

// --- The recorded measurement: what a scene load costs
// -------------------------------------------------

CY_TEST_CASE("reflection scaling: the reflected path a scene load takes") {
    // Task 1.2. A scene load is: for each record in the stream, resolve its TypeId to a type, then
    // apply the record to an object. Both halves went through a scan at M1, and the type set is
    // what decides how much that cost.
    //
    // Representative rather than maximal: 512 component types is the order a real project reaches
    // by the time it ships, and 4 to 48 fields spans a tag component and a character-settings
    // struct. 20 000 records is a populated level's worth of entities.
    constexpr u32 type_count = 512;
    constexpr u32 record_count = 20000;

    TypeCorpus corpus(type_count, 4, 48);
    TypeRegistry registry;
    CY_REQUIRE(static_cast<bool>(corpus.register_all(registry)));
    const std::vector<cy::reflect::ByteBuffer> encoded = encode_one_of_each(corpus);

    Sequence sequence(0x5EED5EEDULL);
    std::vector<u32> stream(record_count);
    for (u32 index = 0; index < record_count; ++index) {
        stream[index] = sequence.next(type_count);
    }

    std::vector<u8> object(4096, 0);

    // Nothing is asserted inside a timed loop: a doctest assertion costs more than the work it
    // would be checking, and asserting per record measures doctest rather than the decode.
    bool loaded = true;

    // THE BEST OF THREE, NOT ONE RUN OF EACH. A ratio between two timings taken once is a
    // measurement of whichever run was descheduled, and this assertion failed `just test-all
    // --profile dev` about one round in five at M3's gate — inside `four-profiles`, which every
    // milestone ledger runs, so a flake here is a flake in the whole ladder. The minimum of a few
    // repetitions is the standard answer: contention can only ever make a run slower, so the
    // smallest is the closest to the cost being compared, and it costs three passes over a corpus
    // that already fits the integration budget several times over.
    constexpr u32 kRepeats = 3;
    f64 scan_ns = 0.0;
    f64 index_ns = 0.0;
    for (u32 repeat = 0; repeat < kRepeats; ++repeat) {
        // The M1 path: a scan for the type, then find_field() per field of every record.
        Clock::time_point started = Clock::now();
        for (const u32 which : stream) {
            const TypeInfo* type = scanning_find(registry, TypeId{which + 1});
            loaded = loaded && type != nullptr &&
                     static_cast<bool>(scanning_read_record(*type, encoded[which].data(),
                                                            encoded[which].size(), object.data()));
        }
        const f64 scan_run = elapsed_ns(started);

        // The M2 path: an indexed lookup that hands back the FieldIndex the registry built at
        // registration, and a decode that probes it once per field in the record.
        started = Clock::now();
        for (const u32 which : stream) {
            const FieldIndex* fields = registry.fields(TypeId{which + 1});
            loaded = loaded && fields != nullptr &&
                     static_cast<bool>(cy::reflect::read_record(
                         *fields, encoded[which].data(), encoded[which].size(), object.data()));
        }
        const f64 index_run = elapsed_ns(started);

        if (repeat == 0 || scan_run < scan_ns) {
            scan_ns = scan_run;
        }
        if (repeat == 0 || index_run < index_ns) {
            index_ns = index_run;
        }
    }
    CY_REQUIRE(loaded);

    const std::string line = report("scene load, 512 types", scan_ns, index_ns, record_count);
    CY_TEST_MESSAGE(line);
    const std::string totals = "total: scan " + std::to_string(scan_ns / 1e6) + " ms, indexed " +
                               std::to_string(index_ns / 1e6) + " ms";
    CY_TEST_MESSAGE(totals);
    // Measured at 2.2x end to end on this corpus, and the modest factor is worth understanding
    // rather than inflating: a 26-field record's decode is real work — a probe and a memcpy per
    // field — and the quadratic term is only the part *around* it. The two components have their
    // own measurements above, where the factor is 250x for the lookup and 12x for a wide record's
    // decode. The bound here is 1.5x: a revert to scanning would take this ratio to 1.0, which is
    // what the assertion has to catch, and anything tighter would make a busy machine fail it.
    CY_CHECK_GT(scan_ns, index_ns * 1.5);
}

// --- CORRECTNESS ACROSS THE REBUILDS, WHICH IS WHY THIS CASE IS HERE AND NOT IN test_registry.cpp
// -----
//
// Everything above measures complexity. This one measures nothing: it asserts that a registry which
// has doubled its table four times still resolves every entry it was given, by identifier, by name
// and in registration order. That is a *unit* claim about the index, and it lived in the unit suite
// until the taxonomy caught it — two hundred registrations with a sweep of every earlier entry
// after each is twenty thousand lookups, which in the Debug configuration measured 1.0-2.3 ms
// against the unit suite's one-millisecond budget and failed eight runs in ten. A wall-clock budget
// is not a thing to argue with: the case belongs in the suite whose budget fits it, which is this
// one.
//
// It stays quadratic on purpose. A rebuild that dropped an entry, or inserted one under the wrong
// hash, is invisible until the type nobody looked up recently fails to resolve, so every entry is
// looked up again after every registration rather than once at the end.

CY_TEST_CASE("every registration survives the growth that rehashes the index") {
    // The entry table doubles from sixteen and the index is rebuilt with it. A rebuild that dropped
    // an entry, or that inserted one under the wrong hash, is invisible until the type nobody
    // looked up recently fails to resolve — so every entry is looked up again after every
    // registration.
    //
    // The inner sweep accumulates rather than asserting: a doctest assertion costs more than the
    // lookup it is checking, and forty thousand of them would put a unit test an order of magnitude
    // over the taxonomy's millisecond. The first mismatch is reported by index, which is what a
    // per-lookup assertion would have given.
    //
    // Two hundred crosses four rebuilds (the capacity doubles 16 -> 32 -> 64 -> 128 -> 256) and
    // leaves the case comfortably inside a unit test's millisecond on a slow machine; the sweep is
    // quadratic, so this number is chosen against the budget rather than for roundness.
    constexpr cy::u32 count = 200;
    std::vector<cy::reflect::TypeInfo> types(count);
    std::vector<std::string> names;
    names.reserve(count);
    for (cy::u32 index = 0; index < count; ++index) {
        names.push_back("cy::test::Grown" + std::to_string(index));
    }

    cy::reflect::TypeRegistry registry;
    cy::u32 first_lost = count;
    for (cy::u32 index = 0; index < count; ++index) {
        types[index] = cy::reflect::type_of<cy::demo::Health>();
        types[index].id = cy::reflect::TypeId{index + 1};
        types[index].name = names[index].c_str();
        CY_REQUIRE(registry.add(types[index]).has_value());

        for (cy::u32 earlier = 0; earlier <= index && first_lost == count; ++earlier) {
            if (registry.find(cy::reflect::TypeId{earlier + 1}) != &types[earlier]) {
                first_lost = earlier;
            }
        }
    }
    CY_CHECK_EQ(first_lost, count);  // the index of the first entry the index lost, or `count`
    CY_CHECK_EQ(registry.size(), count);

    // The name table once, at the end rather than on every step: hashing a string is linear in its
    // length, and forty-five thousand of those is the difference between this test fitting the
    // taxonomy's millisecond and not. A rebuild that lost a name would still be caught here.
    cy::u32 first_unnamed = count;
    for (cy::u32 index = 0; index < count && first_unnamed == count; ++index) {
        if (registry.find(names[index].c_str()) != &types[index]) {
            first_unnamed = index;
        }
    }
    CY_CHECK_EQ(first_unnamed, count);

    // Registration order is the iteration order, hash or no hash: artefacts derived from a walk of
    // the registry must not depend on a hash function.
    cy::u32 position = 0;
    bool in_order = true;
    for (const cy::reflect::TypeInfo* entry : registry) {
        in_order = in_order && entry == &types[position];
        ++position;
    }
    CY_CHECK(in_order);
    CY_CHECK_EQ(position, count);
}
