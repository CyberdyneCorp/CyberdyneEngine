// The shader library: the variant and program tables, and the encoding they serialise to.
// Tasks 3.4 and 3.7.
//
// THE ENCODING IS FLAT AND FIXED-WIDTH. Every field is a `u32` or a `u64`, strings live in one
// table and are referenced by offset, and the code words are one contiguous block. That shape is
// chosen so a parse is a bounds check and a set of spans rather than a walk, which is what a
// shipping build's load path wants; and it is why the format carries its own counts rather than
// terminators.
//
// ENDIANNESS. The engine's cooked artefacts are little-endian, which every platform on the roadmap
// is. This file writes and reads host order and does not swap; a big-endian target would need a
// swapping reader here and in every other cooked format, which is one decision made once rather
// than a per-format guess.

#include <cy/backends/shader/library.h>

#include <cy/core/base/assert.h>

#include <cstring>
#include <iterator>

namespace cy::shader {
namespace {

/// The on-disk width of the two fixed records. Written out rather than derived from a struct,
/// because the encoding is the contract and a `sizeof` would let a compiler's padding change the
/// format without anybody editing it.
///
///   program: 32-byte content hash, six u32 fields, one u64 compile time
///   variant: four u32 fields, one u64 permutation key
constexpr usize kProgramRecordBytes = 32 + (6 * sizeof(u32)) + sizeof(u64);
constexpr usize kVariantRecordBytes = (4 * sizeof(u32)) + sizeof(u64);

/// Appends fixed-width fields to a byte array. Every write can fail, because the array can.
class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(&out) {}

