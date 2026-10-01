# check-embedded-shader-headers

Check every committed embedded-shader header against its Slang sources, regenerate the stale
ones, and let clang build the test harness without weakening the warning set.
