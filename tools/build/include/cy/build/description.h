#ifndef CY_BUILD_DESCRIPTION_H
#define CY_BUILD_DESCRIPTION_H
// The build description: `cybuild 1`, a graph as text. M6 task 7.1.
//
// A graph assembled only in C++ would be a graph only a C++ program can see, and
// `build-and-packaging` requires that "the graph SHALL be inspectable so that a developer can see
// why a node ran". So the graph has a written form, `cy_build` reads it, and the same file is what
// a test builds twice to check that two runs produce one set of bytes.
//
// The grammar shares its lexical conventions with `cy/core/serialize/text.h` and with the editor's
// `cyworld` — two-space indent, quoted names, `#` comments, one record per line:
//
//     cybuild 1
//     node "import:city" import "copy" 1
//       platform "host"
//       profile "client"
//       bundle "base"
//       source "assets/city.txt"
//       upstream "generate:types"
//       output "derived/city.bin"
//       option "quality" "high"
//       distributable false
//
// The node's four head words are its name, its kind, its producer and that producer's version. The
// version is on the node rather than looked up from the registry on purpose: a remote worker keys
// the node it was handed, and `build-and-packaging` requires a cooker's version increase to change
// the key of every node invoking it — which is checkable from the description alone.

#include <cy/build/graph.h>
#include <cy/build/producer.h>
#include <cy/core/base/expected.h>

#include <string>
#include <string_view>

namespace cy::build {

/// Parse a description into a finalised graph.
///
/// `producers` is optional and is consulted only for a node whose line omits its producer version:
/// a description read on a remote worker must be keyable WITHOUT the registry, because the worker
/// was handed the node and not the tool, so the version lives on the node once it is known.
///
/// Fails with `InvalidArgument` on a malformed record, `NotFound` on an unknown upstream or an
/// unknown producer, and `AlreadyExists` on a duplicate node name.
[[nodiscard]] Status read_description(std::string_view document, BuildGraph& out,
                                      const ProducerRegistry* producers = nullptr);

/// Write a graph back out. Round-trips: `read_description(write_description(g))` produces a graph
/// with the same nodes in the same order, which is what makes a description a thing a tool may
/// generate rather than only consume.
[[nodiscard]] std::string write_description(const BuildGraph& graph);

}  // namespace cy::build

#endif  // CY_BUILD_DESCRIPTION_H
