# Spec Delta

## ADDED Requirements

### Requirement: Embedded Swift source tooling
CyberEditor SHALL be able to edit an ordinary game Swift package using the same package structure and
SourceKit-LSP semantics used by external editors. The embedded tooling SHALL provide syntax-aware
presentation, diagnostics, completion, hover information, symbol search, rename, and definition and
reference navigation where the installed supported Swift toolchain provides them.

Failure or absence of SourceKit-LSP SHALL NOT prevent text editing, saving, command-line Swift builds,
or use of an external editor. The editor SHALL report which language features are unavailable and how
to install or select a supported toolchain.

#### Scenario: SourceKit enriches an ordinary package
- **WHEN** a supported SourceKit-LSP is available for the project's Swift package
- **THEN** the embedded workspace SHALL expose its diagnostics and navigation without introducing a
  second project format or generated source database

#### Scenario: Language server is unavailable
- **WHEN** SourceKit-LSP cannot be started or becomes unavailable
- **THEN** source buffers SHALL remain editable and saveable, builds SHALL remain available, and the
  missing language features SHALL be reported with a remedy

#### Scenario: External tooling remains compatible
- **WHEN** the same game package is opened in Xcode or another SourceKit-LSP client
- **THEN** it SHALL use the same sources and package manifest without an Editor-specific conversion
  step