    [[nodiscard]] Status u32_value(u32 value) noexcept {
        return out_->append(Span<const u8>(reinterpret_cast<const u8*>(&value), sizeof(value)));
    }
    [[nodiscard]] Status u64_value(u64 value) noexcept {
        return out_->append(Span<const u8>(reinterpret_cast<const u8*>(&value), sizeof(value)));
    }
    /// A whole record's fields in one call. Every record here is a run of `u32`s, and writing them
    /// one `if` at a time is what turned these encoders into the most complex functions in the
    /// module for no benefit — the failure is the same failure for every field.
    [[nodiscard]] Status u32_fields(Span<const u32> fields) noexcept {
        for (const u32 field : fields) {
            if (Status written = u32_value(field); !written) {
                return written;
            }
        }
        return ok();
    }
    [[nodiscard]] Status bytes(const void* data, usize size) noexcept {
        return out_->append(Span<const u8>(static_cast<const u8*>(data), size));
    }
    [[nodiscard]] usize position() const noexcept { return out_->size(); }

private:
    Array<u8>* out_;
};

/// Reads the same fields back, refusing to run off the end.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u32_value(u32& out) noexcept {
        if (cursor_ + sizeof(u32) > bytes_.size()) {
            return false;
        }
        std::memcpy(&out, bytes_.data() + cursor_, sizeof(u32));
        cursor_ += sizeof(u32);
        return true;
    }
    [[nodiscard]] bool u64_value(u64& out) noexcept {
        if (cursor_ + sizeof(u64) > bytes_.size()) {
            return false;
        }
        std::memcpy(&out, bytes_.data() + cursor_, sizeof(u64));
        cursor_ += sizeof(u64);
        return true;
    }
    [[nodiscard]] bool read(void* out, usize size) noexcept {
        if (cursor_ + size > bytes_.size()) {
            return false;
        }
        std::memcpy(out, bytes_.data() + cursor_, size);
        cursor_ += size;
        return true;
    }
    /// The counterpart of `Writer::u32_fields`. False when the stream ran out.
    [[nodiscard]] bool u32_fields(Span<u32> fields) noexcept {
        for (u32& field : fields) {
            if (!u32_value(field)) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool skip_to(usize position) noexcept {
        if (position > bytes_.size()) {
            return false;
        }
        cursor_ = position;
        return true;
    }
    [[nodiscard]] usize position() const noexcept { return cursor_; }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
};

/// The string table: interned once, referenced by offset. Offset zero is the empty string, so a
/// zeroed field reads as "no name" rather than as a dangling reference.
class StringTable {
public:
    explicit StringTable(Allocator& allocator) noexcept : text_(allocator) {}

    [[nodiscard]] Expected<u32, Error> intern(Name name) noexcept {
        if (text_.empty()) {
            if (Status pushed = text_.push_back('\0'); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        const std::string_view value = name.text();
        if (value.empty()) {
            return u32{0};
        }
        // Linear, because a library holds tens of distinct names and a hash map would be more code
        // than the scan it replaces.
        for (usize offset = 0; offset < text_.size();) {
            const std::string_view candidate(text_.data() + offset);
            if (candidate == value) {
                return static_cast<u32>(offset);
            }
            offset += candidate.size() + 1;
        }
        const u32 offset = static_cast<u32>(text_.size());
        if (Status appended = text_.append(Span<const char>(value.data(), value.size()));
            !appended) {
            return make_unexpected(appended.error());
        }
        if (Status terminated = text_.push_back('\0'); !terminated) {
            return make_unexpected(terminated.error());
        }
        return offset;
    }

    [[nodiscard]] Span<const char> bytes() const noexcept { return {text_.data(), text_.size()}; }

private:
    Array<char> text_;
};

[[nodiscard]] Name name_at(Span<const char> table, u32 offset) noexcept {
    if (offset >= table.size()) {
        return Name{};
    }
    return Name::intern(std::string_view(table.data() + offset));
}

// --- Reflection records ------------------------------------------------------------------------

Status write_reflection(Writer& writer, StringTable& strings, const Reflection& reflection,
                        u32& bytes_written) noexcept {
    const usize start = writer.position();
    const u32 counts[] = {static_cast<u32>(reflection.bindings().size()),
                          static_cast<u32>(reflection.push_constants().size()),
                          static_cast<u32>(reflection.vertex_inputs().size()),
                          static_cast<u32>(reflection.spec_constants().size()),
                          static_cast<u32>(reflection.entry_points().size()),
                          reflection.instruction_count(),
                          reflection.spirv_version(),
                          static_cast<u32>(reflection.stages())};
    if (Status written = writer.u32_fields({counts, std::size(counts)}); !written) {
        return written;
    }

    for (const ReflectedBinding& binding : reflection.bindings()) {
        Expected<u32, Error> name = strings.intern(binding.name);
        if (!name) {
            return make_unexpected(name.error());
        }
        const u32 fields[] = {binding.set,
                              binding.binding,
                              binding.count,
                              name.value(),
                              static_cast<u32>(binding.kind),
                              static_cast<u32>(binding.stages),
                              binding.runtime_array ? 1U : 0U};
        if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
            return written;
        }
    }
    for (const ReflectedPushConstant& range : reflection.push_constants()) {
        const u32 fields[] = {range.offset, range.size, static_cast<u32>(range.stages)};
        if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
            return written;
        }
    }
    for (const ReflectedVertexInput& input : reflection.vertex_inputs()) {
        Expected<u32, Error> name = strings.intern(input.name);
        if (!name) {
            return make_unexpected(name.error());
        }
        const u32 fields[] = {input.location, static_cast<u32>(input.format), name.value()};
        if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
            return written;
        }
    }
    for (const ReflectedSpecConstant& constant : reflection.spec_constants()) {
        Expected<u32, Error> name = strings.intern(constant.name);
        if (!name) {
            return make_unexpected(name.error());
        }
        const u32 fields[] = {constant.id, constant.default_value, name.value()};
        if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
            return written;
        }
    }
    for (const ReflectedEntryPoint& entry : reflection.entry_points()) {
        Expected<u32, Error> name = strings.intern(entry.name);
        if (!name) {
            return make_unexpected(name.error());
        }
        const u32 fields[] = {name.value(), static_cast<u32>(entry.stage), entry.workgroup_size[0],
                              entry.workgroup_size[1], entry.workgroup_size[2]};
        if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
            return written;
        }
    }

    bytes_written = static_cast<u32>(writer.position() - start);
    return ok();
}

