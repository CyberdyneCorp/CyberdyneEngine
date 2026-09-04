// The compression interface and its seekable framing. Task 3.3.6.

#include <cy/core/assets/compression.h>
#include <cy/test/test.h>

#include <cstring>

using namespace cy::assets;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

/// Compressible bytes with real structure, so a ratio means something.
cy::Array<u8> corpus(usize size) {
    cy::Array<u8> bytes;
    CY_REQUIRE(bytes.resize(size).has_value());
    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(((i / 64) * 7) + (i % 13));
    }
    return bytes;
}

/// Reads out of a buffer, counting what it touched. This is how "only the containing frames are
/// read" becomes a number a test can assert on.
struct CountingSource {
    const cy::Array<u8>* bytes = nullptr;
    u64 read = 0;
};

cy::Status counting_read(void* user, u64 offset, void* destination, usize size) noexcept {
    auto* source = static_cast<CountingSource*>(user);
    if (offset + size > source->bytes->size()) {
        return cy::fail(cy::ErrorCode::OutOfRange, "past the end");
    }
    std::memcpy(destination, source->bytes->data() + offset, size);
    source->read += size;
    return cy::ok();
}

}  // namespace

CY_TEST_CASE("Zstd round-trips a block") {
    const cy::Array<u8> input = corpus(usize{64} * 1024);
    cy::Array<u8> compressed;
    CY_REQUIRE(
        compressed.resize(compress_bound(CompressionMethod::Zstd, input.size())).has_value());

    const auto written = compress(CompressionMethod::Zstd, CompressionLevel::Balanced, input.data(),
                                  input.size(), compressed.data(), compressed.size());
    CY_REQUIRE(written.has_value());
    CY_CHECK(written.value() < input.size());  // this corpus is compressible

    cy::Array<u8> restored;
    CY_REQUIRE(restored.resize(input.size()).has_value());
    CY_REQUIRE(decompress(CompressionMethod::Zstd, compressed.data(), written.value(),
                          restored.data(), restored.size())
                   .has_value());
    CY_CHECK(std::memcmp(input.data(), restored.data(), input.size()) == 0);
}

CY_TEST_CASE("The stored method copies") {
    const cy::Array<u8> input = corpus(1024);
    cy::Array<u8> output;
    CY_REQUIRE(output.resize(input.size()).has_value());
    const auto written = compress(CompressionMethod::None, CompressionLevel::Fast, input.data(),
                                  input.size(), output.data(), output.size());
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(written.value(), input.size());
    CY_CHECK(std::memcmp(input.data(), output.data(), input.size()) == 0);
}

CY_TEST_CASE("A codec that is not pinned is refused by name") {
    // LZ4 and Deflate are in the specification and in this enumeration; neither is in
    // deps/manifest.toml, so every entry point refuses them rather than pretending.
    CY_CHECK_FALSE(compression_method_available(CompressionMethod::Lz4));
    CY_CHECK_FALSE(compression_method_available(CompressionMethod::Deflate));
    CY_CHECK(compression_method_available(CompressionMethod::Zstd));
    CY_CHECK(compression_method_available(CompressionMethod::None));

    cy::Array<u8> output;
    CY_REQUIRE(output.resize(64).has_value());
    const auto refused = compress(CompressionMethod::Lz4, CompressionLevel::Fast, "abc", 3,
                                  output.data(), output.size());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);
}

CY_TEST_CASE("A too-small output buffer is reported rather than overrun") {
    const cy::Array<u8> input = corpus(4096);
    cy::Array<u8> output;
    CY_REQUIRE(output.resize(8).has_value());
    const auto refused = compress(CompressionMethod::Zstd, CompressionLevel::Balanced, input.data(),
                                  input.size(), output.data(), output.size());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::BufferTooSmall);
}

CY_TEST_CASE("A truncated payload fails rather than misparsing") {
    const cy::Array<u8> input = corpus(4096);
    cy::Array<u8> compressed;
    CY_REQUIRE(
        compressed.resize(compress_bound(CompressionMethod::Zstd, input.size())).has_value());
    const auto written = compress(CompressionMethod::Zstd, CompressionLevel::Balanced, input.data(),
                                  input.size(), compressed.data(), compressed.size());
    CY_REQUIRE(written.has_value());

    cy::Array<u8> restored;
    CY_REQUIRE(restored.resize(input.size()).has_value());
    CY_CHECK_FALSE(decompress(CompressionMethod::Zstd, compressed.data(), written.value() / 2,
                              restored.data(), restored.size())
                       .has_value());
}

CY_TEST_CASE("Framing round-trips the whole payload") {
    const cy::Array<u8> input = corpus(usize{200} * 1024);
    cy::Array<u8> stored;
    cy::Array<FrameIndexEntry> index;
    CY_REQUIRE(compress_framed(CompressionMethod::Zstd, CompressionLevel::Balanced, input.data(),
                               input.size(), 64 * 1024, stored, index)
                   .has_value());
    CY_CHECK_EQ(index.size(), 4u);  // 200 KiB in 64 KiB frames
    CY_CHECK_EQ(framed_uncompressed_size(index.span()), input.size());

    cy::Array<u8> restored;
    CY_REQUIRE(restored.resize(input.size()).has_value());
    CountingSource source{&stored, 0};
    CY_REQUIRE(decompress_range(CompressionMethod::Zstd, index.span(), 64 * 1024, counting_read,
                                &source, 0, restored.data(), restored.size(), nullptr)
                   .has_value());
    CY_CHECK(std::memcmp(input.data(), restored.data(), input.size()) == 0);
}

