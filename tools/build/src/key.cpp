#include <cy/build/key.h>

#include <algorithm>
#include <ranges>

namespace cy::build {
assets::DerivedKind derived_kind_of(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Import:
            return assets::DerivedKind::Import;
        case NodeKind::Shader:
            return assets::DerivedKind::Shader;
        case NodeKind::Cook:
        case NodeKind::Package:
            return assets::DerivedKind::Page;
        case NodeKind::Generate:
        case NodeKind::Manifest:
            return assets::DerivedKind::Metadata;
        case NodeKind::Unknown:
            break;
    }
    return assets::DerivedKind::Unknown;
}

namespace {

void contribute_digests(assets::DerivationKeyBuilder& builder, const char* prefix,
                        std::vector<KeyedDigest> entries) {
    // Sorted by logical name, here and nowhere else. §1.2: contributions are hashed in order, so
    // order is part of the contract; sorting at the single point that builds a key is what makes
    // the contract hold without every caller having to know about it.
    std::ranges::sort(entries,
                      [](const KeyedDigest& a, const KeyedDigest& b) { return a.name < b.name; });
    for (const KeyedDigest& entry : entries) {
        std::string name = prefix;
        name += entry.name;
        builder.source(name, entry.hash);
    }
}

}  // namespace

Expected<assets::DerivationKey, Error> derivation_key(const KeyInputs& inputs) {
    if (inputs.node == nullptr) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no node to key", 0});
    }
    if (inputs.toolchain == nullptr || !toolchain_is_complete(*inputs.toolchain)) {
        // Refused rather than defaulted. A key computed without the toolchain is the exact defect
        // design.md §1.3 demonstrated on the real importer, and defaulting would reintroduce it
        // silently the first time a caller forgot the field.
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a derivation key needs a complete toolchain", 0});
    }

    const NodeDesc& node = *inputs.node;
    assets::DerivationKeyBuilder builder;
    builder.producer(derived_kind_of(node.kind), node.producer, node.producer_version);
    builder.text("node.kind", node_kind_name(node.kind));
    inputs.toolchain->contribute(builder);
    builder.text("target.platform", node.platform);
    builder.text("cook.profile", node.profile);

    contribute_digests(builder, "source:", inputs.sources);
    contribute_digests(builder, "upstream:", inputs.upstreams);

    // Options arrive sorted — `BuildGraph::add` sorts them — and are contributed under their own
    // names, so an option that is added but never read still changes the key. That is deliberate:
    // "an option absent from the key silently does nothing" is the failure §1.2 tabulates.
    for (const NodeOption& option : node.options) {
        std::string name = "option:";
        name += option.name;
        builder.text(name, option.value);
    }

    // The declared output names, sorted. Adding an output to a node changes what the node IS, and a
    // build that served the old artefact for the new declaration would report a hit for a node that
    // has never produced the output the downstream one is about to read.
    std::vector<std::string> outputs = node.outputs;
    std::ranges::sort(outputs);
    for (const std::string& output : outputs) {
        builder.text("output", output);
    }

    return builder.finish();
}

assets::ContentHash result_digest(
    const std::vector<std::pair<std::string, assets::ContentHash>>& outputs) noexcept {
    std::vector<std::pair<std::string, assets::ContentHash>> sorted = outputs;
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first < b.first; });

    // Framed the way a key is framed, and for the same reason: an unframed concatenation of names
    // and digests lets two different output sets produce one digest, which would defeat early
    // cutoff by making two different results look identical.
    assets::ContentHasher hasher;
    const u64 count = sorted.size();
    hasher.update(&count, sizeof(count));
    for (const auto& entry : sorted) {
        const u64 length = entry.first.size();
        hasher.update(&length, sizeof(length));
        hasher.update(entry.first.data(), entry.first.size());
        hasher.update(entry.second.bytes, assets::ContentHash::kByteLength);
    }
    return hasher.finish();
}

}  // namespace cy::build