Status read_reflection(Reader& reader, Span<const char> strings, Reflection& out) noexcept {
    u32 counts[8] = {};
    if (!reader.u32_fields({counts, std::size(counts)})) {
        return fail(ErrorCode::InvalidArgument, "the shader library's reflection is truncated");
    }

    for (u32 index = 0; index < counts[0]; ++index) {
        u32 fields[7] = {};
        if (!reader.u32_fields({fields, std::size(fields)})) {
            return fail(ErrorCode::InvalidArgument, "a binding record is truncated");
        }
        ReflectedBinding binding;
        binding.set = fields[0];
        binding.binding = fields[1];
        binding.count = fields[2];
        binding.name = name_at(strings, fields[3]);
        binding.kind = static_cast<rhi::DescriptorKind>(fields[4]);
        binding.stages = static_cast<rhi::ShaderStage>(fields[5]);
        binding.runtime_array = fields[6] != 0;
        if (Status added = out.add_binding(binding); !added) {
            return added;
        }
    }
    for (u32 index = 0; index < counts[1]; ++index) {
        u32 fields[3] = {};
        if (!reader.u32_fields({fields, std::size(fields)})) {
            return fail(ErrorCode::InvalidArgument, "a push-constant record is truncated");
        }
        ReflectedPushConstant range;
        range.offset = fields[0];
        range.size = fields[1];
        range.stages = static_cast<rhi::ShaderStage>(fields[2]);
        if (Status added = out.add_push_constant(range); !added) {
            return added;
        }
    }
    for (u32 index = 0; index < counts[2]; ++index) {
        u32 fields[3] = {};
        if (!reader.u32_fields({fields, std::size(fields)})) {
            return fail(ErrorCode::InvalidArgument, "a vertex-input record is truncated");
        }
        ReflectedVertexInput input;
        input.location = fields[0];
        input.format = static_cast<rhi::Format>(fields[1]);
        input.name = name_at(strings, fields[2]);
        if (Status added = out.add_vertex_input(input); !added) {
            return added;
        }
    }
    for (u32 index = 0; index < counts[3]; ++index) {
        u32 fields[3] = {};
        if (!reader.u32_fields({fields, std::size(fields)})) {
            return fail(ErrorCode::InvalidArgument, "a specialization record is truncated");
        }
        ReflectedSpecConstant constant;
        constant.id = fields[0];
        constant.default_value = fields[1];
        constant.name = name_at(strings, fields[2]);
        if (Status added = out.add_spec_constant(constant); !added) {
            return added;
        }
    }
    for (u32 index = 0; index < counts[4]; ++index) {
        u32 fields[5] = {};
        if (!reader.u32_fields({fields, std::size(fields)})) {
            return fail(ErrorCode::InvalidArgument, "an entry-point record is truncated");
        }
        ReflectedEntryPoint entry;
        entry.name = name_at(strings, fields[0]);
        entry.stage = static_cast<rhi::ShaderStage>(fields[1]);
        entry.workgroup_size[0] = fields[2];
        entry.workgroup_size[1] = fields[3];
        entry.workgroup_size[2] = fields[4];
        if (Status added = out.add_entry_point(entry); !added) {
            return added;
        }
    }

    out.set_instruction_count(counts[5]);
    out.set_spirv_version(counts[6]);
    out.sort();
    return ok();
}

/// One program record: the content hash, six offsets, and the compile time.
///
/// A free function taking only public types, so the encoding of a record lives in one place and
/// `ShaderLibrary::serialize` reads as the three sections it writes rather than as their contents.
Status write_program_record(Writer& writer, StringTable& strings, const CompiledShader& shader,
                            const assets::ContentHash& hash, u32 code_offset, u32 reflection_offset,
                            u32 reflection_bytes) noexcept {
    Expected<u32, Error> entry_point = strings.intern(shader.entry_point());
    if (!entry_point) {
        return make_unexpected(entry_point.error());
    }
    if (Status written = writer.bytes(hash.bytes, sizeof(hash.bytes)); !written) {
        return written;
    }
    const u32 fields[] = {code_offset,         static_cast<u32>(shader.spirv().size()),
                          reflection_offset,   reflection_bytes,
                          entry_point.value(), static_cast<u32>(shader.stage())};
    if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
        return written;
    }
    return writer.u64_value(shader.stats().compile_ns);
}

Status write_variant_record(Writer& writer, StringTable& strings, const VariantKey& key,
                            u32 program) noexcept {
    Expected<u32, Error> module_name = strings.intern(key.module_name);
    if (!module_name) {
        return make_unexpected(module_name.error());
    }
    Expected<u32, Error> entry_point = strings.intern(key.entry_point);
    if (!entry_point) {
        return make_unexpected(entry_point.error());
    }
    const u32 fields[] = {module_name.value(), entry_point.value(), static_cast<u32>(key.stage),
                          program};
    if (Status written = writer.u32_fields({fields, std::size(fields)}); !written) {
        return written;
    }
    return writer.u64_value(key.permutation.value);
}

