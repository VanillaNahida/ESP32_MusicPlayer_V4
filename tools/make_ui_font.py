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

# 复用 gen_lvgl_font_bin.py 里的「代价感知字符集编码 + 自适应合并」工具。
# 为什么需要它：全量字符集有 4 万多个码点，直接拼命令行会超过 Windows
# CreateProcess 的 32767 字节上限（WinError 206）。那个脚本已经解决了这个问题
# （长连续段用 -r、孤立码点用字面量、必要时按级合并区间）。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import gen_lvgl_font_bin as gfb
except ImportError:
    gfb = None

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


def patch_metrics(path, line_height, base_line):
    """把生成结果里的 line_height/base_line 改回旧字库的值。

    为什么需要这两个值会变：lv_font_conv 按**所选字符集**里所有字形的最大外接框
    计算 line_height/base_line。字符集一放大它们就跟着变
    （实测：7,710 字形 → 15/3；全量 44,812 字形 → 23/7；只选 1 个字 → 11/1）。
    而界面布局是按 15 调好的，行距一变文本就会溢出/错位。

    为什么改回去是安全的：字形自身的 ofs_y/ofs_x/box_w/box_h/adv_w **只取决于
    源字体和字号**，与字符集无关（已实测：同一个「中」在单字子集和全量里都是
    box=10x11 ofs=(1,-1) adv_w=192，与 bin 全量字体也逐字段一致）。
    所以沿用旧的 line_height/base_line，渲染结果与旧字库逐像素一致。
    """
    with open(path, 'r', encoding='utf-8', errors='replace') as fp:
        txt = fp.read()
    new, n1 = re.subn(r'\.line_height\s*=\s*-?\d+', '.line_height = %d' % line_height, txt, count=1)
    new, n2 = re.subn(r'\.base_line\s*=\s*-?\d+', '.base_line = %d' % base_line, new, count=1)
    if n1 != 1 or n2 != 1:
        sys.stderr.write('keep-metrics: 改写失败 (line_height=%d hit, base_line=%d hit)\n' % (n1, n2))
        return False
    with open(path, 'w', encoding='utf-8') as fp:
        fp.write(new)
    print('  keep-metrics: line_height=%d base_line=%d（沿用旧字库，保持界面排版）'
          % (line_height, base_line))
    return True


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
    ap.add_argument('--full', action='store_true',
                    help='用源字体支持的**全部**码点（最全字符集）；此时忽略继承来的 --symbols 表')
    ap.add_argument('--keep-metrics', action='store_true',
                    help='生成后把 line_height/base_line 改回继承字体的值，避免字符集变化导致排版走样')
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

    if args.full:
        # 全量：用源字体 cmap 里的**全部码点**，替掉继承来的那几段 --symbols
        if gfb is None:
            raise SystemExit('--full 需要同目录的 gen_lvgl_font_bin.py，导入失败')
        cps = gfb.sanitize(gfb.load_cmap(args.font[0]))
        for c in extra:
            cps.add(ord(c))
        runs = gfb.coalesce(cps)
        print('full-charset: %d code points in %d runs' % (len(cps), len(runs)))
        picked = None
        for gap in gfb.GAP_LADDER:
            merged = gfb.merge_ranges(runs, gap) if gap else runs
            rr, sym = gfb.split_runs(merged)
            cand = list(argv)
            for b in gfb.batch_tokens([gfb.fmt_range(a, b2) for (a, b2) in rr],
                                      gfb.RANGE_BATCH_BYTES):
                cand += ['-r', b]
            for s in gfb.batch_tokens(sym, gfb.SYMBOLS_CHUNK_BYTES, sep=''):
                if s:
                    cand += ['--symbols=' + s]
            cand += ['--no-compress', '-o', out]
            n = gfb.command_line_bytes(cand, LV_FONT_CONV)
            if n <= gfb.ARGV_BUDGET_BYTES:
                print('  encoding: %d range token(s) + %d literal symbol(s), merged gaps<=%d'
                      % (len(rr), len(sym), gap))
                print('  argv: %d bytes (limit 32767, budget %d)  [OK]' % (n, gfb.ARGV_BUDGET_BYTES))
                picked = cand
                break
        if picked is None:
            raise SystemExit('无法把字符集压进命令行上限（这不该发生）')
        argv = picked
    else:
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

    if args.keep_metrics and before['line_height'] is not None:
        patch_metrics(out, before['line_height'], before['base_line'])
        after = font_metrics(out)
        print('  metrics now: line_height=%s base_line=%s'
              % (after['line_height'], after['base_line']))
    if extra:
        miss = extra - {chr(c) for c in after['glyphs']}
        print('  required chars still missing: %d' % len(miss))
        if miss:
            print('    ' + ' '.join('U+%04X' % ord(c) for c in sorted(miss)[:60]))
    print('  size: %d bytes' % os.path.getsize(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
