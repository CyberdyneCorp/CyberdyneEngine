// The M4 milestone gate, as a test. Tasks 5.3 and 5.4.
//
// It runs samples/04-character headless with a fixed tick budget and asserts three things that no
// other suite can:
//
//   1. THE ARTEFACT WORKS. A character written in Swift moves, jumps, is lifted onto a step, is
//      blocked by a wall, is heard, and is followed by a camera — measured off the run's own report
//      rather than off a screenshot, because this sample draws nothing (main.cpp says why).
//
//   2. THE GAMEPLAY IS IN SWIFT.  **This is task 5.3's real check.** The same binary is run again
//      with `--no-behaviours`, which loads the same module, builds the same level out of it, brings
//      up the same five servers and runs the same scripted input through the same command stream —
//      and creates neither deciding behaviour. Every line of C++ in the sample runs. Nothing moves.
//
//      A static check can only fail on the words somebody thought to forbid;
//      samples/04-character/tools/check_no_cpp_gameplay.py is that check and is the weaker half.
//      This one fails on any decision at all having leaked to the C++ side, whatever it is called.
//      The two runs differ in exactly one argument, which is what makes the comparison a control
//      rather than two separate observations.
//
//   3. IT REPRODUCES. Two runs of the same command produce byte-identical reports — the same
//      positions, the same log hash, the same input hash. `physics` requires determinism across
//      runs on one platform and this is the artefact's own instance of it, over a character
//      controller, an input pipeline, a command log and an audio mixer at once.
//
// WHY THE ASSERTIONS ARE INEQUALITIES AND NOT POSITIONS. What the character's position IS after
// four hundred ticks is a property of the reference backend's bounding-volume collision, and Jolt
// would answer differently in the last digit while playing identically — `physics`' whole point. So
// this file asserts what the requirement says (it moved, it left the ground, it was blocked, it
// made a sound) and case 3 asserts that whatever the answer was, it was the same answer twice.

#include <cy/test/test.h>

#include <cstdlib>
#include <string>

#include "process.h"

namespace {

/// A short run: past the jump at tick 250 and up the first steps, inside the smoke budget.
constexpr const char* kTicks = " --ticks 400";

[[nodiscard]] std::string line_with(const std::string& output, const char* marker) {
    const std::string::size_type found = output.find(marker);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type end = output.find('\n', found);
    return output.substr(found, end == std::string::npos ? end : end - found);
}

/// The number that follows `key=` on `line`, or a sentinel a caller can tell apart from a real
/// zero.
[[nodiscard]] double number_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return -1e30;
    }
    return std::strtod(line.c_str() + found + std::char_traits<char>::length(key), nullptr);
}

struct Run {
    cy::test::smoke::ProcessResult process;
    std::string module_line;
    std::string input_line;
    std::string motion_line;
    std::string ground_line;
    std::string audio_line;
    std::string camera_line;

    [[nodiscard]] bool parsed() const noexcept {
        return !module_line.empty() && !input_line.empty() && !motion_line.empty() &&
               !ground_line.empty() && !audio_line.empty() && !camera_line.empty();
    }
};

[[nodiscard]] Run run_sample(const std::string& arguments) {
    Run run;
    run.process = cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_CHARACTER) + arguments);
    run.module_line = line_with(run.process.output, "module   behaviours=");
    run.input_line = line_with(run.process.output, "input    committed=");
    run.motion_line = line_with(run.process.output, "motion   ticks=");
    run.ground_line = line_with(run.process.output, "ground   grounded=");
    run.audio_line = line_with(run.process.output, "audio    footsteps=");
    run.camera_line = line_with(run.process.output, "camera   views=");
    return run;
}

}  // namespace

CY_TEST_CASE("samples/04-character runs headless, plays itself, and exits cleanly") {
    const Run run = run_sample(kTicks);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    // The module loaded and registered all three behaviours, and its level reached physics.
    CY_CHECK_EQ(number_after(run.module_line, "behaviours="), 3.0);
    CY_CHECK(number_after(run.module_line, "level=") >= 5.0);

    // Every tick produced exactly one committed command and no rejection: one input path, and the
    // structural validation accepted it. A rejection here would mean the control binding is wrong.
    CY_CHECK_EQ(number_after(run.input_line, "committed="), 400.0);
    CY_CHECK_EQ(number_after(run.input_line, "rejected="), 0.0);

    // It moved. The script walks it into a wall, up four steps and back, so several metres is a
    // floor rather than a figure — see the header on why this is an inequality.
    CY_CHECK(number_after(run.motion_line, "travelled=") > 5.0);
    // It jumped, and the jump is the one whose key went down and up inside a single tick window.
    // A level-sampling input resolver reports no jump at all and this is zero.
    CY_CHECK(number_after(run.audio_line, "jumps=") >= 1.0);
    CY_CHECK(number_after(run.ground_line, "airborne=") > 0.0);
    // It collided with the level: it stood on the floor and something stopped it.
    CY_CHECK(number_after(run.ground_line, "grounded=") > 100.0);
    CY_CHECK(number_after(run.ground_line, "wall=") > 0.0);
    // It was heard.
    CY_CHECK(number_after(run.audio_line, "footsteps=") >= 3.0);
    CY_CHECK(number_after(run.audio_line, "voices=") >= 3.0);
    CY_CHECK(number_after(run.audio_line, "frames=") > 0.0);
    // And a camera followed it: one render view produced per tick, and the camera itself moved.
    CY_CHECK_EQ(number_after(run.camera_line, "views="), 400.0);
    CY_CHECK(number_after(run.camera_line, "travelled=") > 1.0);
}

