#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pre-generate an LVGL **binary** (`.bin`) font from a TTF/OTF/TTC source.

The output is meant to be copied to the SD card (e.g. `<sd>/fonts/xxx.bin`)
and loaded at runtime with LVGL's `lv_font_load("S:/fonts/xxx.bin")` — so the
firmware does not bake in the glyphs and the font can be swapped on the card
without re-flashing.

Why a `.bin` instead of the usual `.c`:
    lv_font_conv supports `--format bin`, whose output `lv_font_load()` can
    decode at runtime (head / cmap / loca / glyf tables). A `.c` file must be
    compiled into the firmware; a `.bin` is read from the SD card and
    allocated in PSRAM, keeping the flash image small while providing a
    much larger character set than the built-in embedded font.

Default behaviour — FULL charset:
    "根据输入字体文件的所支持的字符集，生成全量字体bin"
    All the code points the source font's cmap advertises are baked in.

How the charset is passed to lv_font_conv (important):
    lv_font_conv only accepts `-r <range>` / `--symbols <chars>` on the command
    line, and Windows' CreateProcess caps the WHOLE command line at 32767
    **bytes**. A big CJK font can advertise tens of thousands of code points,
    so the encoding matters a lot:

      * a contiguous run is cheapest as a range token  (`-r 0x4E00-0x4E20`)
      * an isolated code point is cheapest as a literal (`--symbols=字`),
        because `0x4E00` costs 6+ chars while the character costs 3 bytes

    This script therefore picks, **per run**, whichever is cheaper, and if the
    command line still would not fit it progressively merges runs across
    growing gaps (asking for code points the font lacks is harmless —
    lv_font_conv simply skips them). That guarantees the invocation fits.

Control characters (U+0000..U+001F) and surrogates are dropped: they cannot be
rendered, and U+0000 cannot even appear in argv.

Usage
-----
    # create a virtualenv and install fontTools first (one-time):
    python -m venv .venv
    .venv\\Scripts\\python -m pip install fonttools

    # full charset, size 12, bpp 4 (defaults)
    .venv\\Scripts\\python tools/gen_lvgl_font_bin.py

    # a different source font
    .venv\\Scripts\\python tools/gen_lvgl_font_bin.py --input "assets/腾祥嘉丽大圆简.TTF"

    # only the characters used by a set of files (much smaller .bin)
    .venv\\Scripts\\python tools/gen_lvgl_font_bin.py \\
        --chars-from songlist.txt --chars-from lib/ui/src/ui_events.cpp

    # dry run: report sizes/plan, write nothing
    .venv\\Scripts\\python tools/gen_lvgl_font_bin.py --check