/// One program record, read back. The fields are left as they were written; turning them into a
/// `CompiledShader` needs the code and reflection sections and is `restore_program`'s job.
struct ProgramRecord {
    assets::ContentHash hash;
    u32 fields[6] = {};
    u64 compile_ns = 0;
};

bool read_program_record(Reader& reader, ProgramRecord& out) noexcept {
    return reader.read(out.hash.bytes, sizeof(out.hash.bytes)) &&
           reader.u32_fields({out.fields, std::size(out.fields)}) &&
           reader.u64_value(out.compile_ns);
}

Expected<CompiledShader, Error> restore_program(Allocator& allocator, const ProgramRecord& record,
                                                Span<const u8> bytes, usize code_at,
                                                usize reflection_at,
                                                Span<const char> strings) noexcept {
    Array<u32> code(allocator);
    if (Status sized = code.resize(record.fields[1]); !sized) {
        return make_unexpected(sized.error());
    }
    const usize code_bytes = static_cast<usize>(record.fields[1]) * sizeof(u32);
    const usize code_offset = code_at + (static_cast<usize>(record.fields[0]) * sizeof(u32));
    if (code_offset + code_bytes > bytes.size()) {
        return fail(ErrorCode::InvalidArgument, "a program's code is out of range");
    }
    std::memcpy(code.data(), bytes.data() + code_offset, code_bytes);

    if (reflection_at + record.fields[2] + record.fields[3] > bytes.size()) {
        return fail(ErrorCode::InvalidArgument, "a program's reflection is out of range");
    }
    Reader reflection_reader(
        Span<const u8>(bytes.data() + reflection_at + record.fields[2], record.fields[3]));
    Reflection reflection(allocator);
    if (Status read = read_reflection(reflection_reader, strings, reflection); !read) {
        return make_unexpected(read.error());
    }

    CompileStats stats;
    stats.compile_ns = record.compile_ns;
    stats.spirv_words = record.fields[1];
    stats.instruction_count = reflection.instruction_count();
    stats.backend = "library";

    CompiledShader shader(allocator);
    if (Status restored = shader.restore(
            std::move(code), std::move(reflection), name_at(strings, record.fields[4]),
            static_cast<rhi::ShaderStage>(record.fields[5]), stats, record.hash);
        !restored) {
        return make_unexpected(restored.error());
    }
    return shader;
}

}  // namespace

ShaderLibrary::ShaderLibrary(Allocator& allocator) noexcept
    : allocator_(&allocator), variants_(allocator), programs_(allocator) {}

ShaderProgramId ShaderLibrary::find_program(const assets::ContentHash& hash) const noexcept {
    for (usize index = 0; index < programs_.size(); ++index) {
        if (programs_[index].hash == hash) {
            return ShaderProgramId{static_cast<u32>(index)};
        }
    }
    return ShaderProgramId{};
}

Expected<ShaderProgramId, Error> ShaderLibrary::intern_program(CompiledShader&& shader) noexcept {
    const ShaderProgramId existing = find_program(shader.hash());
    if (existing.is_valid()) {
        // The identical module is already here. This is the deduplication `shader-system` asks for:
        // two materials that lower to the same code share one program and therefore one pipeline.
        ++programs_[existing.value].references;
        return existing;
    }
    const assets::ContentHash hash = shader.hash();
    Expected<Program*, Error> slot = programs_.emplace_back(Program{hash, std::move(shader), 1});
    if (!slot) {
        return make_unexpected(slot.error());
    }
    return ShaderProgramId{static_cast<u32>(programs_.size() - 1)};
}

Expected<ShaderVariantId, Error> ShaderLibrary::insert(const VariantKey& key,
                                                       CompiledShader&& shader) noexcept {
    const ShaderVariantId existing = find(key);
    if (existing.is_valid()) {
        if (Status replaced = replace(existing, std::move(shader)); !replaced) {
            return make_unexpected(replaced.error());
        }
        return existing;
    }

    Expected<ShaderProgramId, Error> program = intern_program(std::move(shader));
    if (!program) {
        return make_unexpected(program.error());
    }
    if (Status pushed = variants_.push_back(Variant{key, program.value(), 0}); !pushed) {
        return make_unexpected(pushed.error());
    }
    return ShaderVariantId{static_cast<u32>(variants_.size() - 1)};
}

