#include <cy/servers/text/cooked_font.h>

#include <cstring>

namespace cy::text {
namespace {

constexpr char kMagic[8] = {'C', 'Y', 'F', 'O', 'N', 'T', '\0', '\0'};

/// The font file starts on this boundary within a cooked font. FreeType and HarfBuzz both read
/// their tables a byte at a time and do not need it; it is there so that a cooked font read into an
/// aligned buffer hands either library an aligned table directory anyway, which costs at most
/// fifteen bytes.
constexpr usize kFontAlignment = 16;

class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(out) {}

    [[nodiscard]] Status bytes(const void* data, usize size) noexcept {
        if (size == 0) {
            return ok();
        }
        return out_.append(Span<const u8>(static_cast<const u8*>(data), size));
    }
    [[nodiscard]] Status u32_le(u32 value) noexcept {
        const u8 raw[4] = {static_cast<u8>(value), static_cast<u8>(value >> 8U),
                           static_cast<u8>(value >> 16U), static_cast<u8>(value >> 24U)};
        return bytes(raw, sizeof(raw));
    }
    [[nodiscard]] Status f32_le(f32 value) noexcept {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32_le(bits);
    }
    [[nodiscard]] Status string(std::string_view text) noexcept {
        if (Status length = u32_le(static_cast<u32>(text.size())); !length) {
            return length;
        }
        return bytes(text.data(), text.size());
    }
    [[nodiscard]] Status align(usize boundary) noexcept {
        while (out_.size() % boundary != 0) {
            if (Status pushed = out_.push_back(0); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

private:
    Array<u8>& out_;
};

/// Reads fields in order and remembers the first one that did not fit, so a refusal names it.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool failed() const noexcept { return failed_ != nullptr; }
    [[nodiscard]] const char* failure() const noexcept { return failed_; }
    [[nodiscard]] usize position() const noexcept { return cursor_; }

    [[nodiscard]] Span<const u8> take(usize size, const char* field) noexcept {
        if (failed() || size > bytes_.size() - cursor_) {
            fail_on(field);
            return {};
        }
        const Span<const u8> taken(bytes_.data() + cursor_, size);
        cursor_ += size;
        return taken;
    }
    [[nodiscard]] u32 u32_le(const char* field) noexcept {
        const Span<const u8> raw = take(4, field);
        if (raw.empty()) {
            return 0;
        }
        return static_cast<u32>(raw[0]) | (static_cast<u32>(raw[1]) << 8U) |
               (static_cast<u32>(raw[2]) << 16U) | (static_cast<u32>(raw[3]) << 24U);
    }
    [[nodiscard]] f32 f32_le(const char* field) noexcept {
        const u32 bits = u32_le(field);
        f32 value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    [[nodiscard]] std::string_view string(const char* field) noexcept {
        const u32 length = u32_le(field);
        const Span<const u8> raw = take(length, field);
        return {reinterpret_cast<const char*>(raw.data()), raw.size()};
    }
    void align(usize boundary, const char* field) noexcept {
        const usize padding = (boundary - (cursor_ % boundary)) % boundary;
        (void)take(padding, field);
    }
    void fail_on(const char* field) noexcept {
        if (failed_ == nullptr) {
            failed_ = field;
        }
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
    const char* failed_ = nullptr;
};

[[nodiscard]] Status write_desc(Writer& writer, const FontDesc& desc) noexcept {
    const u8 flags[4] = {static_cast<u8>(desc.mode), static_cast<u8>(desc.hinting),
                         static_cast<u8>(desc.synthetic_bold ? 1U : 0U),
                         static_cast<u8>(desc.synthetic_italic ? 1U : 0U)};
    Status status = writer.string(desc.family);
    status = status ? writer.f32_le(desc.size_pixels) : status;
    status = status ? writer.bytes(flags, sizeof(flags)) : status;
    status = status ? writer.f32_le(desc.distance_range) : status;
    status = status ? writer.u32_le(desc.axis_count) : status;
    for (usize index = 0; status && index < kMaxFontAxes; ++index) {
        status = writer.bytes(desc.axes[index].tag, 4);
        status = status ? writer.f32_le(desc.axes[index].value) : status;
    }
    status = status ? writer.u32_le(desc.feature_count) : status;
    for (usize index = 0; status && index < kMaxFontFeatures; ++index) {
        status = writer.bytes(desc.features[index].tag, 4);
        status = status ? writer.u32_le(desc.features[index].value) : status;
    }
    return status;
}

void read_desc(Reader& reader, FontDesc& desc) noexcept {
    desc.family = reader.string("family");
    desc.size_pixels = reader.f32_le("size");
    const Span<const u8> flags = reader.take(4, "render flags");
    if (!flags.empty()) {
        if (flags[0] > static_cast<u8>(RenderMode::SignedDistanceField) ||
            flags[1] > static_cast<u8>(Hinting::Full)) {
            reader.fail_on("render flags");
        }
        desc.mode = static_cast<RenderMode>(flags[0]);
        desc.hinting = static_cast<Hinting>(flags[1]);
        desc.synthetic_bold = flags[2] != 0;
        desc.synthetic_italic = flags[3] != 0;
    }
    desc.distance_range = reader.f32_le("distance range");
    desc.axis_count = reader.u32_le("axis count");
    for (usize index = 0; index < kMaxFontAxes; ++index) {
        const Span<const u8> tag = reader.take(4, "axis tag");
        if (!tag.empty()) {
            std::memcpy(desc.axes[index].tag, tag.data(), 4);
        }
        desc.axes[index].value = reader.f32_le("axis value");
    }
    desc.feature_count = reader.u32_le("feature count");
    for (usize index = 0; index < kMaxFontFeatures; ++index) {
        const Span<const u8> tag = reader.take(4, "feature tag");
        if (!tag.empty()) {
            std::memcpy(desc.features[index].tag, tag.data(), 4);
        }
        desc.features[index].value = reader.u32_le("feature value");
    }
    if (desc.axis_count > kMaxFontAxes) {
        reader.fail_on("axis count");
    }
    if (desc.feature_count > kMaxFontFeatures) {
        reader.fail_on("feature count");
    }
    if (!(desc.size_pixels > 0.0f)) {
        reader.fail_on("size");
    }
}

[[nodiscard]] Status write_glyph(Writer& writer, const CookedGlyph& glyph) noexcept {
    Status status = writer.u32_le(glyph.glyph);
    status = status ? writer.f32_le(glyph.metrics.advance) : status;
    status = status ? writer.f32_le(glyph.metrics.bearing_x) : status;
    status = status ? writer.f32_le(glyph.metrics.bearing_y) : status;
    status = status ? writer.u32_le(glyph.metrics.width) : status;
    status = status ? writer.u32_le(glyph.metrics.height) : status;
    status = status ? writer.u32_le(static_cast<u32>(glyph.format)) : status;
    status = status ? writer.u32_le(glyph.x) : status;
    status = status ? writer.u32_le(glyph.y) : status;
    return status;
}

[[nodiscard]] CookedGlyph read_glyph(Reader& reader) noexcept {
    CookedGlyph glyph;
    glyph.glyph = reader.u32_le("glyph index");
    glyph.metrics.advance = reader.f32_le("glyph advance");
    glyph.metrics.bearing_x = reader.f32_le("glyph bearing");
    glyph.metrics.bearing_y = reader.f32_le("glyph bearing");
    glyph.metrics.width = reader.u32_le("glyph width");
    glyph.metrics.height = reader.u32_le("glyph height");
    const u32 format = reader.u32_le("glyph format");
    if (format >= kPixelFormatCount) {
        reader.fail_on("glyph format");
    }
    glyph.format = static_cast<PixelFormat>(format < kPixelFormatCount ? format : 0U);
    glyph.x = reader.u32_le("glyph position");
    glyph.y = reader.u32_le("glyph position");
    return glyph;
}

/// Whether a glyph's rectangle lies inside its page. Checked on read, so the server's copy out of
/// the page cannot run past it.
[[nodiscard]] bool inside(const CookedGlyph& glyph, const CookedPage& page) noexcept {
    const u64 right = static_cast<u64>(glyph.x) + glyph.metrics.width;
    const u64 bottom = static_cast<u64>(glyph.y) + glyph.metrics.height;
    if (glyph.metrics.width == 0 || glyph.metrics.height == 0) {
        return right <= page.extent && bottom <= page.extent;
    }
    return page.extent != 0 && right <= page.extent && bottom <= page.extent;
}

}  // namespace

Status write_cooked_font(const CookedFontContent& content, Array<u8>& out) noexcept {
    out.clear();
    Writer writer(out);
    Status status = writer.bytes(kMagic, sizeof(kMagic));
    status = status ? writer.u32_le(kCookedFontVersion) : status;
    status = status ? write_desc(writer, content.desc) : status;
    status = status ? writer.u32_le(content.source.face_index) : status;
    status = status ? writer.u32_le(static_cast<u32>(content.source.bytes.size())) : status;
    status = status ? writer.align(kFontAlignment) : status;
    status =
        status ? writer.bytes(content.source.bytes.data(), content.source.bytes.size()) : status;
    status = status ? writer.u32_le(static_cast<u32>(content.ranges.size())) : status;
    for (usize index = 0; status && index < content.ranges.size(); ++index) {
        status = writer.u32_le(content.ranges[index].first);
        status = status ? writer.u32_le(content.ranges[index].last) : status;
    }
    status = status ? writer.u32_le(static_cast<u32>(content.fallbacks.size())) : status;
    for (usize index = 0; status && index < content.fallbacks.size(); ++index) {
        status = writer.string(content.fallbacks[index]);
    }
    status = status ? writer.u32_le(static_cast<u32>(content.glyphs.size())) : status;
    for (usize index = 0; status && index < content.glyphs.size(); ++index) {
        status = write_glyph(writer, content.glyphs[index]);
    }
    for (u32 format = 0; status && format < kPixelFormatCount; ++format) {
        const CookedPage& page = content.pages[format];
        const usize expected = static_cast<usize>(page.extent) * page.extent *
                               bytes_per_pixel(static_cast<PixelFormat>(format));
        if (page.pixels.size() != expected) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked font page whose pixels do not fill its extent");
        }
        status = writer.u32_le(page.extent);
        status = status ? writer.bytes(page.pixels.data(), page.pixels.size()) : status;
    }
    return status;
}

Status CookedFont::parse(Span<const u8> bytes) noexcept {
    *this = CookedFont{};
    Reader reader(bytes);
    const Span<const u8> magic = reader.take(sizeof(kMagic), "magic");
    if (magic.empty() || std::memcmp(magic.data(), kMagic, sizeof(kMagic)) != 0) {
        return fail(ErrorCode::InvalidArgument, "not a cooked font: the magic does not match");
    }
    const u32 version = reader.u32_le("version");
    if (version != kCookedFontVersion) {
        return fail(ErrorCode::Unsupported,
                    "a cooked font of another version; re-cook it with this build's importer");
    }
    read_desc(reader, desc_);
    source_.face_index = reader.u32_le("face index");
    const u32 font_size = reader.u32_le("font size");
    reader.align(kFontAlignment, "font alignment");
    source_.bytes = reader.take(font_size, "font bytes");

    const u32 range_count = reader.u32_le("range count");
    for (u32 index = 0; !reader.failed() && index < range_count; ++index) {
        CodepointRange range;
        range.first = reader.u32_le("range");
        range.last = reader.u32_le("range");
        if (range.last < range.first) {
            reader.fail_on("range");
        }
        if (Status pushed = ranges_.push_back(range); !pushed) {
            return pushed;
        }
    }
    const u32 fallback_count = reader.u32_le("fallback count");
    for (u32 index = 0; !reader.failed() && index < fallback_count; ++index) {
        if (Status pushed = fallbacks_.push_back(reader.string("fallback")); !pushed) {
            return pushed;
        }
    }
    const u32 glyph_count = reader.u32_le("glyph count");
    for (u32 index = 0; !reader.failed() && index < glyph_count; ++index) {
        if (Status pushed = glyphs_.push_back(read_glyph(reader)); !pushed) {
            return pushed;
        }
    }
    for (u32 format = 0; !reader.failed() && format < kPixelFormatCount; ++format) {
        CookedPage& page = pages_[format];
        page.extent = reader.u32_le("page extent");
        if (page.extent > 16384) {
            reader.fail_on("page extent");
            break;
        }
        page.pixels = reader.take(static_cast<usize>(page.extent) * page.extent *
                                      bytes_per_pixel(static_cast<PixelFormat>(format)),
                                  "page pixels");
    }
    for (const CookedGlyph& glyph : glyphs_) {
        if (!reader.failed() && !inside(glyph, page(glyph.format))) {
            reader.fail_on("a glyph rectangle outside its page");
        }
    }
    if (reader.failed()) {
        const char* field = reader.failure();
        *this = CookedFont{};
        return fail(ErrorCode::InvalidArgument, field);
    }
    return ok();
}

bool CookedFont::covers(Codepoint codepoint) const noexcept {
    for (const CodepointRange& range : ranges_) {
        if (codepoint >= range.first && codepoint <= range.last) {
            return true;
        }
    }
    return false;
}

}  // namespace cy::text