CY_TEST_CASE("Scenario: Partial read of a large asset") {
    // WHEN a streaming system requests one mip level of a large texture
    // THEN only the containing compression frames SHALL be read and decompressed.
    const usize frame_bytes = usize{64} * 1024;
    const cy::Array<u8> input = corpus(frame_bytes * 16);
    cy::Array<u8> stored;
    cy::Array<FrameIndexEntry> index;
    CY_REQUIRE(compress_framed(CompressionMethod::Zstd, CompressionLevel::Balanced, input.data(),
                               input.size(), static_cast<u32>(frame_bytes), stored, index)
                   .has_value());
    CY_CHECK_EQ(index.size(), 16u);

    // One 1 KiB window in the middle of frame 9.
    const u64 offset = (frame_bytes * 9) + 1000;
    cy::Array<u8> window;
    CY_REQUIRE(window.resize(1024).has_value());
    CountingSource source{&stored, 0};
    u32 frames_touched = 0;
    CY_REQUIRE(decompress_range(CompressionMethod::Zstd, index.span(),
                                static_cast<u32>(frame_bytes), counting_read, &source, offset,
                                window.data(), window.size(), &frames_touched)
                   .has_value());

    CY_CHECK(std::memcmp(input.data() + offset, window.data(), window.size()) == 0);
    // The claim, stated as numbers: one frame out of sixteen, and the bytes read from the payload
    // are that frame's alone rather than the whole entry's.
    CY_CHECK_EQ(frames_touched, 1u);
    CY_CHECK_EQ(source.read, index[9].compressed_size);
    CY_CHECK(source.read < stored.size() / 4);
}

CY_TEST_CASE("A range spanning two frames touches exactly two") {
    const usize frame_bytes = 4096;
    const cy::Array<u8> input = corpus(frame_bytes * 8);
    cy::Array<u8> stored;
    cy::Array<FrameIndexEntry> index;
    CY_REQUIRE(compress_framed(CompressionMethod::None, CompressionLevel::Fast, input.data(),
                               input.size(), static_cast<u32>(frame_bytes), stored, index)
                   .has_value());

    cy::Array<u8> window;
    CY_REQUIRE(window.resize(200).has_value());
    CountingSource source{&stored, 0};
    u32 frames_touched = 0;
    CY_REQUIRE(decompress_range(CompressionMethod::None, index.span(),
                                static_cast<u32>(frame_bytes), counting_read, &source,
                                frame_bytes - 100, window.data(), window.size(), &frames_touched)
                   .has_value());
    CY_CHECK_EQ(frames_touched, 2u);
    CY_CHECK(std::memcmp(input.data() + frame_bytes - 100, window.data(), window.size()) == 0);
}

CY_TEST_CASE("A range outside the payload is refused") {
    const cy::Array<u8> input = corpus(4096);
    cy::Array<u8> stored;
    cy::Array<FrameIndexEntry> index;
    CY_REQUIRE(compress_framed(CompressionMethod::None, CompressionLevel::Fast, input.data(),
                               input.size(), 1024, stored, index)
                   .has_value());
    cy::Array<u8> window;
    CY_REQUIRE(window.resize(64).has_value());
    CountingSource source{&stored, 0};
    const auto refused =
        decompress_range(CompressionMethod::None, index.span(), 1024, counting_read, &source, 4090,
                         window.data(), window.size(), nullptr);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("An incompressible frame is stored rather than grown") {
    // Random-looking bytes: zstd cannot shrink them, and a frame that grew would make the package
    // larger than its input. The reader tells a stored frame by its two sizes being equal.
    cy::Array<u8> input;
    CY_REQUIRE(input.resize(8192).has_value());
    cy::u64 state = 0x9E3779B97F4A7C15ull;
    for (u8& byte : input) {
        state = (state * 6364136223846793005ull) + 1442695040888963407ull;
        byte = static_cast<u8>(state >> 56);
    }

    cy::Array<u8> stored;
    cy::Array<FrameIndexEntry> index;
    CY_REQUIRE(compress_framed(CompressionMethod::Zstd, CompressionLevel::Balanced, input.data(),
                               input.size(), 4096, stored, index)
                   .has_value());
    CY_CHECK(stored.size() <= input.size());
    for (const FrameIndexEntry& frame : index) {
        CY_CHECK(frame.compressed_size <= frame.uncompressed_size);
    }

    cy::Array<u8> restored;
    CY_REQUIRE(restored.resize(input.size()).has_value());
    CountingSource source{&stored, 0};
    CY_REQUIRE(decompress_range(CompressionMethod::Zstd, index.span(), 4096, counting_read, &source,
                                0, restored.data(), restored.size(), nullptr)
                   .has_value());
    CY_CHECK(std::memcmp(input.data(), restored.data(), input.size()) == 0);
}

CY_TEST_CASE("Already-compressed data is not recompressed by default") {
    // `core-assets-and-io`: "Already-compressed data such as block-compressed texture blocks SHALL
    // NOT be recompressed by default."
    CY_CHECK(default_method_for(PayloadForm::AlreadyCompressed) == CompressionMethod::None);
    CY_CHECK(default_method_for(PayloadForm::Compressible) == CompressionMethod::Zstd);
}
