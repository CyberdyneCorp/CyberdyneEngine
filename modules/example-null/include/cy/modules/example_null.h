// modules/example-null — a module that does nothing, kept as the template.
//
// A module's public header carries only what its dependents may use. Its private dependencies never
// appear here: `project-and-plugins` requires that a private dependency is not transitively
// exposed, and cmake/modules.cmake fails the configure when one is.

#pragma once

namespace cy::modules {

/// What a module says about itself. The same facts are in module.json, which is authoritative; this
/// is the runtime's view of them.
struct ModuleDescription {
    const char* name;
    const char* registration_level;
    int registration_level_index;
    const char* layer;
    int layer_index;
    bool hot_reload;
};

/// This module's description, as its manifest declares it.
ModuleDescription example_null_description();

/// Registration entry points. A real module registers its factories with the servers of its
/// registration level here, and undoes exactly that in the second — the runtime tears subsystems
/// down in the exact reverse of the order it built them up.
void example_null_register();
void example_null_unregister();

/// How many times register() has run without a matching unregister(). Zero before startup and after
/// shutdown; the ordering test has something to assert on.
int example_null_registration_depth();

}  // namespace cy::modules
