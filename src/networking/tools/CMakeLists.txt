# src/networking/tools/ — the bandwidth curve instrument. M9 task 4.4.
#
# The exit criterion is "Bandwidth stays within budget as entity count scales; interest management
# is **measured**, not assumed", and tasks.md repeats the word: "measured as a curve against entity
# count, which is the exit criterion's own word, not asserted at one population".
#
# A test asserting a budget at one population would satisfy neither. So this is an executable that
# runs the real `InterestSet` and the real `PriorityScheduler` at eight populations and prints the
# measurement as CSV; `curve.py` draws the committed figure from that CSV, so the picture cannot
# describe a run that did not happen. `tests/test_scheduler.cpp` asserts the curve's *property* —
# the budget holds at every population and the entities examined scale with the peer's cells rather
# than with the world — over the same code, at a population a test tier can afford.
cy_add_module(
    NAME cy_net_bandwidth_curve
    LAYER tools
    TYPE EXECUTABLE
    SOURCES bandwidth_curve.cpp
    PRIVATE_DEPENDENCIES cy::networking)
