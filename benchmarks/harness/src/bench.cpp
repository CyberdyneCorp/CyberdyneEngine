// The benchmark runner: registration, the timing loop, the calibration, and the results file that
// benchmarks/tools/compare.py turns into a pass or a failure.
//
// Two decisions are worth stating here, because both are what make a threshold checkable at all.
//
// The measurement is the MINIMUM of several repetitions, not the mean. The distribution of a
// microbenchmark is one-sided: nothing makes the work faster than it is, and everything else on the
// machine makes some repetitions slower. The minimum is the estimate of the work; the mean is an
// estimate of the machine's load.
//
// Every result is also reported as a RATIO against a calibration workload measured in the same
// process. An absolute nanosecond figure is a property of the machine that produced it, so a
// committed baseline of nanoseconds can only be compared against the one machine it was recorded
// on — which is how a threshold becomes noise and then becomes ignored. The ratio divides the
// machine out for anything ALU- or latency-bound. It does not divide out cache or memory
// behaviour: a benchmark dominated by memory traffic needs a baseline recorded on the reference
// machine, and benchmarks/README.md says so.

#include <cy/bench/bench.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cy::bench {
namespace {

struct Entry {
    const char* name;
    const char* description;
    Body body;
};

// A function-local static, not a namespace-scope vector: registration happens during static
// initialisation, and the order of that across translation units is unspecified.
std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The calibration workload: a dependent multiply-add chain. Latency-bound, no memory traffic, and
/// the compiler cannot vectorise it away, so its cost is a property of the core rather than of the
/// build.
void calibration_workload(std::uint64_t iterations) {
    std::uint64_t state = 1;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
    }
    keep(state);
}

struct Options {
    double min_time_ms = 20.0;
    int reps = 5;
    std::string filter;
    std::string json_path;
    bool list = false;
};

struct Result {
    const char* name;
    const char* description;
    std::uint64_t iterations;
    double ns_per_op;
    double ratio;
};

/// Time one body at a fixed iteration count, in nanoseconds per iteration.
double time_once(Body body, std::uint64_t iterations) {
    const double start = now_seconds();
    body(iterations);
    const double elapsed = now_seconds() - start;
    return elapsed * 1e9 / static_cast<double>(iterations);
}

/// Choose an iteration count that takes at least min_time_ms, then measure it reps times and keep
/// the fastest.
double measure(Body body, double min_time_ms, int reps, std::uint64_t& iterations_out) {
    constexpr std::uint64_t kMaxIterations = 1ULL << 32;
    std::uint64_t iterations = 1024;
    double elapsed_ms = 0.0;

    while (iterations < kMaxIterations) {
        const double start = now_seconds();
        body(iterations);
        elapsed_ms = (now_seconds() - start) * 1e3;
        if (elapsed_ms >= min_time_ms) {
            break;
        }
        // Grow towards the target with a margin, and never by less than doubling: a run too short
        // to measure gives a ratio too noisy to extrapolate from.
        const double growth = elapsed_ms > 0.0 ? (min_time_ms / elapsed_ms) * 1.4 : 100.0;
        iterations = static_cast<std::uint64_t>(static_cast<double>(iterations) *
                                                std::max(2.0, std::min(growth, 1000.0)));
    }

    double best = time_once(body, iterations);
    for (int rep = 1; rep < reps; ++rep) {
        best = std::min(best, time_once(body, iterations));
    }
    iterations_out = iterations;
    return best;
}

void write_json_string(std::FILE* out, const char* text) {
    std::fputc('"', out);
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            std::fputc('\\', out);
        }
        std::fputc(*c, out);
    }
    std::fputc('"', out);
}