"""

import argparse
import os
import re
import struct
import subprocess
import sys

try:
    from fontTools.ttLib import TTFont, TTCollection
except ImportError:
    TTFont = None
    TTCollection = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = os.path.join(ROOT, "assets", "DreamHanSansSC-W17.ttf")

# npm-installed lv_font_conv (same mechanism as make_ui_font.py).
LV_FONT_CONV = os.path.join(
    os.environ.get("APPDATA", ""), "npm", "node_modules", "lv_font_conv", "lv_font_conv.js")

NODE = "node"

# Windows CreateProcess limits the whole command line to 32767 bytes. Target
# a bit under that so the fixed arguments and quoting never push us over.
ARGV_BUDGET_BYTES = 29000

# Per-argument sizes (keep each one modest: easier to debug, and avoids any
# per-argument cap in node/argparse).
RANGE_BATCH_BYTES = 1800
SYMBOLS_CHUNK_BYTES = 6000

# If the command line does not fit, merge runs separated by at most `gap`
# missing code points, trying these gaps in order until it fits. The last
# entry turns the whole charset into one single range.
GAP_LADDER = [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048,
              4096, 8192, 16384, 32768, 65536, 1 << 20]


def clen(s):
    """Length in bytes (what the OS limit counts), not characters."""
    return len(s.encode("utf-8", "surrogatepass"))


def sanitize(cps):
    """Drop code points that must not go into argv / cannot be displayed."""
    return {c for c in cps if c >= 0x20 and not (0xD800 <= c <= 0xDFFF)}


def load_cmap(path):
    """Return a set of all code points the font can draw (best cmap)."""
    if path.lower().endswith(".ttc"):
        fonts = TTCollection(path, lazy=True).fonts
    else:
        fonts = [TTFont(path, lazy=True)]
    cps = set()
    for f in fonts:
        try:
            cps.update(f.getBestCmap().keys())
        except Exception as e:
            sys.stderr.write("cannot read cmap of %s: %s\n" % (path, e))
    return cps


def coalesce(cps):
    """Turn a set of code points into a sorted list of (start, end) ranges."""
    cps = sorted(cps)
    if not cps:
        return []
    out = []
    s = p = cps[0]
    for cp in cps[1:]:
        if cp == p + 1:
            p = cp
        else:
            out.append((s, p))
            s = p = cp
    out.append((s, p))
    return out


def merge_ranges(runs, max_gap):
    """Merge runs separated by at most `max_gap` missing code points."""
    if not runs:
        return []
    out = [list(runs[0])]
    for (s, e) in runs[1:]:
        if s - out[-1][1] - 1 <= max_gap:
            if e > out[-1][1]:
                out[-1][1] = e
        else:
            out.append([s, e])
    return [(a, b) for a, b in out]


def chars_from_files(paths):
    out = set()
    for p in paths:
        with open(p, "r", encoding="utf-8", errors="replace") as fp:
            for ch in fp.read():
                if ord(ch) >= 32:
                    out.add(ord(ch))
    return out


def fmt_range(a, b):
    return "0x%X-0x%X" % (a, b) if a != b else "0x%X" % a


def split_runs(runs):
    """Per run, choose the cheaper encoding: a range token or literal chars.

    Returns (range_runs, symbol_chars).
    """
    range_runs = []
    symbols = []
    for (a, b) in runs:
        range_cost = clen(fmt_range(a, b)) + 1      # +1 for the joining comma
        symbol_cost = sum(clen(chr(c)) for c in range(a, b + 1))
        if range_cost <= symbol_cost:
            range_runs.append((a, b))
        else:
            symbols.extend(chr(c) for c in range(a, b + 1))
    return range_runs, symbols


def batch_tokens(tokens, max_bytes, sep=","):
    """Join tokens with `sep`, splitting so each batch stays under max_bytes."""
    batches = []
    cur = []
    cur_len = 0
    for t in tokens:
        tl = clen(t)
        new_len = (cur_len + clen(sep) + tl) if cur else tl
        if cur and new_len > max_bytes:
            batches.append(sep.join(cur))
            cur = [t]
            cur_len = tl
        else:
            cur.append(t)
            cur_len = new_len
    if cur:
        batches.append(sep.join(cur))
    return batches


def append_charset(argv, runs):
    """Append the charset to argv; return (range_run_count, symbol_count)."""
    range_runs, symbols = split_runs(runs)
    for b in batch_tokens([fmt_range(a, b2) for (a, b2) in range_runs], RANGE_BATCH_BYTES):
        argv += ["-r", b]
    for s in batch_tokens(symbols, SYMBOLS_CHUNK_BYTES, sep=""):
        if s:
            # Use the `--symbols=<value>` form: a value starting with '-' would
            # otherwise be mistaken for an option by argparse.
            argv += ["--symbols=" + s]
    return len(range_runs), len(symbols)


def base_argv(args):
    return [
        "--size", str(args.size),
        "--bpp", str(args.bpp),
        "--format", "bin",
        "--font", args.input,
        "--no-compress",
        "--no-prefilter",
        "--no-kerning",
        "-o", args.out,
    ]


def command_line_bytes(argv, conv):
    """Approximate byte length of the full `node <conv> <argv...>` command."""
    return clen(NODE) + 1 + clen(conv) + 1 + sum(clen(a) + 1 for a in argv)


def verify_bin(path):
    """Check the produced .bin against LVGL 8.3's structural limits.

    Two limits silently corrupt a runtime-loaded font, so they are worth
    checking instead of assuming:

      * `lv_font_fmt_txt_dsc_t.cmap_num` is a **9-bit** field -> at most 511
        cmap subtables. More than that and the surplus characters become
        unreachable (they draw as placeholders).
      * the loader sizes a FORMAT0_FULL id list in a **uint8_t** -> a subtable
        with more than 255 entries overflows and is read truncated.

    Returns (ok, report_lines).
    """
    data = open(path, "rb").read()
    if len(data) < 16 or data[4:8] != b"head":
        return False, ["not an LVGL bin font (no 'head' table at offset 4)"]

    def u32(o):
        return struct.unpack_from("<I", data, o)[0]

    def u16(o):
        return struct.unpack_from("<H", data, o)[0]

    head_len = u32(0)
    tables = u16(12)
    font_size = u16(14)
    cmaps_start = head_len
    cmaps_len = u32(cmaps_start)
    cmap_count = u32(cmaps_start + 8)
    loca_start = cmaps_start + cmaps_len
    glyph_count = u32(loca_start + 8)

    lines = ["%d bytes, tables=%d, size=%d, glyphs=%d, cmaps=%d"
             % (len(data), tables, font_size, glyph_count, cmap_count)]
    ok = True

    if cmap_count > 511:
        ok = False
        lines.append("[FAIL] cmaps=%d exceeds LVGL's 9-bit cmap_num limit (511);"
                     " most characters would be unreachable." % cmap_count)
        lines.append("       fix: restrict the charset with --chars-from, or use"
                     " a smaller/fewer-block source font.")

    table_off = cmaps_start + 12
    max_fmt0 = 0
    risky = 0
    for i in range(min(cmap_count, 100000)):
        o = table_off + i * 16
        if o + 16 > len(data):
            break
        entries = u16(o + 12)
        fmt = data[o + 14]
        if fmt == 0:  # FORMAT0_FULL
            max_fmt0 = max(max_fmt0, entries)
            if entries > 255:
                risky += 1
    if risky:
        ok = False
        lines.append("[FAIL] %d FORMAT0_FULL subtable(s) have >255 entries"
                     " (LVGL loader uint8_t overflow, max=%d)" % (risky, max_fmt0))

    if ok:
        lines.append("[OK] structures are within LVGL 8.3 limits"
                     " (cmaps<=511, FORMAT0_FULL<=255)")
    return ok, lines


def resolve_charset(args):
    """Return the requested set of code points (before coalescing)."""
    restrict = set()
    for f in args.chars_from:
        restrict |= chars_from_files([f])

    if args.ranges:
        cps = set()
        for s in args.ranges:
            for tok in s.split(","):
                m = re.match(r"^\s*(0x[0-9a-fA-F]+|\d+)(?:\s*-\s*(0x[0-9a-fA-F]+|\d+))?\s*$", tok)
                if not m:
                    raise SystemExit("bad range token: %s" % tok)
                a = int(m.group(1), 0)
                b = int(m.group(2), 0) if m.group(2) else a
                cps.update(range(a, b + 1))
        if restrict:
            cps &= restrict
        return cps

    if TTFont is None:
        raise SystemExit("fontTools not installed; run: .venv\\Scripts\\python -m pip install fonttools")
    cps = load_cmap(args.input)
    if restrict:
        cps &= restrict
    return cps


def main():
    ap = argparse.ArgumentParser(
        description="Generate an LVGL BINARY font (.bin) for runtime lv_font_load from SD.")
    ap.add_argument("--input", default=DEFAULT_SOURCE,
                    help="source font (.ttf/.otf/.ttc); default: %(default)s")
    ap.add_argument("-o", "--output", default=None,
                    help="output .bin; default: assets/<basename>-<size>.bin")
    ap.add_argument("--size", type=int, default=12, help="font size in px (default 12)")
    ap.add_argument("--bpp", type=int, choices=(1, 2, 3, 4, 8), default=4,
                    help="bits per pixel (default 4)")
    ap.add_argument("--chars-from", action="append", default=[],
                    help="read characters from this file and only bake those (repeatable)")
    ap.add_argument("--ranges", action="append", default=[],
                    help="explicit code-point ranges, e.g. '0x20-0x7F 0x4E00-0x9FFF' (overrides full)")
    ap.add_argument("--check", action="store_true",
                    help="report only; do not run lv_font_conv")
    ap.add_argument("--lv-font-conv", default=None,
                    help="path to lv_font_conv.js (default: npm global)")
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        raise SystemExit("source font not found: %s" % args.input)

    base = os.path.splitext(os.path.basename(args.input))[0]
    out = args.output or os.path.join(ROOT, "assets", "%s-%d.bin" % (base, args.size))
    args.out = out
    os.makedirs(os.path.dirname(out), exist_ok=True)

    conv = args.lv_font_conv or LV_FONT_CONV
    if not os.path.isfile(conv):
        raise SystemExit("lv_font_conv not found at %s\ninstall with: npm i -g lv_font_conv" % conv)

    raw = resolve_charset(args)
    cps = sanitize(raw)
    dropped = len(raw) - len(cps)
    runs = coalesce(cps)

    print("source     : %s" % os.path.relpath(args.input, ROOT))
    print("chars      : %d code points in %d runs%s"
          % (len(cps), len(runs), ("  (dropped %d control/surrogate)" % dropped) if dropped else ""))
    if not cps:
        raise SystemExit("empty character set — nothing to generate")

    # ---- pick the most compact encoding that still fits the OS argv limit ----
    chosen = None
    chosen_gap = 0
    n_range = n_sym = 0
    est = 0
    for gap in GAP_LADDER:
        merged = merge_ranges(runs, gap) if gap else runs
        argv = base_argv(args)
        n_range, n_sym = append_charset(argv, merged)
        est = command_line_bytes(argv, conv)
        if est <= ARGV_BUDGET_BYTES:
            chosen = argv
            chosen_gap = gap
            break
    if chosen is None:  # last resort: one single span
        argv = base_argv(args)
        n_range, n_sym = append_charset(argv, [(min(cps), max(cps))])
        chosen = argv
        chosen_gap = GAP_LADDER[-1]
        est = command_line_bytes(argv, conv)

    print("encoding   : %d range token(s) + %d literal symbol(s), merged gaps<=%d"
          % (n_range, n_sym, chosen_gap))
    print("argv       : %d bytes (limit 32767, budget %d)%s"
          % (est, ARGV_BUDGET_BYTES, "  [OK]" if est <= ARGV_BUDGET_BYTES else "  [OVER!]"))
    if chosen_gap:
        print("             (runs were merged to fit the command line; lv_font_conv"
              " skips code points the font lacks, so the glyph set is unchanged)")
    print("output     : %s" % os.path.relpath(out, ROOT))
    if args.check:
        print("check mode : nothing generated")
        return 0

    cmd = [NODE, conv] + chosen
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise SystemExit("lv_font_conv failed with %d" % r.returncode)

    size = os.path.getsize(out)
    print("done       : %s (%d bytes, size=%d, bpp=%d)"
          % (os.path.relpath(out, ROOT), size, args.size, args.bpp))

    ok, vlines = verify_bin(out)
    print("verify     : %s" % vlines[0])
    for ln in vlines[1:]:
        print("             %s" % ln)
    if not ok:
        raise SystemExit("generated font fails LVGL 8.3 structural checks (see above)")

    print("SD deploy  : copy to <sd>/fonts/%s (LVGL loads it as 'S:/fonts/%s')"
          % (os.path.basename(out), os.path.basename(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
