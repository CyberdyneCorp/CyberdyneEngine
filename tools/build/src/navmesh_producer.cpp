// SPDX-License-Identifier: MIT
// `navmesh` as a graph node: a saved navigation bake in, a cooked navigation mesh out. Issue #28,
// task 2.4 of `implement-issue-28-navigation-authoring`.
//
// ================================================================================================
// WHAT THE NODE IS
// ================================================================================================
//
// The editor never bakes. The engine's navigation service bakes a world and the runtime host saves
// the result as a content-addressed sidecar, `navigation/<bake identity>.cynavmesh`, while the
// world document records that identity on its NavigationWorld component. A cook declares one
// node per navigation world:
//
//     node "navmesh:level" cook "navmesh"
//       source "navigation/<identity>.cynavmesh"
//       output "derived/level.navmesh"
//       option "bake-identity" "<identity>"
//
// The producer decodes the sidecar with `decode_nav_bake`, which re-derives every tile digest and
// the stored bake identity and refuses any mismatch, then refuses a sidecar whose identity is not
// the one the world names. A sidecar that passes is the cooked asset: the `.cynavmesh` format is
// what the runtime installs with `install_nav_bake`, so the output is its verified bytes.
//
// THE KEY. The sidecar is a declared source and `bake-identity` a declared option, so the node's
// derivation key covers the sidecar's content digest, the identity and `kNavmeshProducerVersion`.
// A rebake is a new sidecar and a new identity, so it is a new key; an unchanged world is served
// from the artefact store; a producer version bump re-cooks every navigation mesh.

#include "navmesh_producer.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/bake_codec.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>

namespace cy::build {
namespace {

/// The identity option: decimal, or hexadecimal with a `0x` prefix.
[[nodiscard]] Expected<u64, Error> parse_identity(std::string_view text) {
    if (text.empty()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a navmesh node names its world's bake identity"});
    }
    const std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(owned.c_str(), &end, 0);
    if (errno != 0 || end == nullptr || *end != '\0' || owned.front() == '-') {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the bake identity is not an unsigned integer"});
    }
    return static_cast<u64>(value);
}

}  // namespace

Status produce_navmesh(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a navmesh node declares exactly one sidecar and one output");
    }
    const std::string& sidecar = node.sources.front();
    const Expected<u64, Error> expected = parse_identity(node.option("bake-identity"));
    if (!expected) {
        context.diagnose(Severity::Error, "navmesh-identity", expected.error().message, node.name);
        return make_unexpected(expected.error());
    }
    Array<u8> bytes(default_allocator());
    if (Status read = context.read(sidecar, bytes); !read) {
        return read;
    }
    const auto asset = navigation::decode_nav_bake(default_allocator(), bytes.span());
    if (!asset) {
        context.diagnose(Severity::Error, "navmesh-sidecar", asset.error().message, sidecar);
        return make_unexpected(asset.error());
    }
    if (asset->bake_identity != *expected) {
        const Error mismatch{ErrorCode::InvalidArgument,
                             "the sidecar's bake identity differs from the world's"};
        context.diagnose(Severity::Error, "navmesh-identity", mismatch.message, sidecar);
        return make_unexpected(mismatch);
    }
    return context.write(node.outputs.front(), bytes.data(), bytes.size());
}

}  // namespace cy::build
