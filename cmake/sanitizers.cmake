# cmake/sanitizers.cmake — CY_SANITIZE wiring for AddressSanitizer, UndefinedBehaviorSanitizer and
# ThreadSanitizer.
#
# Task 4.2.3. `testing-and-quality` requires CI to run the unit and integration suites under ASan,
# UBSan and TSan at least nightly, and `build-system-and-platforms` makes CY_SANITIZE the option that
# selects them.
#
# CY_SANITIZE is declared by cmake/features.cmake — it is part of the option set that file owns, and
# it reaches the generated headers from there. This file reads it and must not redeclare it, which is
# why the top-level CMakeLists.txt includes features before sanitizers. Both run before the tree is
# added, because the flags selected here are usage requirements of every engine target.
#
# The flags are directory-scope (add_compile_options / add_link_options at the top level), not usage
# requirements of cy::compile-options, and that is deliberate for the same reason -fno-exceptions is:
# a sanitized build must sanitize *everything* in the binary, including the fetched dependencies. A
# runtime built without the instrumentation and linked into one built with it produces false reports
# and missed ones, which is worse than not running the tool.

include_guard(GLOBAL)

# What each name means, so that a diagnostic can say more than "unknown".
set(CY_SANITIZER_address   "AddressSanitizer: use-after-free, buffer overflow, double free")
set(CY_SANITIZER_leak      "LeakSanitizer: allocations still live at exit")
set(CY_SANITIZER_undefined "UndefinedBehaviorSanitizer: signed overflow, bad shifts, misaligned access")
set(CY_SANITIZER_thread    "ThreadSanitizer: data races and lock-order inversions")
set(CY_SANITIZER_memory    "MemorySanitizer: reads of uninitialised memory (Clang only)")

set(CY_SANITIZER_NAMES address leak undefined thread memory)

# ThreadSanitizer maintains its own shadow memory and cannot coexist with the others; MemorySanitizer
# is the same, and additionally requires every library in the process to be instrumented.
set(CY_SANITIZER_EXCLUSIVE thread memory)

function(_cy_sanitizer_reject reason)
    message(FATAL_ERROR
        "CY_SANITIZE=\"${CY_SANITIZE}\": ${reason}\n"
        "  Available: address, leak, undefined, thread, memory — comma separated.\n"
        "  address and undefined combine and are what CI runs over the unit and integration "
        "suites; thread and memory each run alone.\n"
        "  Example: cmake --preset dev -DCY_SANITIZE=address,undefined")
endfunction()

# Split, validate and return the requested set.
function(_cy_sanitizer_parse out)
    string(REPLACE "," ";" requested "${CY_SANITIZE}")
    list(REMOVE_ITEM requested "")
    list(REMOVE_DUPLICATES requested)

    foreach(name IN LISTS requested)
        if(NOT name IN_LIST CY_SANITIZER_NAMES)
            _cy_sanitizer_reject("'${name}' is not a sanitizer this build knows")
        endif()
    endforeach()

    foreach(exclusive IN LISTS CY_SANITIZER_EXCLUSIVE)
        list(LENGTH requested count)
        if(exclusive IN_LIST requested AND count GREATER 1)
            _cy_sanitizer_reject(
                "'${exclusive}' cannot be combined with another sanitizer — it replaces the "
                "allocator and the memory mapping, and two of those in one process report on each "
                "other rather than on the engine. Build it on its own")
        endif()
    endforeach()

    set(${out} "${requested}" PARENT_SCOPE)
endfunction()

