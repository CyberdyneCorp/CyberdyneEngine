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

# Issue #82: the dedicated server, a client for it, and the session measurement. All three run the
# demo game in net_demo.cpp, which tests/test_session.cpp also compiles, so what the tools print and
# what the tests assert are about the same game. The server links nothing but cy::networking — no
# renderer, audio, interface or GPU — which is the dedicated-server requirement as a link graph.
foreach(cy_net_tool IN ITEMS server client)
    cy_add_module(
        NAME cy_net_${cy_net_tool}
        LAYER tools
        TYPE EXECUTABLE
        SOURCES net_${cy_net_tool}.cpp net_demo.cpp
        PRIVATE_DEPENDENCIES cy::networking)
endforeach()

# The measurement and the trace behind the video share one session arrangement, net_sim_session.cpp.
foreach(cy_net_tool IN ITEMS session_bench session_trace)
    cy_add_module(
        NAME cy_net_${cy_net_tool}
        LAYER tools
        TYPE EXECUTABLE
        SOURCES net_${cy_net_tool}.cpp net_sim_session.cpp net_demo.cpp
        PRIVATE_DEPENDENCIES cy::networking)
endforeach()