CY_TEST_CASE("samples/04-character: with the Swift behaviours absent, nothing decides anything") {
    // TASK 5.3, MEASURED. Both runs load the module, build the level from it, resolve the contract,
    // bring up five servers and push four hundred ticks of the same scripted input through the same
    // command stream. They differ in one argument.
    const Run played = run_sample(kTicks);
    const Run control = run_sample(std::string(kTicks) + " --no-behaviours");
    CY_REQUIRE(played.process.ran);
    CY_REQUIRE(control.process.ran);
    CY_REQUIRE_EQ(control.process.exit_code, 0);
    CY_REQUIRE(control.parsed());

    // The level is still there, so "it did not move" is not "there was nothing to move on".
    CY_CHECK_EQ(number_after(control.module_line, "behaviours="), 1.0);
    CY_CHECK_EQ(number_after(control.module_line, "level="),
                number_after(played.module_line, "level="));

    // THE C++ RAN IDENTICALLY. Same commands committed, same rejections, same input hash: the input
    // pipeline, the command stream and the log did exactly what they did in the played run.
    CY_CHECK_EQ(number_after(control.input_line, "committed="),
                number_after(played.input_line, "committed="));
    CY_CHECK_EQ(number_after(control.input_line, "rejected="), 0.0);
    CY_CHECK_EQ(line_with(control.process.output, "input    committed="),
                line_with(played.process.output, "input    committed="));

    // AND NOTHING HAPPENED. No motion, no jump, no sound, no camera movement — because every one of
    // those was a decision, and the two objects that make decisions were not created.
    CY_CHECK_EQ(number_after(control.motion_line, "travelled="), 0.0);
    CY_CHECK_EQ(number_after(control.ground_line, "grounded="), 0.0);
    CY_CHECK_EQ(number_after(control.ground_line, "airborne="), 0.0);
    CY_CHECK_EQ(number_after(control.audio_line, "footsteps="), 0.0);
    CY_CHECK_EQ(number_after(control.audio_line, "landings="), 0.0);
    CY_CHECK_EQ(number_after(control.audio_line, "jumps="), 0.0);
    CY_CHECK_EQ(number_after(control.audio_line, "voices="), 0.0);
    CY_CHECK_EQ(number_after(control.camera_line, "travelled="), 0.0);
    // The camera and the mixer still RAN — one view per tick and the same number of audio frames as
    // the played run — so "nothing moved" is a statement about decisions and not about the host
    // having quietly skipped its own work.
    CY_CHECK_EQ(number_after(control.camera_line, "views="),
                number_after(played.camera_line, "views="));
    CY_CHECK_EQ(number_after(control.audio_line, "frames="),
                number_after(played.audio_line, "frames="));
    // THERE IS NO CHARACTER AT ALL in the control, and that is the sharpest form of the claim: the
    // capsule's radius, height, step offset and spawn are the game's numbers, so with the game
    // absent the host has nothing to build a body out of and does not invent one.

    // The control is only a control if the played run differs, so the difference is asserted here
    // as well as in the case above: a change that stopped the character moving would otherwise make
    // this whole test pass by agreeing with itself.
    CY_CHECK(number_after(played.motion_line, "travelled=") > 5.0);
    CY_CHECK(number_after(played.audio_line, "voices=") > 0.0);
}

CY_TEST_CASE("samples/04-character reproduces exactly across two runs") {
    // `physics`' "Determinism across runs on one platform", over the whole artefact rather than
    // over one subsystem: the same input, the same command log hash, the same character position
    // and the same camera position, in two separate processes.
    const Run first = run_sample(kTicks);
    const Run second = run_sample(kTicks);
    CY_REQUIRE(first.process.ran);
    CY_REQUIRE(second.process.ran);
    CY_REQUIRE(first.parsed());
    CY_REQUIRE(second.parsed());

    CY_CHECK_EQ(first.input_line, second.input_line);
    CY_CHECK_EQ(first.motion_line, second.motion_line);
    CY_CHECK_EQ(first.ground_line, second.ground_line);
    CY_CHECK_EQ(first.audio_line, second.audio_line);
    CY_CHECK_EQ(first.camera_line, second.camera_line);
}
