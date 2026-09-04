// src/core/config/ — the project graph and layered typed configuration.
//
// Section 4 of M1. Governed by `project-and-plugins`, whose first requirement is that **the project
// graph is authoritative**: a project is what its manifest declares, folder layout is conventional
// and not load-bearing, and an undeclared dependency, a cycle or an upward layer dependency is a
// build error rather than a warning.
//
// Three headers, and the seam between them is worth stating once:
//
//   project.h          what the project graph says — modules, plugins, content roots, build
//                      targets. Read-only, generated from the validated manifest at configure time.
//   settings.h         layered typed configuration. The manifest supplies values; the engine's
//                      schema supplies types; resolution reports which layer supplied what.
//   module_registry.h  the registration levels, and the deterministic order the runtime brings the
//                      project's modules up and takes them down in.
//
// The enforcement itself is not here and cannot be: cycles and undeclared dependencies have to fail
// the *build*, so they are cmake/project.cmake's and tools/project/'s. What is here is the result.

#pragma once

#include <cy/core/config/module_registry.h>
#include <cy/core/config/project.h>
#include <cy/core/config/settings.h>
