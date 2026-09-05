## MODIFIED Requirements

### Requirement: Commands are the single action surface
Every user-invokable action SHALL be registered in a **command registry** with an identifier, a label,
a category, an availability predicate, and an optional default binding.

Menus, toolbars, context menus, keyboard shortcuts, the command palette, automation, tests, and the
**agent interface** defined in `editor-agent-interface` SHALL all invoke commands. An action reachable only through a specific widget SHALL
be a defect.

Command availability SHALL be queryable, so that a disabled action can explain why it is unavailable.

Commands that modify project state SHALL execute through the transaction system defined in
`editor-documents-and-transactions`.

Command metadata SHALL be rich enough for **machine invocation**, not merely for rendering a menu
item: typed parameters with their meaning, a description written for a caller that cannot see the
interface, and a declared **effect class** — read, reversible mutation, irreversible mutation, or
external effect.

A registry that only has to satisfy a menu will not satisfy a caller that has never seen the menu,
and retrofitting metadata across an established registry is an entry-by-entry migration. This
therefore applies from the first command registered.

#### Scenario: A command is invocable without seeing the interface
- **WHEN** a caller has only the registry
- **THEN** it SHALL be able to determine what the command does, what its parameters mean, and what
  class of effect it has

#### Scenario: One action, six entry points
- **WHEN** an action exists
- **THEN** it SHALL be invocable from a menu, a shortcut, the palette, a script, and a test without
  additional implementation

#### Scenario: Unavailability is explainable
- **WHEN** a command is disabled
- **THEN** the reason SHALL be reportable rather than the control being merely greyed
