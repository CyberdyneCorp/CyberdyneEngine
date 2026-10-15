# The quiet-host wrapper's session census (tools/quiet-host/), against a scripted process table: the
# race it guards against — a member reaping a child while the census reads — cannot be arranged on a
# real host on demand, and `smoke.quiet_host_nested` met it once on an idle runner.

cy_add_test(
    NAME         quiet_host_census
    KIND         unit
    SOURCES      test_census.cpp
    DEPENDENCIES cy::host-load)
