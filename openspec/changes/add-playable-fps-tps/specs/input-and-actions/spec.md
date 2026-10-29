## ADDED Requirements

### Requirement: An override addresses its own action's bindings
A rebinding override's slot SHALL count the bindings of the action it names, in that scheme and in
table order, so that the first binding of any action is slot zero whatever other actions are bound
before it.

#### Scenario: A game's jump moved to F
- **WHEN** a context binds movement then jump, and a profile overrides jump's slot zero with F
- **THEN** F SHALL trigger jump, the key it replaced SHALL not, and movement SHALL keep its keys
