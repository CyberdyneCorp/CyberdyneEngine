#pragma once
// Stable iteration and tie-breaking. Task 4.2.7.
//
// `simulation-and-determinism` — "Stable iteration and tie-breaking": queries declare their
// ordering requirement, every algorithm selecting among equal candidates declares a tie-break by
// stable identity, and iteration over containers whose order is unspecified — hash maps in
// particular — never determines authoritative results. "Lookup is permitted; iteration as a
// decision order is not."
//
// --- THE ONE THING THIS FILE MAKES UNSPELLABLE ---------------------------------------------------
//
// `select_best()` **has no overload without a tie-break**. The requirement is that every
// equal-candidate selection declares one; the way to make a project comply is not to check that it
// did, but to give it no way to ask the question without answering it. A `max_element` with a
// `<` comparator is the shape that produces "whichever the container happened to hold first", and
// it is exactly what is missing here.
//
// The identity a tie-break uses is the caller's, because only the caller knows which of its
// candidate's fields is stable: an entity index is, a pointer is not, a position in a `HashMap`'s
// bucket array is emphatically not. What this file can do is require one and use it, which it does.
//
// --- WHAT IS STILL ONLY A CONVENTION -------------------------------------------------------------
//
// Nothing here stops a system from writing its own loop over a `HashMap`. `Ordering` is a
// declaration a query carries so that the scheduler and the validator can read it; enforcing it is
// the chaos scheduler's and the lint's, both at M9. Said plainly rather than implied.

#include <cy/core/base/types.h>

namespace cy::determinism {

/// What a query says about the order it needs.
///
/// `Unspecified` is a claim, not an absence: the system asserts that its result does not depend on
/// the order, which is what lets the scheduler split the work any way it likes and what the chaos
/// scheduler will falsify if it is untrue.
enum class Ordering : u8 {
    Unspecified = 0,
    Stable = 1,
};

const char* ordering_name(Ordering ordering) noexcept;

/// The winner among `count` candidates, by score, with ties broken by stable identity.
///
/// `score(i)` returns something ordered; `identity(i)` returns a stable u64 — an entity index, a
/// manifest id, a rule number. `prefer_lower` picks the smallest score instead of the largest,
/// which covers path cost and the like without a second function whose tie-break could drift from
/// this one's. Returns `count` when there are no candidates.
///
/// Ties break towards the *lower* identity, always, in both directions of `prefer_lower`. A rule
/// that changed with the comparison direction would be two rules.
template <class Score, class Identity>
[[nodiscard]] constexpr usize select_best(usize count, Score score, Identity identity,
                                          bool prefer_lower = false) noexcept {
    if (count == 0) {
        return 0;
    }
    usize best = 0;
    auto best_score = score(usize{0});
    u64 best_identity = identity(usize{0});
    for (usize index = 1; index < count; ++index) {
        const auto candidate = score(index);
        const bool better = prefer_lower ? (candidate < best_score) : (best_score < candidate);
        const bool tied =
            !better && !(prefer_lower ? (best_score < candidate) : (candidate < best_score));
        if (better || (tied && identity(index) < best_identity)) {
            best = index;
            best_score = candidate;
            best_identity = identity(index);
        }
    }
    return best;
}

/// Sort `count` elements by a stable key, breaking ties by identity, using the caller's swap.
///
/// An insertion sort, deliberately: it is stable, it allocates nothing, it needs no comparator
/// object with a lifetime, and the sequences a simulation ties among — candidate targets, spawn
/// points, equal-cost path nodes — are short. A general sort belongs in `core/memory`; what belongs
/// here is one whose ordering is fully determined by the two functions it is given, with no
/// dependence on the input's initial arrangement.
template <class Key, class Identity, class Swap>
constexpr void sort_by_key(usize count, Key key, Identity identity, Swap swap) noexcept {
    for (usize i = 1; i < count; ++i) {
        for (usize j = i; j > 0; --j) {
            const auto left = key(j - 1);
            const auto right = key(j);
            const bool out_of_order =
                (right < left) || (!(left < right) && identity(j) < identity(j - 1));
            if (!out_of_order) {
                break;
            }
            swap(j - 1, j);
        }
    }
}

}  // namespace cy::determinism