bool write_json(const Options& options, double calibration_ns, std::uint64_t calibration_iterations,
                const std::vector<Result>& results) {
    std::FILE* out = std::fopen(options.json_path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "cy::bench: cannot write %s\n", options.json_path.c_str());
        return false;
    }

    std::fprintf(out, "{\n  \"min_time_ms\": %.3f,\n  \"reps\": %d,\n", options.min_time_ms,
                 options.reps);
    std::fprintf(out, "  \"calibration\": { \"ns_per_op\": %.6f, \"iterations\": %llu },\n",
                 calibration_ns, static_cast<unsigned long long>(calibration_iterations));
    std::fprintf(out, "  \"benchmarks\": [\n");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& result = results[i];
        std::fprintf(out, "    { \"name\": ");
        write_json_string(out, result.name);
        std::fprintf(out, ", \"description\": ");
        write_json_string(out, result.description);
        std::fprintf(out, ", \"iterations\": %llu, \"ns_per_op\": %.6f, \"ratio\": %.6f }%s\n",
                     static_cast<unsigned long long>(result.iterations), result.ns_per_op,
                     result.ratio, i + 1 == results.size() ? "" : ",");
    }
    std::fprintf(out, "  ]\n}\n");
    std::fclose(out);
    return true;
}

void print_usage(const char* program) {
    std::printf(
        "usage: %s [options]\n"
        "  --json <file>        write the machine-readable results compare.py reads\n"
        "  --filter <substring> run only benchmarks whose name contains <substring>\n"
        "  --min-time-ms <ms>   minimum time for one repetition (default 20)\n"
        "  --reps <n>           repetitions per benchmark, fastest wins (default 5)\n"
        "  --list               list the benchmarks and what each measures, and exit\n"
        "  --help               this text\n"
        "\n"
        "Measuring is all this program does. `just test-bench` runs it and then compares the\n"
        "results against benchmarks/baseline.json, which is where the thresholds live.\n",
        program);
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argument, "--help") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        } else if (std::strcmp(argument, "--list") == 0) {
            options.list = true;
        } else if (std::strcmp(argument, "--json") == 0 && has_value) {
            options.json_path = argv[++i];
        } else if (std::strcmp(argument, "--filter") == 0 && has_value) {
            options.filter = argv[++i];
        } else if (std::strcmp(argument, "--min-time-ms") == 0 && has_value) {
            options.min_time_ms = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argument, "--reps") == 0 && has_value) {
            options.reps = std::max(1, static_cast<int>(std::strtol(argv[++i], nullptr, 10)));
        } else {
            std::fprintf(stderr, "cy::bench: unrecognised argument '%s'\n\n", argument);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

Registration::Registration(const char* name, const char* description, Body body) {
    registry().push_back(Entry{.name = name, .description = description, .body = body});
}

}  // namespace cy::bench

int main(int argc, char** argv) {
    using namespace cy::bench;

    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    if (options.list) {
        for (const Entry& entry : registry()) {
            std::printf("%s\n    %s\n", entry.name, entry.description);
        }
        return 0;
    }

    if (registry().empty()) {
        std::fprintf(stderr, "cy::bench: no benchmarks are registered in this binary.\n");
        return 1;
    }

    std::uint64_t calibration_iterations = 0;
    const double calibration_ns =
        measure(&calibration_workload, options.min_time_ms, options.reps, calibration_iterations);
    std::printf("calibration  %.3f ns/op over %llu iterations (a dependent multiply-add chain)\n\n",
                calibration_ns, static_cast<unsigned long long>(calibration_iterations));

    std::printf("%-40s %12s %10s %14s\n", "benchmark", "ns/op", "ratio", "iterations");
    std::vector<Result> results;
    for (const Entry& entry : registry()) {
        if (!options.filter.empty() &&
            std::string{entry.name}.find(options.filter) == std::string::npos) {
            continue;
        }
        std::uint64_t iterations = 0;
        const double ns_per_op = measure(entry.body, options.min_time_ms, options.reps, iterations);
        const double ratio = calibration_ns > 0.0 ? ns_per_op / calibration_ns : 0.0;
        results.push_back(Result{.name = entry.name,
                                 .description = entry.description,
                                 .iterations = iterations,
                                 .ns_per_op = ns_per_op,
                                 .ratio = ratio});
        std::printf("%-40s %12.4f %10.4f %14llu\n", entry.name, ns_per_op, ratio,
                    static_cast<unsigned long long>(iterations));
    }

    if (results.empty()) {
        std::fprintf(stderr, "\ncy::bench: filter '%s' matched no benchmark.\n",
                     options.filter.c_str());
        return 1;
    }

    if (!options.json_path.empty() &&
        !write_json(options, calibration_ns, calibration_iterations, results)) {
        return 1;
    }

    return 0;
}