ShaderVariantId ShaderLibrary::find(const VariantKey& key) const noexcept {
    for (usize index = 0; index < variants_.size(); ++index) {
        if (variants_[index].key == key) {
            return ShaderVariantId{static_cast<u32>(index)};
        }
    }
    return ShaderVariantId{};
}

const VariantKey* ShaderLibrary::key_at(ShaderVariantId id) const noexcept {
    return id.value < variants_.size() ? &variants_[id.value].key : nullptr;
}

ShaderProgramId ShaderLibrary::program_of(ShaderVariantId id) const noexcept {
    return id.value < variants_.size() ? variants_[id.value].program : ShaderProgramId{};
}

const CompiledShader* ShaderLibrary::shader_at(ShaderVariantId id) const noexcept {
    if (id.value >= variants_.size()) {
        return nullptr;
    }
    return program_at(variants_[id.value].program);
}

const CompiledShader* ShaderLibrary::program_at(ShaderProgramId id) const noexcept {
    return id.value < programs_.size() ? &programs_[id.value].shader : nullptr;
}

Status ShaderLibrary::replace(ShaderVariantId id, CompiledShader&& shader) noexcept {
    if (id.value >= variants_.size()) {
        return fail(ErrorCode::NotFound, "no such shader variant");
    }
    Variant& variant = variants_[id.value];
    const ShaderProgramId previous = variant.program;

    Expected<ShaderProgramId, Error> program = intern_program(std::move(shader));
    if (!program) {
        // The library is untouched: this is the "a failed compile keeps the previous pipeline" half
        // of the hot-reload requirement, and it holds for an allocation failure too.
        return make_unexpected(program.error());
    }
    variant.program = program.value();
    ++variant.generation;
    if (previous.is_valid() && previous != variant.program &&
        programs_[previous.value].references != 0) {
        --programs_[previous.value].references;
    }
    return ok();
}

u32 ShaderLibrary::generation(ShaderVariantId id) const noexcept {
    return id.value < variants_.size() ? variants_[id.value].generation : 0;
}

LibraryReport ShaderLibrary::report() const noexcept {
    LibraryReport out;
    out.variant_count = static_cast<u32>(variants_.size());
    out.program_count = static_cast<u32>(programs_.size());
    out.deduplicated =
        out.variant_count > out.program_count ? out.variant_count - out.program_count : 0;
    for (const Program& program : programs_) {
        out.code_bytes += program.shader.spirv().size() * sizeof(u32);
        out.compile_ns += program.shader.stats().compile_ns;
    }
    for (usize index = 0; index < variants_.size(); ++index) {
        const Variant& variant = variants_[index];
        const CompiledShader* shader = program_at(variant.program);
        if (shader == nullptr) {
            continue;
        }
        const u32 instructions = shader->reflection().instruction_count();
        if (instructions > out.max_instruction_count) {
            out.max_instruction_count = instructions;
            out.max_instruction_variant = ShaderVariantId{static_cast<u32>(index)};
        }
        const auto stage = static_cast<u16>(variant.key.stage);
        for (u32 bit = 0; bit < 8; ++bit) {
            if ((stage & (1U << bit)) != 0) {
                ++out.stage_counts[bit];
            }
        }
    }
    return out;
}

void ShaderLibrary::clear() noexcept {
    variants_.clear();
    programs_.clear();
}

Status ShaderLibrary::serialize(Array<u8>& out) const noexcept {
    StringTable strings(*allocator_);
    Array<u8> program_records(*allocator_);
    Array<u32> code(*allocator_);
    Array<u8> reflection_blob(*allocator_);
    Array<u8> variant_records(*allocator_);

    Writer programs_writer(program_records);
    Writer reflection_writer(reflection_blob);
    for (const Program& program : programs_) {
        const u32 code_offset = static_cast<u32>(code.size());
        if (Status appended = code.append(program.shader.spirv()); !appended) {
            return appended;
        }
        const u32 reflection_offset = static_cast<u32>(reflection_blob.size());
        u32 reflection_bytes = 0;
        if (Status written = write_reflection(reflection_writer, strings,
                                              program.shader.reflection(), reflection_bytes);
            !written) {
            return written;
        }
        if (Status written =
                write_program_record(programs_writer, strings, program.shader, program.hash,
                                     code_offset, reflection_offset, reflection_bytes);
            !written) {
            return written;
        }
    }

    Writer variants_writer(variant_records);
    for (const Variant& variant : variants_) {
        if (Status written =
                write_variant_record(variants_writer, strings, variant.key, variant.program.value);
            !written) {
            return written;
        }
    }

    const Span<const char> string_bytes = strings.bytes();
    out.clear();
    Writer header(out);
    const u32 header_fields[] = {kLibraryMagic,
                                 kLibraryVersion,
                                 static_cast<u32>(variants_.size()),
                                 static_cast<u32>(programs_.size()),
                                 static_cast<u32>(string_bytes.size()),
                                 static_cast<u32>(code.size()),
                                 static_cast<u32>(reflection_blob.size()),
                                 0};
    if (Status written = header.u32_fields({header_fields, std::size(header_fields)}); !written) {
        return written;
    }
    if (Status written = header.bytes(string_bytes.data(), string_bytes.size()); !written) {
        return written;
    }
    if (Status written = header.bytes(program_records.data(), program_records.size()); !written) {
        return written;
    }
    if (Status written = header.bytes(code.data(), code.size() * sizeof(u32)); !written) {
        return written;
    }
    if (Status written = header.bytes(reflection_blob.data(), reflection_blob.size()); !written) {
        return written;
    }
    return header.bytes(variant_records.data(), variant_records.size());
}

