# `deps/fonts/` — provenance

The fonts the engine carries: CyberUI's interface face and the faces `text-and-fonts`' tests shape
and rasterise (issue #86). Every file here is produced by `tools/content/make_fonts.py` and nothing
here is edited by hand.

## Noto (SIL Open Font License 1.1)

Cut from the Noto families as [google/fonts](https://github.com/google/fonts) publishes them, at
commit `5e8a3ba899557829a76cfdac30fa512bda91d7ca`, under the SIL Open Font License 1.1 —
[`OFL.txt`](OFL.txt), the licence file google/fonts ships beside the Latin family. The Noto
families declare **no Reserved Font Name**, so a modified version may keep the name.

The modification is subsetting and instancing (fontTools 4.55): each file keeps the codepoint ranges
below and every OpenType layout feature, has its hinting instructions removed, and is pinned to
`wdth` 100; the Latin face keeps its `wght` axis and the others are pinned to the Regular weight.

| File | Upstream file (sha256) | Ranges | Used by |
|---|---|---|---|
| `NotoSans-Latin-VF.ttf` | `ofl/notosans/NotoSans[wdth,wght].ttf` (`bfb7bb69…6b3d`) | U+0020–007E, U+00A0–00FF, U+2013–2014, U+2018–201D, U+2026, U+25CC | CyberUI's interface font (compiled in with `CY_TEXT`); the variable-axis, distance-field, Latin shaping and importer tests |
| `NotoSansArabic-Subset.ttf` | `ofl/notosansarabic/NotoSansArabic[wdth,wght].ttf` (`63111b5b…869e`) | U+0020, U+0600–06FF, U+200C–200F, U+25CC | Arabic shaping goldens and the joining comparison |
| `NotoSansDevanagari-Subset.ttf` | `ofl/notosansdevanagari/NotoSansDevanagari[wdth,wght].ttf` (`14ec4af4…853e`) | U+0020, U+0900–097F, U+200C–200D, U+25CC | Devanagari shaping golden |
| `NotoSansThai-Subset.ttf` | `ofl/notosansthai/NotoSansThai[wdth,wght].ttf` (`5a1c559b…c9ef8`) | U+0020, U+0E00–0E7F, U+25CC | Thai shaping golden |
| `NotoSansHebrew-Subset.ttf` | `ofl/notosanshebrew/NotoSansHebrew[wdth,wght].ttf` (`7ef36a2c…de88`) | U+0020, U+0590–05FF, U+25CC | bidirectional layout, synthetic styles |
| `NotoSansHebrew-Subset.woff` | the Hebrew subset above, re-packaged as WOFF 1 | as above | WOFF loading |
| `NotoSansThaiHebrew.ttc` | the Thai and Hebrew subsets above, as a TrueType collection | as above | collection loading |

The full upstream digests are in `tools/content/make_fonts.py`, which refuses to cut from a file
that does not match them.

## The project's own

| File | What it is |
|---|---|
| `CyberColourTest.ttf` | One COLRv0 glyph (U+25A0): a red square with a blue square inside it, and a CPAL palette. Built from nothing by `colour_font()` in `tools/content/make_fonts.py`; licensed as the rest of this repository is. |

## Digests of what the script writes

```
8b0ac5fdd04f65907814031f825a6cabdb96bcdf8cac199c82bef923331d3858  NotoSans-Latin-VF.ttf
6a05b1c1db63b3e3e60633ba827fb3ea27fcc53c4a8ac93188e1f63494b61911  NotoSansArabic-Subset.ttf
96da76106c0015e16b72dda9139241fd36e1ddcf4f7539e97097df7912ea37a1  NotoSansDevanagari-Subset.ttf
c3629a960930bb1eba4d30b5a855bee52737582600645bfa88e3f856e5f44d35  NotoSansThai-Subset.ttf
65d039eae9d535524fe34be7e5e257c510cc3ffd5685d2f09affbe556cd4ac73  NotoSansHebrew-Subset.ttf
e41f36789e9f918cf5d699092ae4a279c47a49072bbda94c4e7f8f7db320b106  NotoSansHebrew-Subset.woff
5be162e54e040885a9f73b1a7de615d969dcb3a2860bd487f0a61bd61b984684  NotoSansThaiHebrew.ttc
69219a765ad935daad7903d904ddce402a113c05c3024042ecf9b911405cc202  CyberColourTest.ttf
```
