#ifndef CY_BUILD_SRC_TEXT_H
#define CY_BUILD_SRC_TEXT_H
// The one line-and-quoted-word reader every text format in this module uses. Private to
// tools/build/src/.
//
// Three formats are written here — the build description (`cybuild 1`), the package manifest
// (`cypackage 1`) and the patch manifest (`cypatch 1`) — and they share a lexical shape with
// `cy/core/serialize/text.h` and with the editor's `cyworld`: two-space indent, quoted names, `#`
// comments, one record per line. Sharing the reader rather than writing three is not tidiness: the
// package manifest is read back by the patcher and the patch manifest by the installer, and two
// parsers that disagreed about how a quote is escaped would disagree about a file name inside a
// shipped patch.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::build::text {

/// One line, split into its indent depth and its words. A quoted word keeps its spaces.
struct Line {
    u32 depth = 0;
    std::vector<std::string> words;
    u32 number = 0;

    [[nodiscard]] bool empty() const noexcept { return words.empty(); }
    [[nodiscard]] std::string_view word(usize index) const noexcept {
        return index < words.size() ? std::string_view(words[index]) : std::string_view();
    }
};

/// Split a document into lines. Blank lines and `#` comment lines are dropped; a line whose quoting
/// is unbalanced fails with `InvalidArgument` naming its number.
[[nodiscard]] Expected<std::vector<Line>, Error> read(std::string_view document);

/// Quote a word for writing: `"` and `\` are escaped and nothing else is, so the form round-trips
/// through `read` exactly.
[[nodiscard]] std::string quote(std::string_view word);

}  // namespace cy::build::text

#endif  // CY_BUILD_SRC_TEXT_H
