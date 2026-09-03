#pragma once
// Privacy classification and the export policy that acts on it.
//
// `diagnostics-profiling-and-crash` — "Privacy classification": every field captured by
// diagnostics, logging or crash reporting carries a classification, and a field's classification
// determines whether it is included in an artefact that leaves the machine. design.md section 2
// makes the classification a required argument of the field macro rather than a review step; see
// field.h for the half of the rule the compiler enforces.

#include <cy/core/diagnostics/prelude.h>

namespace cy {

/// Ordered by increasing sensitivity, so a policy is a ceiling rather than a set.
enum class Privacy : diag::u8 {
    Public = 0,
    Developer = 1,
    PotentiallyPersonal = 2,
    Sensitive = 3,
    Secret = 4,
};

/// What classifications an artefact may contain. An artefact declares its policy in its header, so
/// a reader can tell what was excluded rather than guessing at a gap.
///
/// There is no widen(): a policy can only be tightened, which is what lets a project restrict what
/// its builds may emit without being able to loosen what the engine declared.
class ExportPolicy {
public:
    /// Everything the machine may see. Written to a developer's own disk.
    static constexpr ExportPolicy local() noexcept { return ExportPolicy(Privacy::Sensitive); }
    /// An artefact prepared to leave the machine. Excludes Sensitive, as Secret is excluded always.
    static constexpr ExportPolicy upload() noexcept {
        return ExportPolicy(Privacy::PotentiallyPersonal);
    }
    /// The tightest useful policy: identifiers, counts and timings only.
    static constexpr ExportPolicy public_only() noexcept { return ExportPolicy(Privacy::Public); }

    /// Secret is admitted by no policy at any ceiling. Credentials, tokens, private communications
    /// and personal files are never captured automatically, so the classification that names them
    /// has no path into any artefact.
    [[nodiscard]] constexpr bool allows(Privacy field) const noexcept {
        return field != Privacy::Secret &&
               static_cast<diag::u8>(field) <= static_cast<diag::u8>(ceiling_);
    }

    [[nodiscard]] constexpr ExportPolicy tighten(Privacy ceiling) const noexcept {
        return ExportPolicy(ceiling < ceiling_ ? ceiling : ceiling_);
    }

    [[nodiscard]] constexpr Privacy ceiling() const noexcept { return ceiling_; }

private:
    explicit constexpr ExportPolicy(Privacy ceiling) noexcept : ceiling_(ceiling) {}

    Privacy ceiling_;
};

/// The classification's name, for a report header or a diagnostic. Never used in the emission path.
const char* privacy_name(Privacy value) noexcept;

}  // namespace cy
