# visual-scripting Spec Delta

## ADDED Requirements

### Requirement: Compiled graphs are debugged at node granularity
The debugger SHALL stop a compiled gameplay graph before a node runs, on the bytecode and the native back
end alike, without interpreting the graph. Breakpoints SHALL name a graph and a node and MAY name one
entity, in which case only that entity's instance SHALL stop. While stopped, the debugger SHALL step the
instance to its next node (step into) or to its next node on an execution chain (step over), continue it
to the next breakpoint, read the value of a node's pin through the program's debug map, read the
instance's graph variables, and report the nodes run, in execution order. A program the debugger is not
attached to SHALL contain no debugging instruction, and a build without development features (Profile and
Shipping) SHALL contain no debugging code in its run loops.

#### Scenario: A breakpoint stops only the entity it names
- **WHEN** a breakpoint on a node names one entity and two entities run the graph through that node
- **THEN** only the named entity's instance SHALL stop, before the node runs, and the other SHALL run on

#### Scenario: Stepping follows execution order
- **WHEN** an instance stopped at its event is stepped into repeatedly
- **THEN** it SHALL stop at each node in the order the debugger's trace records them running, and a step across a wait SHALL stop at the next node when the instance resumes

#### Scenario: The same breakpoint on either back end
- **WHEN** one graph is run with one breakpoint on the bytecode and on the native back end
- **THEN** both SHALL stop before the same node with the same pause state and the same pin values

#### Scenario: Release builds carry no debugger
- **WHEN** the engine is built without development features
- **THEN** instrumenting a program for the debugger SHALL be refused and the run loops SHALL contain no probe test

### Requirement: A graph break pauses the whole simulation tick
When a graph stops at a breakpoint or a step, the simulation tick it stopped in SHALL pause as a whole: no
further simulation tick, physics step, gameplay system or graph SHALL run, and no event SHALL be raised,
until the debugger continues or steps it. The work the break interrupted SHALL then run on from the
stopped node in its original order before any later tick. A run that is stopped and continued SHALL
produce the same simulation, tick for tick, as the same run without the debugger.

#### Scenario: A paused tick holds its remaining work
- **WHEN** two instances resume on one tick and the first stops at a breakpoint
- **THEN** the second SHALL not run until the debugger continues, and it SHALL then run in that same tick

#### Scenario: Debugging does not change the game
- **WHEN** a run stops at breakpoints and is stepped and continued
- **THEN** every placement and every cue SHALL be the same, on the same tick, as in the run without the debugger

#### Scenario: Debugging does not change a handler's budget
- **WHEN** a handler runs under the debugger, with or without breaks
- **THEN** it SHALL finish or exhaust its instruction budget exactly where it does without the debugger: a
  probe SHALL not be charged to the budget, and a continued handler SHALL resume with the budget it had spent

### Requirement: Running graphs reload with their variables
A graph SHALL declare per-instance variables, each with a name, a type and a default, kept across events
and waits; a variable's identity SHALL be the node that declares it. Saving a graph during play SHALL
recompile it and swap the new program in for every instance at the next tick boundary, moving each
instance's variables by identity: a variable both programs declare keeps its value, a new one starts at
its default and a removed one is dropped. A variable whose type changed SHALL refuse the reload with a
diagnostic on its declaring node, and the running program SHALL be kept unchanged. A wait in progress SHALL
continue in the new program when its wait node survives.

#### Scenario: A running counter keeps its count
- **WHEN** a graph counting orders is edited and saved while play runs
- **THEN** each instance SHALL keep its count from the next tick on and count with the new program

#### Scenario: A refused reload leaves the old program running
- **WHEN** an edit changes a variable's type while play runs
- **THEN** the reload SHALL be refused on that variable's node and the instances SHALL go on running the previous program
