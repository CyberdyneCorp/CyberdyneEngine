## ADDED Requirements

### Requirement: Committed compiled shaders stay current
A compiled shader committed to the repository, such as an embedded SPIR-V or MSL header for a pass
that must exist in a build without a shader compiler, SHALL be declared with the invocations that
reproduce it from its sources. Continuous integration SHALL recompile every declared header with
the development build's compiler and fail when the result differs from the committed bytes. It SHALL
also fail when a header of that kind is in the tree but not declared. A header that is deliberately
not regenerated SHALL be excluded explicitly, with the reason recorded.

#### Scenario: A shader source changes and its header does not
- **WHEN** a change edits a Slang source or a module it imports and leaves a committed header
  compiled from it unchanged
- **THEN** the check SHALL fail, naming the header and the group that produces it
- **AND** the committed tree SHALL NOT be modified by the check

#### Scenario: A new embedded header is not declared
- **WHEN** a change adds a header matching the embedded-shader name patterns without declaring or
  excluding it
- **THEN** the check SHALL fail, naming the header
