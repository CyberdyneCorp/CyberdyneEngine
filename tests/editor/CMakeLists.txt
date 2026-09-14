# tests/editor/ — the editor's claims that the ENGINE has to hold up too.
#
# The README beside this file recorded, at M5, that the two entries declared from
# `samples/05-editor-session/CMakeLists.txt` belong here and that "whoever next edits
# tests/CMakeLists.txt should add add_subdirectory(editor) and move the two declarations into a
# CMakeLists.txt here". This is the directory that add makes reachable. The two M5 entries are NOT
# moved with it: the change that owns them is archived, moving them is a lateral edit with its own
# risk of a silently unregistered test, and this rung's business is the suites below.
#
# unit.editor_documents — a node has a name, on the engine's side of the format. The Rust half of
# the same claim is `cargo test`'s; this is the half that fails when the engine's reader and the
# editor's writer stop agreeing, which is the failure `worldfile.h` was written to close for
# identity and which a name would otherwise repeat.

cy_add_test(
    NAME editor_documents
    KIND unit
    SOURCES test_documents.cpp
    DEPENDENCIES cy::scene-serialization
    DEFINITIONS
        "CY_SAMPLE_WORLD=\"${CMAKE_SOURCE_DIR}/samples/05b-editor-window/project/worlds/city.cyworld\"")
