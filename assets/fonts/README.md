<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Application Fonts

The generated LVGL font subset used by the Hong Kong / Singapore / Malaysia radio
application, produced by [`tools/gen_fonts.py`](../../tools/gen_fonts.py).

## Files

| File | Purpose |
| --- | --- |
| `NotoSansSC-Regular.otf` | Source font. Noto Sans SC (SIL Open Font License 1.1), 7.9 MiB, SHA-256 `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`. |
| `radio_font_cjk_16.c` | Generated 16 px, 4 bpp, uncompressed subset. The only generated face. |
| `inventory.json` | The exact code point inventory, the source hash, the converter version, and the list of scripts that are deliberately **not** covered. |

## Why one face, and why so wide

The application UI is English, so fixed UI text is rendered with LVGL's built-in
Montserrat (14 px and 20 px), whose line heights are a compact 16 px and 22 px.
The generated CJK face exists **only for dynamic text**: a Wi-Fi SSID, an ICY
stream title, or an imported station name. `radio_font_for_text()` picks it
automatically as soon as a string contains a non-ASCII byte.

Because that fallback has to render whatever the user's environment happens to
contain, it covers the **complete CJK Unified Ideographs block** plus compatibility
ideographs, kana, Greek, Cyrillic, Latin-1 and Latin Extended-A, and the standard
punctuation and symbol blocks. Measured against the source font: 21,072 of the
21,087 code points in the CJK block are really present.

An earlier revision generated a GB2312 + Big5-level-1 subset instead, and a second
20 px face for titles. Both were removed: the narrow subset produced placeholder
boxes for any uncommon character (a neighbour's SSID is not limited to common
characters), and the 20 px face stopped being selected by any code path once the
UI became English.

| Inventory | Glyphs | Bitmap payload |
| --- | ---: | ---: |
| GB2312 + Big5 level 1 (previous) | 12,953 | 1,455 KiB |
| **Full CJK block and friends (current)** | **22,439** | **~2.4 MiB** |

## Not covered

The source font does not contain these, so neither does the generated face. The
list is also recorded in `inventory.json` so the limit is explicit:

Hangul (Korean), Thai, Arabic, Hebrew, Devanagari, CJK Extension B and above.

Adding them means merging another font family, which is a separate Flash-cost
decision.

## Integration

`main/CMakeLists.txt` compiles the generated file into the `main` component via
`target_sources`; `main/radio_fonts.c` owns the writable descriptor and the
Montserrat fallback. Copying a generated file into this directory does not by
itself put it on screen.

Character heights matter for layout: the generator's `line_height` is 31 px for
this 16 px face (Noto Sans SC has wide ascent/descent). Never size a label from the
nominal pixel size — use `lv_font_get_line_height()`, which is what the UI does.

## Reproducing

```bash
python tools/gen_fonts.py            # regenerate the subset
python tools/gen_fonts.py --check    # report inventory sizes only
python tests/test_radio_font_coverage.py
python .idf/measure_font_reach.py    # how far the source font can reach (local tool)
```

Conversion parameters: `lv_font_conv 1.5.3`, `--bpp 4`, `--format lvgl`,
`--no-compress`, `--lv-include lvgl.h`. The inventory is passed to the converter
in-process through `lib/cli` because a single `--symbols` argument for ~24,000
characters far exceeds the Windows command line limit of 32,767 characters. The
exact command is constructed in `tools/gen_fonts.py`; do not reimplement it.

The generated C file is about 18 MiB of source text, so a full rebuild that touches
it takes several minutes. Incremental builds that only touch `main/*.c` are fast
because the font object is already compiled.

## Coverage acceptance

`tests/test_radio_font_coverage.py` parses the generated tables and fails if any
code point from a `main/` string literal is missing, if the CJK baseline coverage
drops below the floor, or if any spot-checked (deliberately uncommon, assigned) CJK
code point disappears. It also asserts a known-absent code point is reported
missing, so the check cannot pass vacuously. On-device rendering is a separate
check: without a physical device, non-Latin rendering stays unverified regardless
of what the host test reports.
