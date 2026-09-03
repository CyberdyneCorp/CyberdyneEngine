// The entry point every test binary links, so that no test file carries a main.
//
// It is doctest's own main: option parsing, filtering (`--test-case=`, `--test-suite=`), and the
// reporters — including `--reporters=junit --out=<file>`, which is the machine-readable result
// `testing-and-quality` requires of the harness. CTest's `--output-junit` covers the suite level;
// this covers the test-case level inside one binary.
//
// Defining the implementation here rather than in a test file is what keeps doctest's translation
// unit out of the incremental build: a test that changes recompiles itself, not the framework.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <cy/test/test.h>
