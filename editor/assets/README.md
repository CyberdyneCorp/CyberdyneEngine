# `editor/assets/` — what the editor ships inside its binary

Small, derived, and compiled in with `include_bytes!` rather than read at start-up. A file read at
start-up is a failure mode a compiled-in asset does not have, and the window icon in particular has
to exist before there is a window to put it on.

## `identity/`

| File | What it is | Where it is drawn |
|---|---|---|
| `cyberengine-horizontal.png` | The horizontal lockup — mark, `CYBERENGINE`, `BY CYBERDYNE` — on transparency | The application header, dark theme |
| `cyberengine-monochrome.png` | The monochrome lockup as a **white mask**, tinted at draw time | The application header, light theme |
| `cyberengine-mark-256.png` | The mark alone, square, 256 px | The window and taskbar icon |
| `cyberengine-mark-64.png` | The same at 64 px | Reserved for a small icon where one is asked for |
| `derive.py` | The script that produced all four from the normative sheet | Run from the repository root |

**The source is `docs/design/images/cyberengine-logo.png`, which is normative.** Nothing here is
redrawn: `derive.py` crops the sheet and keys its backdrop out by luminance headroom, so the mark's
metallic gradient and its blue emissive core arrive intact. That matters, because
`editor-visual-language` puts those *in the mark and nowhere else* — the chrome around it stays
charcoal and flat, and `cy-editor-shell`'s `identity.rs` carries the test that says so.

Regenerate with:

```
python3 editor/assets/identity/derive.py
```

It is committed alongside its output because "where did this PNG come from" is a question every
asset in a repository eventually gets asked, and a derived file whose derivation is not committed is
one nobody can safely change.

**The monochrome lockup is a mask on purpose.** One shape, pure white, with its alpha; the interface
tints it with the theme's primary text colour. A colour baked in would be white — invisible on the
light theme — so shipping it as a mask is what lets one file serve both.
