"""The editor's Rust SDK generator. Task 2.3, `editor-rust-application` "Editor SDK boundary".

`editor-rust-application`: "The SDK SHALL be generated or maintained against the ABI description, so
that the two cannot drift, consistent with how the Swift overlay is produced."

So this package is deliberately the same shape as `tools/gen/swift/overlay/`: one reader of the
header (`tools/abi/abi_describe.py`), one hand-written table of the things a C declaration does not
carry, and a `--check` that is the same code path as the write.
"""
