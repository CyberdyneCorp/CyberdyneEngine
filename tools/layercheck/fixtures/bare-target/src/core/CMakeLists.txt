# Fixture (d): a target declared without cy_add_module(), which is a target with no layer and so no
# layer check. The lint MUST fail on it.
add_library(fx_core STATIC allocator.cpp)
