# Headless operation, as a configure-time refusal rather than as a comment.
#
# `gameplay-framework` — "Headless operation":
#
#   The gameplay framework SHALL be **fully functional with no renderer, no audio, no interface, and
#   no GPU**.
#
#   This SHALL be a requirement rather than a build configuration: a gameplay system that requires a
#   camera, viewport, material, or audio device SHALL be a defect.
#
#   ... the dedicated server SHALL link no rendering code.
#
# WHY THIS FILE EXISTS. src/gameplay/CMakeLists.txt used to carry the sentence "None of those is
# reachable from this dependency list, so the dedicated server links no rendering code because there
# is none to link." That was true when it was written and it was a CLAIM, not a check: the next
# author to add a dependency would not have read it, and nothing in the build would have objected.
# A requirement whose only enforcement is a comment is a requirement that holds until somebody is in
# a hurry.
#
# WHAT IT ACTUALLY CHECKS. The TRANSITIVE link closure, not the declared list — a forbidden module
# reached through three intermediaries is the same defect as one reached directly, and it is the one
# a review would miss. It runs deferred, at the end of the top-level directory's processing, because
# a target's closure is only complete once every module has been declared.
#
# HOW TO SEE IT FAIL. Add `cy::audio` to cy_gameplay's PUBLIC_DEPENDENCIES and configure: the
# configure stops and names the path by which audio became reachable.

# The modules a headless session must not need, and the list lives INSIDE the function that uses it.
#
# That is not a style choice. A CMake function sees the variable scope of its CALLER, not of the file
# that defined it — and this check is invoked from a deferred call in the top-level directory, where
# a variable set here in src/gameplay/'s scope does not exist. Written the other way, the list was
# empty at call time, every pattern failed to match, and the check printed "headless holds" over a
# closure containing the audio module. It was found by mutating the dependency list rather than by
# reading the code, which is the only way that class of defect is ever found.

# Walk `target`'s transitive link closure, appending every target name reached to `out_var` and, for
# each, the edge that reached it to `out_edges` as "<reached>|<parent>".
#
# ONE EDGE RATHER THAN A WHOLE PATH. A full path reads better in a diagnostic and needs a per-target
# variable whose name is built from a target spelling that may contain "::" — which CMake accepts
# and then resolves to nothing, producing a route that is quietly wrong. A wrong diagnostic in a
# gate is worse than a terse one, so this records the parent, which is always right.
function(cy_headless_closure target out_var out_edges)
    set(pending "${target}")
    set(seen "")
    set(edges "")

    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")

        if(NOT TARGET "${current}")
            continue()
        endif()
        # An ALIAS is a second name for one target; resolve it so the closure holds real names too.
        get_target_property(aliased "${current}" ALIASED_TARGET)
        if(aliased)
            list(APPEND pending "${aliased}")
            list(APPEND edges "${aliased}|${current}")
            continue()
        endif()

        set(links "")
        get_target_property(type "${current}" TYPE)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(direct "${current}" LINK_LIBRARIES)
            if(direct)
                list(APPEND links ${direct})
            endif()
        endif()
        get_target_property(interface "${current}" INTERFACE_LINK_LIBRARIES)
        if(interface)
            list(APPEND links ${interface})
        endif()

        foreach(link IN LISTS links)
            # Generator expressions and raw flags are not targets and carry no closure.
            if(link MATCHES "^\\$<" OR link MATCHES "^-")
                continue()
            endif()
            list(APPEND pending "${link}")
            list(APPEND edges "${link}|${current}")
        endforeach()
    endwhile()

    set(${out_var} "${seen}" PARENT_SCOPE)
    set(${out_edges} "${edges}" PARENT_SCOPE)
endfunction()

# Fail the configure when `target` can reach anything a headless session must not need.
function(cy_require_headless target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "cy_require_headless: no target '${target}'")
    endif()

    # Matched against the target name, so an ALIAS cannot smuggle one past by another spelling.
    set(CY_HEADLESS_FORBIDDEN
        "^cy_rendering"       # every rendering module, whatever its suffix
        "^cy_render"
        "^cy_rhi"
        "^cy_shader"
        "^cy_material"
        "^cy_audio"
        "^cy_backends_audio"
        "^cy_backends_viewport"
        "^cy_ui"
        "^cy_text"            # text-and-fonts is interface work
        "^cy_vfx"
        "^cy_camera"          # "a gameplay system that requires a camera ... SHALL be a defect"
        "^cy_denoising"
        "^cy_virtual_"
        "^cy_editor"
        "SDL"                 # the window and input backend, by any target spelling
        "vulkan"
        "volk"
        "Vulkan")

    cy_headless_closure("${target}" closure edges)

    set(violations "")
    foreach(reached IN LISTS closure)
        string(REPLACE "::" "_" plain "${reached}")
        foreach(pattern IN LISTS CY_HEADLESS_FORBIDDEN)
            if(plain MATCHES "${pattern}")
                set(parent "<the target itself>")
                foreach(entry IN LISTS edges)
                    string(FIND "${entry}" "|" separator)
                    string(SUBSTRING "${entry}" 0 ${separator} reached_name)
                    if(reached_name STREQUAL reached)
                        math(EXPR after "${separator} + 1")
                        string(SUBSTRING "${entry}" ${after} -1 parent)
                        break()
                    endif()
                endforeach()
                list(APPEND violations "  ${reached}\n      linked by: ${parent}")
                break()
            endif()
        endforeach()
    endforeach()

    if(violations)
        string(REPLACE ";" "\n" rendered "${violations}")
        message(FATAL_ERROR
            "`gameplay-framework` requires headless operation: '${target}' must be fully "
            "functional with no renderer, no audio, no interface and no GPU, and the dedicated "
            "server must link no rendering code.\n"
            "${rendered}\n"
            "  This is a requirement rather than a build configuration, so the fix is to move the "
            "code that needs a renderer, an audio device or an interface ABOVE this module — into "
            "the runtime or the sample — rather than to widen this list.\n"
            "  The list is in src/gameplay/cmake/headless.cmake.")
    endif()

    list(LENGTH closure reached_count)
    message(STATUS "gameplay: headless holds over ${target}'s link closure (${reached_count} targets)")
endfunction()
