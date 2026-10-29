// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_TESTS_PLAY_REPORT_H
#define CY_SAMPLE_PLAY_TESTS_PLAY_REPORT_H
// play_report.h — reading a played sample's report in a test: the sample runs as its own process
// under the play host, its input a script pushed through the platform (input_script.h), and it
// prints one line per claim, `key=value` separated by spaces.

#include <cmath>
#include <cstdlib>
#include <string>

#include "process.h"

namespace cy::play::test {

/// The first line of `output` that contains `marker`, or empty.
[[nodiscard]] inline std::string line_with(const std::string& output, const char* marker) {
    const std::string::size_type found = output.find(marker);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type end = output.find('\n', found);
    return output.substr(found, end == std::string::npos ? end : end - found);
}

/// The number after ` key=` in `line` — the key preceded by a space, so `x=` is not found inside
/// `max=` — or a huge negative when the key is not there.
[[nodiscard]] inline double number_after(const std::string& line, const std::string& key) {
    const std::string spaced = " " + key + "=";
    const std::string::size_type found = line.find(spaced);
    if (found == std::string::npos) {
        return -1e30;
    }
    return std::strtod(line.c_str() + found + spaced.size(), nullptr);
}

/// The `index`th comma-separated number after ` key=`: `at=5.000,0.920,6.000`'s z is index 2.
[[nodiscard]] inline double component_after(const std::string& line, const std::string& key,
                                            int index) {
    const std::string spaced = " " + key + "=";
    std::string::size_type at = line.find(spaced);
    if (at == std::string::npos) {
        return -1e30;
    }
    at += spaced.size();
    for (int skipped = 0; skipped < index; ++skipped) {
        at = line.find(',', at);
        if (at == std::string::npos) {
            return -1e30;
        }
        ++at;
    }
    return std::strtod(line.c_str() + at, nullptr);
}

[[nodiscard]] inline bool near(double value, double expected, double tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

/// Run `sample` played headless — SDL's dummy video driver, no device, one step a frame — with
/// `script` as the hands, and anything else in `extra`.
[[nodiscard]] inline cy::test::smoke::ProcessResult play(const char* sample,
                                                         const std::string& script,
                                                         const std::string& extra = {}) {
    return cy::test::smoke::run(cy::test::smoke::quoted(sample) +
                                " --play --headless --fixed-clock --input-script " +
                                cy::test::smoke::quoted(script) + " " + extra + " 2>&1");
}

}  // namespace cy::play::test

#endif  // CY_SAMPLE_PLAY_TESTS_PLAY_REPORT_H
