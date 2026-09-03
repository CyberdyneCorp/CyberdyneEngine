// Running another program and reading what it said. Used by both smoke tests.
//
// A smoke test is the one kind that runs a whole binary rather than linking part of one, so this is
// the shape every test in this directory needs: a command, its output, and its exit code. It lives
// here rather than in tests/harness/ because it is specific to that kind — a unit test that spawns
// a process is a unit test in the wrong suite.

#ifndef CY_TEST_SMOKE_PROCESS_H
#define CY_TEST_SMOKE_PROCESS_H

#include <cstdio>
#include <string>

#if defined(_WIN32)
#    define CY_SMOKE_POPEN _popen
#    define CY_SMOKE_PCLOSE _pclose
#else
#    include <sys/wait.h>
#    define CY_SMOKE_POPEN popen
#    define CY_SMOKE_PCLOSE pclose
#endif

namespace cy::test::smoke {

struct ProcessResult {
    bool ran = false;
    int exit_code = -1;
    std::string output;
};

/// What pclose() reports, as the exit code a reader expects. On POSIX it is a wait status and a
/// process killed by a signal has no exit code of its own, so it is reported as 128 + the signal —
/// the shell's convention, and enough to tell a crash from a clean failure.
inline int decode_exit_status(int raw) {
#if defined(_WIN32)
    return raw;
#else
    if (WIFEXITED(raw)) {
        return WEXITSTATUS(raw);
    }
    if (WIFSIGNALED(raw)) {
        return 128 + WTERMSIG(raw);
    }
    return -1;
#endif
}

/// Run `command`, capturing its standard output. Standard error is left on the test's own, so a
/// failing child's diagnostics appear in the test log rather than being swallowed.
inline ProcessResult run(const std::string& command) {
    ProcessResult result;
    // The command is a build-generated path to the binary under test plus this test's own
    // arguments; running it is what a smoke test is. NOLINT because clang-tidy is right in general
    // and wrong here, and silencing it in the .clang-tidy would silence it for the whole engine.
    // NOLINTNEXTLINE(bugprone-command-processor)
    std::FILE* pipe = CY_SMOKE_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }

    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }

    result.exit_code = decode_exit_status(CY_SMOKE_PCLOSE(pipe));
    result.ran = true;
    return result;
}

/// A path or argument, quoted for the shell popen() hands the command to.
inline std::string quoted(const std::string& text) {
    return "\"" + text + "\"";
}

}  // namespace cy::test::smoke

#endif  // CY_TEST_SMOKE_PROCESS_H
