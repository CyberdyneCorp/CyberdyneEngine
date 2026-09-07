// Material cooking as a graph node. M7 task 6.3. See cook.h.

#include <cy/material/cook.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/text.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace cy::material {
namespace {

using namespace cy::rendering::material;

constexpr u32 kMagic = 0x424D5943U;  // 'CYMB'

void put_u32(Array<u8>& out, u32 value, Status& status) noexcept {
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> shift) & 0xFFU)); !pushed) {
            status = pushed;
            return;
        }
    }
}

void put_u64(Array<u8>& out, u64 value, Status& status) noexcept {
    put_u32(out, static_cast<u32>(value & 0xFFFFFFFFULL), status);
    put_u32(out, static_cast<u32>(value >> 32U), status);
}

void put_f32(Array<u8>& out, f32 value, Status& status) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits, status);
}

void put_bytes(Array<u8>& out, Span<const u8> bytes, Status& status) noexcept {
    put_u32(out, static_cast<u32>(bytes.size()), status);
    if (!status) {
        return;
    }
    if (Status appended = out.append(bytes); !appended) {
        status = appended;
    }
}

class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    [[nodiscard]] u32 read_u32() noexcept {
        if (!ok_ || cursor_ + 4 > bytes_.size()) {
            ok_ = false;
            return 0;
        }
        u32 value = 0;
        for (u32 index = 0; index < 4U; ++index) {
            value |= static_cast<u32>(bytes_[cursor_ + index]) << (index * 8U);
        }
        cursor_ += 4;
        return value;
    }

    [[nodiscard]] u64 read_u64() noexcept {
        const u64 low = read_u32();
        const u64 high = read_u32();
        return low | (high << 32U);
    }

    [[nodiscard]] f32 read_f32() noexcept {
        const u32 bits = read_u32();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] Span<const u8> read_bytes() noexcept {
        const u32 size = read_u32();
        if (!ok_ || cursor_ + size > bytes_.size()) {
            ok_ = false;
            return {};
        }
        const Span<const u8> view(bytes_.data() + cursor_, size);
        cursor_ += size;
        return view;
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
    bool ok_ = true;
};

void append_text(Array<char>& out, std::string_view text) noexcept {
    for (const char character : text) {
        if (!out.push_back(character)) {
            return;
        }
    }
}

/// One number with a fixed number of decimals, and its surrounding text.
///
/// Not a `printf` format taken from the caller: `-Wformat-nonliteral` is on for the whole tree and
/// it is right to be — a format string that is not a literal is one nothing checks the arguments
/// against. The two shapes the report needs are a count and a millisecond figure, so they are two
/// functions.
void append_count(Array<char>& out, std::string_view prefix, u32 value,
                  std::string_view suffix) noexcept {
    char buffer[32] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%u", value);
    append_text(out, prefix);
    append_text(out, buffer);
    append_text(out, suffix);
}

void append_scaled(Array<char>& out, std::string_view prefix, f32 value,
                   std::string_view suffix) noexcept {
    char buffer[32] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
    append_text(out, prefix);
    append_text(out, buffer);
    append_text(out, suffix);
}

/// The cook report `material-compiler` asks for, as text a build log carries and a person reads.
void write_report(const CompiledMaterial& material, Array<char>& out) noexcept {
    append_text(out, "material ");
    append_text(out, material.primary().name().text());
    append_text(out, "  cook key 0x");
    char key[24] = {};
    (void)std::snprintf(key, sizeof(key), "%016llx",
                        static_cast<unsigned long long>(material.cook_key()));
    append_text(out, key);
    append_text(out, "\n");
    for (const CompiledProgram& program : material.programs()) {
        append_text(out, "  ");
        append_text(out, program_kind_name(program.kind));
        append_text(out, "/");
        append_text(out, quality_tier_name(program.tier));
        if (program.absent) {
            append_text(out, ": no program (opacity is constant, so there is no fragment work)\n");
            continue;
        }
        append_text(out, ": ");
        append_count(out, "", program.cost.texture_samples, " samples");
        append_count(out, ", ", program.cost.arithmetic, " alu");
        append_count(out, ", ", program.cost.closures, " closures");
        append_count(out, ", ", program.cost.permutation_count, " permutations");
        append_scaled(out, ", ", program.cost.full_screen_ms, " ms full screen");
        if (program.generic_evaluator) {
            append_scaled(out, "  [generic evaluator, x", program.cost.generic_cost_multiple, "]");
        }
        append_text(out, "\n");
    }
    for (const CompileDiagnostic& diagnostic : material.diagnostics()) {
        append_text(out, "  ");
        append_text(out, diagnostic_severity_name(diagnostic.severity));
        append_text(out, " ");
        append_text(out, diagnostic.code);
        append_text(out, " (");
        append_text(out, diagnostic.subject.text());
        append_text(out, "): ");
        append_text(out, diagnostic.detail);
        append_text(out, "\n");
    }
}

