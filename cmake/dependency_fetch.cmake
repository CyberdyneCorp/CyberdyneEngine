# cmake/dependency_fetch.cmake — whether a pinned dependency can be fetched shallowly.
#
# A module of its own so tools/deps/selftest.py can ask it the question in script mode (`cmake -P`)
# without configuring the dependency set. cmake/dependencies.cmake is its only other caller.

include_guard(GLOBAL)

# cy_dependency_fetch_is_shallow(<out> <repository> <tag>)
#
# Sets <out> to TRUE when a depth-one clone is certain to contain the pinned commit, FALSE when the
# whole history has to be fetched.
#
# A SHALLOW CLONE REACHES BRANCH TIPS AND TAGS, NOT A COMMIT. FetchContent's GIT_SHALLOW with a
# commit hash runs `git clone --depth 1 --no-single-branch` and then checks the hash out; it never
# asks the server for the commit itself. So the commit has to be the target of an upstream tag,
# which does not move, or the tip of a branch, which does. The manifest writes a pin with no
# upstream tag as `<branch>@<abbrev>` or `unreleased-<abbrev>`, and such a pin is fetchable shallowly
# only until its branch moves: nothings/stb's master moved on 2026-11-01, past the stb_vorbis pin,
# and every configure without a populated cache failed with "Failed to checkout tag". Those pins
# are fetched in full.
#
# GITLAB IS FETCHED IN FULL TOO. The Eigen entry is pinned at a commit on gitlab.com, and M8.c found
# a shallow fetch of it failing the same way. The host is what is read, rather than a list of names
# somebody has to remember to extend.
function(cy_dependency_fetch_is_shallow out repository tag)
    set(shallow TRUE)
    if(NOT repository MATCHES "^https://github\\.com/")
        set(shallow FALSE)
    elseif(tag MATCHES "@" OR tag MATCHES "^unreleased-")
        set(shallow FALSE)
    endif()
    set(${out} ${shallow} PARENT_SCOPE)
endfunction()
