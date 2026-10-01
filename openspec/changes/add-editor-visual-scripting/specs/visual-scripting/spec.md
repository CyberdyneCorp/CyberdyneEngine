# visual-scripting Spec Delta

## ADDED Requirements

### Requirement: Event graphs compile to one program with a handler table
A gameplay graph's entry points SHALL be event nodes, each naming the event it answers, and the graph
SHALL compile to one shared program with a table of where each event's handler begins. Raising an event
on an instance SHALL start that handler on either back end; an event raised while a handler waits SHALL
replace the waiting handler. A graph that answers no event, or two event nodes answering one event, SHALL
be compile errors naming the nodes.

#### Scenario: Two events, one program
- **WHEN** a graph answers two events
- **THEN** it SHALL compile to one program, and raising each event SHALL run only its own handler

#### Scenario: A newer order replaces a waiting one
- **WHEN** an event is raised on an instance whose handler is waiting
- **THEN** the handler SHALL start again from the event, and the earlier wait SHALL not resume

### Requirement: The host declares what a graph may name
A graph's host SHALL declare every function, query, event, command, field and wait a graph may name,
with its kind, arity and capability. The compiler SHALL refuse a name the host did not declare, a name
used as another kind, and a name whose capability the graph was not granted, each with a diagnostic on
the node that names it. The host SHALL resolve every declared name once, when it loads a program, and
SHALL NOT look a name up while a program runs.

#### Scenario: A misspelled function is a compile error on its node
- **WHEN** a graph calls a function its host did not declare
- **THEN** compilation SHALL fail with a diagnostic naming that node and that name