[[nodiscard]] Status encode_bundle(const CompiledMaterial& material, Allocator& allocator,
                                   Array<u8>& out) noexcept {
    Status status = ok();
    put_u32(out, kMagic, status);
    put_u32(out, kBundleVersion, status);
    put_u32(out, kCompilerVersion, status);
    put_u64(out, material.cook_key(), status);

    Array<u8> ir(allocator);
    if (Status encoded = encode_module(material.primary(), ir); !encoded) {
        return encoded;
    }
    put_bytes(out, ir.span(), status);

    put_u32(out, static_cast<u32>(material.programs().size()), status);
    for (const CompiledProgram& program : material.programs()) {
        put_u32(out, static_cast<u32>(program.kind), status);
        put_u32(out, static_cast<u32>(program.tier), status);
        put_u64(out, program.source.digest, status);
        put_u32(out, program.absent ? 1U : 0U, status);
        put_u32(out, program.cost.texture_samples, status);
        put_u32(out, program.cost.arithmetic, status);
        put_u32(out, program.cost.closures, status);
        put_u32(out, program.cost.permutation_count, status);
        put_f32(out, program.cost.full_screen_ms, status);
        put_bytes(out,
                  Span<const u8>(reinterpret_cast<const u8*>(program.source.text.data()),
                                 program.source.text.size()),
                  status);
    }
    return status;
}

/// `material` — one text material definition in, one cooked bundle out.
[[nodiscard]] Status produce_material(build::NodeContext& context) {
    const build::NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a material node declares exactly one source and one output");
    }

    Allocator& allocator = system_allocator(MemoryDomain::Assets);
    Array<u8> bytes(allocator);
    if (Status read = context.read(node.sources.front(), bytes); !read) {
        return read;
    }

    auto profile = profile_from_name(node.option("profile", "desktop"));
    if (!profile) {
        context.diagnose(build::Severity::Error, "unknown-profile",
                         "no renderer profile of this name",
                         std::string(node.option("profile", "desktop")));
        return make_unexpected(profile.error());
    }
    CompileOptions options;
    options.profile = profile.value();
    options.derive_family = node.option("family", "true") == "true";
    options.derive_tiers = node.option("tiers", "true") == "true";

    Array<u8> bundle(allocator);
    Array<char> report(allocator);
    if (Status cooked = cook_material(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options,
            allocator, bundle, report);
        !cooked) {
        context.diagnose(build::Severity::Error, "material-cook-failed",
                         std::string(report.data(), report.size()), node.sources.front());
        return cooked;
    }
    context.diagnose(build::Severity::Info, "material-cost",
                     std::string(report.data(), report.size()), node.sources.front());
    return context.write(node.outputs.front(), bundle.data(), bundle.size());
}

}  // namespace

Expected<rendering::material::Profile, Error> profile_from_name(std::string_view name) noexcept {
    if (name == "desktop") {
        return desktop_profile();
    }
    if (name == "mobile") {
        return mobile_profile();
    }
    return make_unexpected(Error{ErrorCode::InvalidArgument,
                                 "no renderer profile of this name; it is `desktop` or `mobile`",
                                 0});
}

Status cook_material(std::string_view source, const CompileOptions& options, Allocator& allocator,
                     Array<u8>& out, Array<char>& report) noexcept {
    ParseDiagnostic diagnostic(allocator);
    auto module = parse_material(source, allocator, diagnostic, options.passes);
    if (!module) {
        append_count(report, "line ", diagnostic.line, ": ");
        append_text(report, diagnostic.text());
        append_text(report, "\n");
        return make_unexpected(module.error());
    }
    auto compiled = compile_material(module.value(), options, allocator);
    if (!compiled) {
        append_text(report, "the material could not be compiled\n");
        return make_unexpected(compiled.error());
    }
    write_report(compiled.value(), report);
    if (compiled.value().failed()) {
        // A failed node produces NO artefact. `build-and-packaging` requires it, and a cached
        // failure is a failure you cannot clear by fixing the source.
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the material reported a cook-time error", 0});
    }
    return encode_bundle(compiled.value(), allocator, out);
}

Expected<CookedBundle, Error> decode_bundle(Span<const u8> bytes, Allocator& allocator) noexcept {
    Reader reader(bytes);
    const u32 magic = reader.read_u32();
    const u32 version = reader.read_u32();
    if (!reader.ok() || magic != kMagic) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "not a cooked material bundle", 0});
    }
    if (version != kBundleVersion) {
        return make_unexpected(Error{ErrorCode::Unsupported,
                                     "this bundle was written by a different cooker version", 0});
    }
    CookedBundle bundle(allocator);
    bundle.compiler_version = reader.read_u32();
    bundle.cook_key = reader.read_u64();
    bundle.ir = reader.read_bytes();

    const u32 count = reader.read_u32();
    for (u32 index = 0; index < count && reader.ok(); ++index) {
        CookedProgram program;
        program.kind = static_cast<ProgramKind>(reader.read_u32());
        program.tier = static_cast<QualityTier>(reader.read_u32());
        program.program_digest = reader.read_u64();
        program.absent = reader.read_u32() != 0;
        program.texture_samples = reader.read_u32();
        program.arithmetic = reader.read_u32();
        program.closures = reader.read_u32();
        program.permutation_count = reader.read_u32();
        program.full_screen_ms = reader.read_f32();
        const Span<const u8> source = reader.read_bytes();
        program.source =
            std::string_view(reinterpret_cast<const char*>(source.data()), source.size());
        if (!reader.ok()) {
            break;
        }
        if (Status pushed = bundle.programs.push_back(program); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (!reader.ok()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a truncated cooked material bundle", 0});
    }
    return bundle;
}

Status add_material_producers(build::ProducerRegistry& registry) noexcept {
    // Distributable: the cook is a pure function of its source and its declared options, it links
    // no licensed tool and it names no device. That is a stronger claim than `cook`'s, which is not
    // distributable because it emits blocks against a process-wide component registry.
    return registry.add(build::Producer{"material", kMaterialProducerVersion, produce_material,
                                        /*distributable=*/true});
}

}  // namespace cy::material
