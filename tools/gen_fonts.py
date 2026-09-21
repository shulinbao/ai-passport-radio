#!/usr/bin/env python3
"""Generate the application's Chinese font subsets for LVGL.

Why this exists
---------------
The LVGL baseline in this repository enables Montserrat 14/20 only, and those
fonts contain no CJK glyphs. Shipping Chinese UI text therefore requires
generating a real subset and selecting it on the widgets, which is a build step
with its own inputs. This script keeps that step reproducible: the character
inventory is derived from the application sources plus a defined CJK baseline,
and both the inventory and the converter command live under version control.

Inventory policy
----------------
* ``radio_font_cjk_16`` (body text, list rows, keyboard) covers printable ASCII,
  common Unicode punctuation, the full GB2312 repertoire (Simplified Chinese),
  Big5 level-1 (common Traditional Chinese, needed for Hong Kong station names),
  and every code point that appears anywhere in ``main/``.
* ``radio_font_cjk_20`` (titles and station names) covers only printable ASCII,
  punctuation, and the code points that appear inside C **string literals** in
  ``main/``. It is deliberately small.

Dynamic text (a Wi-Fi SSID, an ICY stream title) can contain code points outside
these sets. The firmware therefore exposes ``radio_font_covers_utf8()`` so the
UI can detect that case instead of silently rendering placeholder boxes.

Windows note
------------
The inventory is passed to the converter in-process through ``lib/cli`` because a
single ``--symbols`` argument for ~11k characters exceeds the Windows command
line limit (32767 characters).

Usage
-----
    python tools/gen_fonts.py [--font PATH] [--check]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_DIR = ROOT / "main"
OUT_DIR = ROOT / "assets" / "fonts"
DEFAULT_FONT = OUT_DIR / "NotoSansSC-Regular.otf"
NODE_TOOLS = Path(
    os.environ.get(
        "RADIO_NODE_TOOLS",
        str(ROOT / ".idf" / ".idf" / "nodetools" / "node_modules"),
    )
)

# Ranges always covered. The application UI is English, so this font exists for
# *dynamic* text only: a Wi-Fi SSID, an ICY stream title, an imported station name.
# Those can be in any script, so cover the whole CJK ideograph block rather than a
# "common characters" subset -- a neighbour's SSID is not limited to GB2312.
#
# Measured against the source font (Noto Sans SC, SIL OFL 1.1): the full block is
# really present (21,072 of 21,087 requested glyphs), costing ~2.4 MiB of bitmap
# payload versus ~1.4 MiB for a GB2312+Big5 subset. The 8 MB app partition has room.
BASE_RANGES = (
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x00FF),   # Latin-1 supplement
    (0x0100, 0x017F),   # Latin Extended-A
    (0x0370, 0x03FF),   # Greek
    (0x0400, 0x04FF),   # Cyrillic
    (0x2000, 0x206F),   # general punctuation
    (0x2190, 0x21FF),   # arrows
    (0x2460, 0x24FF),   # enclosed alphanumerics
    (0x25A0, 0x25FF),   # geometric shapes
    (0x2600, 0x27BF),   # miscellaneous symbols
    (0x3000, 0x303F),   # CJK punctuation
    (0x3040, 0x30FF),   # hiragana + katakana
    (0x4E00, 0x9FFF),   # CJK unified ideographs (complete block)
    (0xF900, 0xFAFF),   # CJK compatibility ideographs
    (0xFF00, 0xFFEF),   # halfwidth and fullwidth forms
)

# Deliberately NOT covered (documented so the limit is explicit, not a surprise):
# Hangul (Korean), Thai, Arabic, Hebrew, Devanagari and CJK Extension B and above.
# The source font does not contain them; adding another family would be a separate
# decision with its own Flash cost.


# ---------------------------------------------------------------------------
# Source scanning
# ---------------------------------------------------------------------------
def strip_comments(text: str) -> tuple[str, list[str]]:
    """Return (code_without_comments, literals).

    Comments and string literals are separated so the 20 px font can be limited
    to text that can actually reach the screen.
    """
    out: list[str] = []
    literals: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if ch == '"':
            i += 1
            buf: list[str] = []
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    buf.append(text[i])
                    buf.append(text[i + 1])
                    i += 2
                    continue
                buf.append(text[i])
                i += 1
            i += 1
            literals.append("".join(buf))
            out.append(" ")
            continue
        out.append(ch)
        i += 1
    return "".join(out), literals


_ESCAPE_RE = re.compile(
    r"\\x([0-9a-fA-F]{1,2})|\\u([0-9a-fA-F]{4})|\\U([0-9a-fA-F]{8})|\\([0-7]{1,3})|\\(.)"
)


def decode_c_string(raw: str) -> str:
    """Decode the escapes a C string literal can contain into real text."""
    out: list[str] = []

    def repl(match: re.Match[str]) -> str:
        if match.group(1) is not None:
            # \xNN is a raw byte; collect the byte and decode as latin-1 so a
            # multi-byte UTF-8 sequence reassembles correctly afterwards.
            return chr(int(match.group(1), 16))
        if match.group(2) is not None:
            return chr(int(match.group(2), 16))
        if match.group(3) is not None:
            return chr(int(match.group(3), 16))
        if match.group(4) is not None:
            return chr(int(match.group(4), 8))
        simple = {
            "n": "\n", "t": "\t", "r": "\r", "0": "\0",
            "\\": "\\", '"': '"', "'": "'",
        }
        return simple.get(match.group(5), match.group(5))

    out.append(_ESCAPE_RE.sub(repl, raw))
    text = "".join(out)
    # Re-assemble UTF-8 that was written byte-by-byte with \xNN escapes.
    try:
        return text.encode("latin-1").decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError):
        return text


def scan_sources() -> tuple[set[int], set[int]]:
    """Return (literal_codepoints, all_codepoints_including_comments)."""
    literal_cps: set[int] = set()
    all_cps: set[int] = set()
    for path in sorted(MAIN_DIR.iterdir()):
        if path.suffix not in (".c", ".h"):
            continue
        text = path.read_text(encoding="utf-8")
        all_cps.update(ord(ch) for ch in text)
        _, literals = strip_comments(text)
        for raw in literals:
            for ch in decode_c_string(raw):
                literal_cps.add(ord(ch))
    return literal_cps, all_cps


# ---------------------------------------------------------------------------
# CJK baselines
# ---------------------------------------------------------------------------
def gb2312_codepoints() -> set[int]:
    """Every code point reachable through the GB2312 charset (Simplified)."""
    points: set[int] = set()
    for hi in range(0xA1, 0xFA):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes((hi, lo)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                points.add(ord(ch))
    return points


def big5_level1_codepoints() -> set[int]:
    """Big5 level-1 (frequently used Traditional characters).

    Big5 level 1 occupies the high-byte range 0xA4-0xC6. Including it matters
    because Hong Kong station names are written in Traditional Chinese, which
    GB2312 alone cannot cover.
    """
    points: set[int] = set()
    for hi in range(0xA4, 0xC7):
        for lo in range(0x40, 0x7F):
            for second in (lo, lo + 0x3F):
                try:
                    ch = bytes((hi, second)).decode("big5")
                except (UnicodeDecodeError, ValueError):
                    continue
                if len(ch) == 1:
                    points.add(ord(ch))
    return points


def base_codepoints() -> set[int]:
    points: set[int] = set()
    for start, end in BASE_RANGES:
        points.update(range(start, end + 1))
    return points


def is_displayable(cp: int) -> bool:
    """Keep only code points that can be drawn as a glyph.

    Control and format characters (Cc/Cf), surrogates, private-use and unassigned
    code points have no glyph in any font; requesting them only produces noise in
    the converter log and in the coverage report.
    """
    if cp < 0x20 or cp == 0x7F:
        return False
    if cp == 0x20:      # space is legitimate even though it is a separator
        return True
    return unicodedata.category(chr(cp))[0] in ("L", "N", "P", "S", "M")


def filter_displayable(points: set[int]) -> set[int]:
    return {cp for cp in points if is_displayable(cp)}


# ---------------------------------------------------------------------------
# Converter driver
# ---------------------------------------------------------------------------
def run_converter(font: Path, symbols: str, size: int, name: str, output: Path) -> None:
    lib = NODE_TOOLS / "lv_font_conv" / "lib" / "cli.js"
    if not lib.is_file():
        raise SystemExit(
            f"lv_font_conv not found at {lib}. Install it with:\n"
            f"  node <npm-cli.js> install --prefix .idf/nodetools lv_font_conv"
        )
    argv = [
        "--font", str(font),
        "--symbols", symbols,
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--lv-include", "lvgl.h",
        "--lv-font-name", name,
        "--output", str(output),
    ]
    driver = OUT_DIR / f".driver-{name}.js"
    driver.write_text(
        "const cli = require(" + json.dumps(str(lib).replace("\\", "/")) + ");\n"
        "cli.run(" + json.dumps(argv) + ").then(\n"
        "  () => process.exit(0),\n"
        "  (err) => { console.error(err && err.message ? err.message : err);"
        " process.exit(1); }\n"
        ");\n",
        encoding="utf-8",
    )
    try:
        result = subprocess.run(["node", str(driver)], cwd=str(ROOT))
    finally:
        driver.unlink(missing_ok=True)
    if result.returncode != 0:
        raise SystemExit(f"lv_font_conv failed for {name}")
    if not output.is_file() or output.stat().st_size == 0:
        raise SystemExit(f"lv_font_conv produced no output for {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", default=os.environ.get("RADIO_FONT_SRC", str(DEFAULT_FONT)))
    parser.add_argument("--check", action="store_true",
                        help="only report the inventory, do not generate")
    args = parser.parse_args()

    font = Path(args.font)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    literal_cps, all_cps = scan_sources()
    base = base_codepoints()

    # A single subset. An earlier revision also generated a 20 px face for titles,
    # but once the UI became English every title is ASCII and is rendered with
    # Montserrat, so that face was never selected -- it was ~190 KiB of dead Flash.
    inv16 = filter_displayable(base | gb2312_codepoints() | big5_level1_codepoints() | all_cps)

    print(f"string-literal code points : {len(literal_cps)}")
    print(f"all main/ code points      : {len(all_cps)}")
    print(f"radio_font_cjk_16 inventory: {len(inv16)}")

    if args.check:
        return 0

    if not font.is_file():
        raise SystemExit(
            f"source font not found: {font}\n"
            "Download Noto Sans SC (SIL OFL 1.1) into assets/fonts/ or pass "
            "--font, or set RADIO_FONT_SRC."
        )
    digest = hashlib.sha256(font.read_bytes()).hexdigest()
    print(f"source font sha256         : {digest}")

    run_converter(font, "".join(chr(cp) for cp in sorted(inv16)), 16,
                  "radio_font_cjk_16", OUT_DIR / "radio_font_cjk_16.c")

    manifest = {
        "source_font": font.name,
        "source_sha256": digest,
        "converter": "lv_font_conv 1.5.3",
        "bpp": 4,
        "compressed": False,
        "fonts": {
            "radio_font_cjk_16": {
                "size": 16,
                "output": "radio_font_cjk_16.c",
                "code_points": sorted(inv16),
            },
        },
        "not_covered": [
            "Hangul", "Thai", "Arabic", "Hebrew", "Devanagari",
            "CJK Extension B and above",
        ],
    }
    (OUT_DIR / "inventory.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=1), encoding="utf-8")

    for name in ("radio_font_cjk_16.c",):
        path = OUT_DIR / name
        print(f"{name}: {path.stat().st_size / 1024:.1f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
