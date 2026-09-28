#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rebuild the LVGL embedded font, reusing the settings of the current one.

Usage examples
--------------
# swap in a new typeface, keeping size/symbols/metrics of the existing font
python tools/make_ui_font.py --font "assets/DreamHanSansSC-W17.ttf"

# also make sure every character used by the SD card file names is covered
python tools/make_ui_font.py --font "assets/DreamHanSansSC-W17.ttf" \
                             --add-chars-from songlist.txt

# dry run: report what the new font can(not) provide, generate nothing
python tools/make_ui_font.py --font "assets/DreamHanSansSC-W17.ttf" \
                             --add-chars-from songlist.txt --check

Why a script instead of a hand-typed command
--------------------------------------------
1. The real command is ~9700 characters and lives in the "Opts:" comment of the
   generated .c file. Retrying it by hand loses symbol sets silently -> glyphs
   quietly disappear from the UI.
2. Editing that command in PowerShell is a minefield: PS 5.1 reads files as ANSI
   by default (CJK becomes mojibake -> wrong glyphs are generated), and
   splitting on \\s+ eats U+3000 (ideographic space), which is itself one of the
   symbols. Python passes argv as UTF-16 and reads files with an explicit
   encoding, so both traps disappear.
3. lv_font_conv silently skips characters the source font does not have, so the
   result must be verified, not assumed.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = os.path.join(ROOT, 'lib', 'ui', 'src', 'ui_font_AlibabaPuHuiTi12.c')
LV_FONT_CONV = os.path.join(
    os.environ.get('APPDATA', ''), 'npm', 'node_modules', 'lv_font_conv', 'lv_font_conv.js')

# Options that carry no per-font meaning and are simply kept as-is.
KEEP_AS_IS = {'--size', '--bpp', '--format', '--lv-include', '--lv-font-name',
              '--no-compress', '--no-prefilter', '--no-kerning', '--lcd', '--lcd-v',
              '--use-color-info', '--autohint-off', '--autohint-strong',
              '--force-fast-kern-format', '--full-info', '--lv-fallback', '-o', '--output'}
# Options that bind to the previously declared --font and must be re-attached.
PER_FONT = {'-r', '--range', '--symbols'}


def read_opts(path):
    """Pull the 'Opts:' command line that lv_font_conv embeds in its output."""
    with open(path, 'r', encoding='utf-8', errors='replace') as fp:
        head = fp.read(200000)
    m = re.search(r'Opts: (.*?)\r?\n \*', head, re.S)
    if not m:
        raise SystemExit('no "Opts:" comment found in %s' % path)
    # ASCII whitespace only: U+3000 is a legitimate symbol, not a separator.
    return [t for t in re.split(r'[ \t]+', m.group(1).strip()) if t]


def split_argv(argv):
    """Return (globals_and_perfont_options_in_order, symbol_option_count)."""
    out = []
    symbols = []
    i = 0
    while i < len(argv):
        t = argv[i]
        if t == '--font':
            i += 2
            continue
        if t in PER_FONT:
            if t == '--range':
                t = '-r'
            symbols.append((t, argv[i + 1]))
            i += 2
            continue
        if t in KEEP_AS_IS:
            out.append((t, argv[i + 1] if t != '--no-compress' else None))
            i += 1 if t == '--no-compress' else 2
            continue
        # unknown / valueless flag: keep verbatim
        out.append((t, None))
        i += 1
    return out, symbols


def get_opt(pairs, key, default=None):
    for k, v in pairs:
        if k == key:
            return v
    return default