function(cy_apply_sanitizers)
    if(NOT CY_SANITIZE)
        return()
    endif()

    _cy_sanitizer_parse(requested)

    foreach(name IN LISTS requested)
        message(STATUS "sanitizer ${name}: ${CY_SANITIZER_${name}}")
    endforeach()

    # A sanitized shipping build measures nothing anyone ships and hides the optimiser's own
    # behaviour behind a 2-20x slowdown. Not an error: reproducing a report against release
    # optimisation is exactly when it is worth doing on purpose.
    if(CMAKE_BUILD_TYPE STREQUAL "Shipping" OR CMAKE_BUILD_TYPE STREQUAL "Profile")
        message(WARNING
            "A sanitizer is enabled in the ${CMAKE_BUILD_TYPE} configuration. Measurements from "
            "this build are not shipping numbers; use --profile debug or dev unless you are "
            "reproducing a report against release optimisation.")
    endif()

    if(MSVC)
        _cy_apply_msvc_sanitizers("${requested}")
        # ASan on MSVC is incompatible with the runtime checks a debug configuration turns on by
        # default. Stripped here rather than in the function above, because the flags variable must
        # be set in the directory's scope and this function is the one the directory calls.
        string(REGEX REPLACE "/RTC[1csu]*" "" debug_flags "${CMAKE_CXX_FLAGS_DEBUG}")
        set(CMAKE_CXX_FLAGS_DEBUG "${debug_flags}" PARENT_SCOPE)
        return()
    endif()

    string(REPLACE ";" "," joined "${requested}")

    # -fno-omit-frame-pointer and -g are what turn a report into a stack trace with line numbers; a
    # sanitizer that fires and names four hex addresses has told you almost nothing.
    set(flags "-fsanitize=${joined}" -fno-omit-frame-pointer -g)

    if("address" IN_LIST requested)
        # Tail-call elimination removes the frame the allocation was made in, which is the frame the
        # report needs most.
        list(APPEND flags -fno-optimize-sibling-calls)
    endif()

    if("undefined" IN_LIST requested)
        # By default UBSan prints and continues, so a CI run finds undefined behaviour and still
        # exits zero. `testing-and-quality` requires the run to fail, so the check traps instead.
        list(APPEND flags -fno-sanitize-recover=undefined)
        # GCC defines no macro for UndefinedBehaviorSanitizer the way it does __SANITIZE_ADDRESS__,
        # and __has_feature is Clang's. A test that must know — the crash probe, whose deliberate
        # null store UBSan diagnoses before the hardware faults — reads this instead, so the answer
        # comes from the build that made the decision rather than from a compiler-specific guess.
        add_compile_definitions(CY_SANITIZE_UNDEFINED=1)
    endif()

    add_compile_options(${flags})
    add_link_options(${flags})

    if("thread" IN_LIST requested)
        # ThreadSanitizer maps its shadow at fixed addresses and refuses to start when the kernel's
        # ASLR entropy puts the executable where it wants to be — "FATAL: ThreadSanitizer:
        # unexpected memory mapping" on a recent Linux with vm.mmap_rnd_bits at 32. Verified here on
        # 6.8, where the run works under `setarch $(uname -m) -R`. Reported at configure time
        # because the failure otherwise looks like a defect in the engine.
        message(STATUS
            "ThreadSanitizer: if a run aborts with \"unexpected memory mapping\", the kernel's "
            "ASLR entropy is too high for its shadow mapping. Run the binary under "
            "`setarch $(uname -m) -R`, or lower vm.mmap_rnd_bits to 28.")
    endif()

    message(STATUS "CY_SANITIZE=${CY_SANITIZE} — instrumenting every target in this build, "
                   "dependencies included")
endfunction()

# MSVC ships AddressSanitizer only. The others are not "not yet wired": cl.exe has no /fsanitize for
# them, so a build asking for one must fail here rather than silently produce an uninstrumented
# binary that CI will report as clean.
function(_cy_apply_msvc_sanitizers requested)
    foreach(name IN LISTS requested)
        if(NOT name STREQUAL "address")
            _cy_sanitizer_reject(
                "MSVC implements AddressSanitizer only; '${name}' has no /fsanitize equivalent. "
                "Run it under Clang or on Linux")
        endif()
    endforeach()

    # Incremental linking is the other incompatibility, and it is on by default in Debug.
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO)
    message(STATUS "CY_SANITIZE=address — /fsanitize=address, runtime checks and incremental "
                   "linking disabled")
endfunction()

cy_apply_sanitizers()
