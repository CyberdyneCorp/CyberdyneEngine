# modules/ — optional functionality.
#
# No module is named here. cmake/modules.cmake discovered them from their manifests when the
# top-level CMakeLists.txt included it — together with any found through CY_EXTRA_MODULE_PATHS — and
# declared a CY_MODULE_<NAME> option for each. This adds the enabled ones, in registration order.
#
# A disabled module's directory is never added: its sources are not compiled and its dependencies
# are not fetched. `build-system-and-platforms` requires exclusion, not a runtime stub.

cy_add_module_subdirectories()
