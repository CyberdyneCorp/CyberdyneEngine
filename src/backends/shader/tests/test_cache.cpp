// The cache key and the tiered cache. Task 3.4.
//
// `shader-system` fixes what the key contains "so a compiler change invalidates derived data
// without invalidating authored assets". Every field below is therefore checked for *sensitivity*:
// a key that ignored one of them would serve a stale binary after an upgrade, which is a class of
// bug that reproduces as "it works on my machine" for a week.

#include <cy/backends/shader/cache.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <utility>

using cy::u8;
using cy::usize;
using namespace cy::shader;

namespace {

CacheKeyInputs base_inputs() {
    CacheKeyInputs inputs;
    inputs.source_hash = cy::assets::content_hash("float4 main() { return 1; }", 27);
    inputs.compiler_name = "slang";
    inputs.compiler_version = "2026.9.2";
    inputs.target_platform = "vulkan-spirv";
    inputs.renderer_profile = "forward-clustered";
    inputs.entry_point = cy::Name::intern("main");
    return inputs;
}

cy::Span<const u8> bytes_of(const char* text, usize length) {
    return {reinterpret_cast<const u8*>(text), length};
}

}  // namespace

CY_TEST_CASE("the same inputs always derive the same key") {
    CY_CHECK(derive_cache_key(base_inputs()) == derive_cache_key(base_inputs()));
}

CY_TEST_CASE("every field the specification names changes the key") {
    const CacheKey reference = derive_cache_key(base_inputs());

    auto changed = base_inputs();
    changed.compiler_version = "2026.10.0";
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.target_platform = "metal-msl";
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.renderer_profile = "visibility-buffer";
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.artefact_version = kShaderArtefactVersion + 1;
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.permutation = 1;
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.debug_info = DebugInfoLevel::Full;
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.optimization = OptimizationLevel::None;
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.entry_point = cy::Name::intern("other");
    CY_CHECK(derive_cache_key(changed) != reference);

    changed = base_inputs();
    changed.source_hash = cy::assets::content_hash("different", 9);
    CY_CHECK(derive_cache_key(changed) != reference);
}

CY_TEST_CASE("the feature set is length-prefixed, so a split cannot collide") {
    // Without a length prefix, {"vulkan", "spirv"} and {"vulkans", "pirv"} would hash identically
    // and two different builds would share one entry.
    const char* const first[] = {"vulkan", "spirv"};
    const char* const second[] = {"vulkans", "pirv"};

    auto a = base_inputs();
    a.feature_set = {first, 2};
    auto b = base_inputs();
    b.feature_set = {second, 2};
    CY_CHECK(derive_cache_key(a) != derive_cache_key(b));
}

CY_TEST_CASE("a key names a sharded path so a big cache is not one directory") {
    const CacheKey key = derive_cache_key(base_inputs());
    auto root = cy::assets::VirtualPath::normalise("cache/shaders");
    CY_REQUIRE(root.has_value());
    auto path = key.to_path(*root);
    CY_REQUIRE(path.has_value());

    char text[cy::assets::ContentHash::kTextLength + 1] = {};
    key.hash.format(text);
    const std::string_view view = path->view();
    CY_CHECK(view.starts_with("cache/shaders/"));
    CY_CHECK(view.ends_with(".cyshader"));
    // Two characters of the digest become the directory.
    CY_CHECK(view[14] == text[0]);
    CY_CHECK(view[15] == text[1]);
    CY_CHECK(view[16] == '/');
}

CY_TEST_CASE("a lookup searches the tiers in order and a miss is not an error") {
    MemoryCacheTier local(cy::current_allocator(), "local", true);
    ShaderCache cache(cy::current_allocator());
    CY_REQUIRE(cache.add_tier(local).has_value());

    const CacheKey key = derive_cache_key(base_inputs());
    cy::Array<u8> out(cy::current_allocator());
    auto found = cache.load(key, out);
    CY_REQUIRE(found.has_value());
    CY_CHECK_FALSE(*found);
    CY_CHECK_EQ(cache.stats().misses, cy::u64{1});

    CY_REQUIRE(cache.store(key, bytes_of("spirv", 5)).has_value());
    found = cache.load(key, out);
    CY_REQUIRE(found.has_value());
    CY_CHECK(*found);
    CY_CHECK_EQ(out.size(), usize{5});
    CY_CHECK_EQ(cache.stats().hits, cy::u64{1});
    CY_CHECK_EQ(cache.stats().promotions, cy::u64{0});
}

CY_TEST_CASE("a hit in a remote tier is promoted into the local one") {
    // `shader-system`'s "CI populates, developers consume": the developer's first build fetches and
    // the second does not.
    MemoryCacheTier local(cy::current_allocator(), "local", true);
    MemoryCacheTier remote(cy::current_allocator(), "shared-remote", false);
    ShaderCache cache(cy::current_allocator());
    CY_REQUIRE(cache.add_tier(local).has_value());
    CY_REQUIRE(cache.add_tier(remote).has_value());

    const CacheKey key = derive_cache_key(base_inputs());
    CY_REQUIRE(remote.publish(key, bytes_of("from-ci", 7)).has_value());
    CY_CHECK_EQ(local.size(), usize{0});

    cy::Array<u8> out(cy::current_allocator());
    auto found = cache.load(key, out);
    CY_REQUIRE(found.has_value());
    CY_CHECK(*found);
    CY_CHECK_EQ(cache.stats().promotions, cy::u64{1});
    CY_CHECK_EQ(local.size(), usize{1});

    // The second lookup does not reach the remote tier.
    found = cache.load(key, out);
    CY_REQUIRE(found.has_value());
    CY_CHECK(*found);
    CY_CHECK_EQ(cache.stats().promotions, cy::u64{1});
}

CY_TEST_CASE("a read-only tier refuses a store and the writable ones still get it") {
    MemoryCacheTier local(cy::current_allocator(), "local", true);
    MemoryCacheTier remote(cy::current_allocator(), "shared-remote", false);
    ShaderCache cache(cy::current_allocator());
    CY_REQUIRE(cache.add_tier(local).has_value());
    CY_REQUIRE(cache.add_tier(remote).has_value());

    const CacheKey key = derive_cache_key(base_inputs());
    CY_REQUIRE(cache.store(key, bytes_of("spirv", 5)).has_value());
    CY_CHECK_EQ(local.size(), usize{1});
    CY_CHECK_EQ(remote.size(), usize{0});
    CY_CHECK_FALSE(remote.store(key, bytes_of("spirv", 5)).has_value());
}
