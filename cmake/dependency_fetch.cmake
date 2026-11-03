# cmake/dependency_fetch.cmake — how deep cmake/dependencies.cmake clones each pinned dependency.
#
# Kept apart from dependencies.cmake so that tools/deps/test_fetch_depth.cmake can run the decision
# in script mode, with no project and no network.

include_guard(GLOBAL)

# Sets `out` to TRUE when a `--depth 1` clone of `repository` is able to check `tag`'s pin out.
#
# FetchContent's shallow clone is `git clone --depth 1 --no-single-branch` followed by a checkout
# of the pinned commit. That clone holds the tip of every branch and every tagged commit, and
# nothing else, so a commit no tag names is present only while it is still a branch tip. The
# manifest spells such a pin `<branch>@<abbrev>` or `unreleased-<abbrev>` (see deps/manifest.toml),
# and stb_vorbis showed what it costs: pinned at master's tip, it stopped configuring the day
# upstream pushed three commits on top. Those pins clone in full.
#
# A server must also allow the shallow fetch at all. GitHub does, and it is the difference between a
# few megabytes and several hundred for SDL and zstd. GitLab does not — the Eigen entry, pinned on
# gitlab.com, fails a shallow fetch with "Failed to checkout tag" — so anything not on GitHub clones
# in full too. The host decides, so the host is what this reads, rather than a list of names
# somebody has to remember to extend.
function(cy_dependency_fetch_shallow repository tag out)
    set(_shallow TRUE)
    if(NOT repository MATCHES "^https://github\\.com/")
        set(_shallow FALSE)
    elseif(tag MATCHES "^[^@]+@[0-9a-f]+$" OR tag MATCHES "^unreleased-[0-9a-f]+$")
        set(_shallow FALSE)
    endif()
    set(${out} ${_shallow} PARENT_SCOPE)
endfunction()