def font_metrics(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as fp:
        txt = fp.read()
    def g(pat):
        m = re.search(pat, txt)
        return int(m.group(1)) if m else None
    glyphs = set()
    for m in re.finditer(r'/\* U\+([0-9A-Fa-f]{4,6}) ', txt):
        glyphs.add(int(m.group(1), 16))
    return {
        'line_height': g(r'\.line_height = (-?\d+)'),
        'base_line': g(r'\.base_line = (-?\d+)'),
        'bpp': g(r'\.bpp = (\d+)'),
        'name': (re.search(r'const lv_font_t (\w+) =', txt) or [None, None])[1]
                if re.search(r'const lv_font_t (\w+) =', txt) else None,
        'glyphs': glyphs,
    }


def chars_from_files(paths):
    out = set()
    for p in paths:
        with open(p, 'r', encoding='utf-8', errors='replace') as fp:
            for ch in fp.read():
                if ord(ch) >= 32:
                    out.add(ch)
    return out


def cmap_coverage(font_paths, chars):
    """Use fontTools (if present) to see which chars the source fonts can draw."""
    try:
        from fontTools.ttLib import TTFont, TTCollection
    except ImportError:
        return None
    missing = set(chars)
    for p in font_paths:
        try:
            fonts = TTCollection(p, lazy=True).fonts if p.lower().endswith('.ttc') \
                else [TTFont(p, lazy=True)]
        except Exception as e:
            sys.stderr.write('cannot open %s: %s\n' % (p, e))
            continue
        for f in fonts:
            try:
                cmap = f.getBestCmap()
            except Exception:
                continue
            for ch in list(missing):
                if ord(ch) in cmap:
                    missing.discard(ch)
    return missing


def main():
    ap = argparse.ArgumentParser(description='Rebuild the LVGL embedded font.')
    ap.add_argument('--font', action='append', required=True,
                    help='source font (repeatable, first one wins per glyph)')
    ap.add_argument('--inherit', default=DEFAULT_SOURCE,
                    help='existing generated font whose Opts are reused')
    ap.add_argument('--out', default=None, help='output .c (default: inherit target)')
    ap.add_argument('--name', default=None, help='--lv-font-name override')
    ap.add_argument('--size', type=int, default=None)
    ap.add_argument('--bpp', type=int, default=None)
    ap.add_argument('--add-chars-from', action='append', default=[],
                    help='file whose characters must also be included (repeatable)')
    ap.add_argument('--check', action='store_true', help='report only, do not generate')
    args = ap.parse_args()

    old_argv = read_opts(args.inherit)
    keep, symbols = split_argv(old_argv)
    old_out = get_opt(keep, '-o') or get_opt(keep, '--output')
    out = args.out or old_out or DEFAULT_SOURCE
    name = args.name or get_opt(keep, '--lv-font-name')
    size = args.size or get_opt(keep, '--size') or '12'
    bpp = args.bpp or get_opt(keep, '--bpp') or '4'
    lv_include = get_opt(keep, '--lv-include') or 'lvgl.h'

    before = font_metrics(args.inherit)
    print('inherit    : %s' % os.path.relpath(args.inherit, ROOT))
    print('  line_height=%s base_line=%s bpp=%s glyphs=%d'
          % (before['line_height'], before['base_line'], before['bpp'], len(before['glyphs'])))
    print('  symbol sets kept: %d' % len(symbols))

    # ---- characters that must end up in the font -------------------------
    extra = chars_from_files(args.add_chars_from)
    if extra:
        print('add-chars  : %d unique characters from %d file(s)'
              % (len(extra), len(args.add_chars_from)))
        cov = cmap_coverage(args.font, extra)
        if cov is None:
            print('  (fontTools not installed, skipping source cmap pre-check)')
        elif cov:
            print('  NOT available in the source font(s): %d' % len(cov))
            print('    ' + ' '.join('U+%04X' % ord(c) for c in sorted(cov)[:60]))
        else:
            print('  all of them are available in the source font(s)')

    if args.check:
        print('check mode: nothing generated')
        return 0

    # ---- rebuild the command --------------------------------------------
    argv = ['--size', str(size), '--bpp', str(bpp), '--format', 'lvgl',
            '--lv-include', lv_include, '--lv-font-name', name]
    for p in args.font:
        argv += ['--font', p]
    for opt, val in symbols:
        argv += [opt, val]
    if extra:
        # every extra character goes through one more --symbols entry
        argv += ['--symbols', ''.join(sorted(extra))]
    argv += ['--no-compress', '-o', out]

    print('generating : %s' % os.path.relpath(out, ROOT))
    print('  argv items=%d  total chars=%d' % (len(argv), sum(len(a) + 1 for a in argv)))
    if not os.path.isfile(LV_FONT_CONV):
        raise SystemExit('lv_font_conv not found at %s' % LV_FONT_CONV)
    r = subprocess.run(['node', LV_FONT_CONV] + argv)
    if r.returncode != 0:
        raise SystemExit('lv_font_conv failed with %d' % r.returncode)

    # ---- verify ----------------------------------------------------------
    after = font_metrics(out)
    print('')
    print('result     : glyphs=%d (was %d, gained %d)'
          % (len(after['glyphs']), len(before['glyphs']),
             len(after['glyphs'] - before['glyphs'])))
    lost = before['glyphs'] - after['glyphs']
    print('  regression (glyphs lost): %d' % len(lost))
    if lost:
        print('    ' + ' '.join('U+%04X' % c for c in sorted(lost)[:60]))
    print('  line_height=%s (was %s)  base_line=%s (was %s)  bpp=%s  name=%s'
          % (after['line_height'], before['line_height'],
             after['base_line'], before['base_line'], after['bpp'], after['name']))
    if after['line_height'] != before['line_height'] or after['base_line'] != before['base_line']:
        print('  WARNING: vertical metrics changed -- check the UI layout.')
    if extra:
        miss = extra - {chr(c) for c in after['glyphs']}
        print('  required chars still missing: %d' % len(miss))
        if miss:
            print('    ' + ' '.join('U+%04X' % ord(c) for c in sorted(miss)[:60]))
    print('  size: %d bytes' % os.path.getsize(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