Expected<ShaderLibrary, Error> ShaderLibrary::parse(Allocator& allocator,
                                                    Span<const u8> bytes) noexcept {
    Reader reader(bytes);
    u32 header[8] = {};
    if (!reader.u32_fields({header, std::size(header)})) {
        return fail(ErrorCode::InvalidArgument, "the shader library header is truncated");
    }
    if (header[0] != kLibraryMagic) {
        return fail(ErrorCode::InvalidArgument, "not a shader library: wrong magic number");
    }
    if (header[1] != kLibraryVersion) {
        return fail(ErrorCode::Unsupported,
                    "the shader library was written by a different version of the format");
    }

    const usize strings_at = reader.position();
    const usize programs_at = strings_at + header[4];
    const usize code_at = programs_at + (static_cast<usize>(header[3]) * kProgramRecordBytes);
    const usize reflection_at = code_at + (static_cast<usize>(header[5]) * sizeof(u32));
    const usize variants_at = reflection_at + header[6];
    const usize end = variants_at + (static_cast<usize>(header[2]) * kVariantRecordBytes);
    if (end > bytes.size()) {
        return fail(ErrorCode::InvalidArgument, "the shader library is shorter than it claims");
    }
    const Span<const char> strings(reinterpret_cast<const char*>(bytes.data() + strings_at),
                                   header[4]);

    ShaderLibrary library(allocator);
    for (u32 index = 0; index < header[3]; ++index) {
        if (!reader.skip_to(programs_at + (static_cast<usize>(index) * kProgramRecordBytes))) {
            return fail(ErrorCode::InvalidArgument, "a program record is out of range");
        }
        ProgramRecord record;
        if (!read_program_record(reader, record)) {
            return fail(ErrorCode::InvalidArgument, "a program record is truncated");
        }
        Expected<CompiledShader, Error> shader =
            restore_program(allocator, record, bytes, code_at, reflection_at, strings);
        if (!shader) {
            return make_unexpected(shader.error());
        }
        if (Status pushed = library.programs_.push_back(
                ShaderLibrary::Program{record.hash, std::move(shader.value()), 0});
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    for (u32 index = 0; index < header[2]; ++index) {
        if (!reader.skip_to(variants_at + (static_cast<usize>(index) * kVariantRecordBytes))) {
            return fail(ErrorCode::InvalidArgument, "a variant record is out of range");
        }
        u32 fields[4] = {};
        u64 permutation = 0;
        if (!reader.u32_fields({fields, std::size(fields)}) || !reader.u64_value(permutation)) {
            return fail(ErrorCode::InvalidArgument, "a variant record is truncated");
        }
        if (fields[3] >= library.programs_.size()) {
            return fail(ErrorCode::InvalidArgument, "a variant names a program that is not there");
        }

        Variant variant;
        variant.key.module_name = name_at(strings, fields[0]);
        variant.key.entry_point = name_at(strings, fields[1]);
        variant.key.stage = static_cast<rhi::ShaderStage>(fields[2]);
        variant.key.permutation = PermutationKey{permutation};
        variant.program = ShaderProgramId{fields[3]};
        ++library.programs_[fields[3]].references;
        if (Status pushed = library.variants_.push_back(variant); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return library;
}

}  // namespace cy::shader
