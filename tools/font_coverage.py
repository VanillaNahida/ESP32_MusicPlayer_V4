#!/usr/bin/env python3
"""检查「已经生成好的 LVGL 点阵字体」到底覆盖了哪些字符。

make_ui_font.py --check 只能问源 TTF 有没有这个字形；
但真正决定屏幕上出不出方块的是**生成出来的 .c**——它的字符集是当初
lv_font_conv 那一次的 charset 决定的，源字体里有、charset 里没要，
一样不会被打进固件。

用法:
    python tools/font_coverage.py --font lib/ui/src/ui_font_AlibabaPuHuiTi12.c \
                                  --chars-from songlist.txt --chars-from lib/ui/src/ui_events.cpp

退出码 0 = 全覆盖；1 = 有缺字（会列出来）。
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# lv_font_fmt_txt.c: rcp = letter - range_start，所以 unicode_list 里存的是
# 「相对 range_start 的偏移」，不是绝对码点（见 lv_font_fmt_txt.c:222）。
ARRAY_RE = re.compile(r'static const uint16_t (\w+)\[\]\s*=\s*\{(.*?)\};', re.S)
CMAP_RE = re.compile(
    r'\.range_start\s*=\s*(\d+)\s*,\s*\.range_length\s*=\s*(\d+)\s*,'
    r'\s*\.glyph_id_start\s*=\s*(\d+)\s*,\s*\.unicode_list\s*=\s*(\w+|NULL)\s*,'
    r'\s*\.glyph_id_ofs_list\s*=\s*(\w+|NULL)\s*,\s*\.list_length\s*=\s*(\d+)\s*,'
    r'\s*\.type\s*=\s*(\w+)', re.S)


def parse_font(path):
    text = Path(path).read_text(encoding='utf-8', errors='replace')

    arrays = {}
    for name, body in ARRAY_RE.findall(text):
        arrays[name] = [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]

    cmaps_block = text.split('lv_font_fmt_txt_cmap_t cmaps[]', 1)
    if len(cmaps_block) != 2:
        raise SystemExit('找不到 cmaps[]，文件不像 lv_font_conv 生成的字体: %s' % path)

    covered = set()
    cmaps = []
    for rs, rl, gid, ulist, gofs, n, typ in CMAP_RE.findall(cmaps_block[1]):
        rs, rl, n = int(rs), int(rl), int(n)
        cmaps.append((rs, rl, ulist, n, typ))
        if typ.endswith('SPARSE_TINY'):
            if ulist == 'NULL':
                continue
            for v in arrays.get(ulist, [])[:n]:
                covered.add(rs + v)
        else:  # FORMAT0_TINY：一段连续码点
            covered.update(range(rs, rs + rl))
    return covered, cmaps


def strip_comments(text):
    """去掉 C/C++ 注释，但**保留字符串字面量里的内容**。

    只有能显示到屏幕上的字才需要字形：注释里的汉字（、或者说明用的制表符）
    缺了也无所谓，不该被报成缺字。反过来，字符串里的字一个都不能漏，
    所以这里不能用「按 // 切一刀」的土办法 —— 那会把 "http://..." 也切了。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            q = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i])
                    out.append(text[i + 1])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


CODE_SUFFIXES = {'.c', '.cpp', '.cc', '.h', '.hpp'}

# 日志调用里的字符串不需要字形 —— 它们只走串口，不上屏。
# 不排掉的话，日志里随便写句中文就会把这项检查变成噪音，检查也就没人看了。
LOG_CALL_RE = re.compile(r'\bSerial\s*\.\s*\w+\s*\((?:[^;()]|\([^;()]*\))*\)\s*;', re.S)


def strip_log_calls(text):
    """把 Serial.xxx(...) 调用整段删掉（连带里面的字符串字面量）。"""
    return LOG_CALL_RE.sub('Serial.log();', text)


def main():
    ap = argparse.ArgumentParser(description='Check glyph coverage of a generated LVGL font.')
    ap.add_argument('--font', default='lib/ui/src/ui_font_AlibabaPuHuiTi12.c')
    ap.add_argument('--chars-from', action='append', default=[],
                    help='file whose characters must be covered (repeatable)')
    ap.add_argument('--keep-comments', action='store_true',
                    help='把注释里的字也算进来（默认只算源码里真正能显示出来的字）')
    ap.add_argument('--keep-logs', action='store_true',
                    help='把 Serial 日志里的字也算进来（默认不算，日志不上屏）')
    args = ap.parse_args()

    covered, cmaps = parse_font(args.font)
    print('font       : %s' % args.font)
    print('  cmaps    : %d, 覆盖码点 %d 个' % (len(cmaps), len(covered)))

    if not args.chars_from:
        print('没有给 --chars-from，只报告字体本身的规模')
        return 0

    want = set()
    for p in args.chars_from:
        fp = Path(p)
        if not fp.is_absolute():
            fp = ROOT / p
        if not fp.exists():
            print('!! 找不到 %s' % p, file=sys.stderr)
            return 2
        text = fp.read_text(encoding='utf-8', errors='replace')
        if fp.suffix.lower() in CODE_SUFFIXES:
            if not args.keep_logs:
                text = strip_log_calls(text)
            if not args.keep_comments:
                text = strip_comments(text)
        want.update(ch for ch in text if ord(ch) >= 32)

    missing = sorted(ch for ch in want if ord(ch) not in covered)
    print('  需要字符 : %d 个（来自 %d 个文件）' % (len(want), len(args.chars_from)))
    if not missing:
        print('  [OK] 全部都有')
        return 0

    print('  [MISSING] 缺 %d 个：' % len(missing))
    for ch in missing:
        print('      %s  U+%04X' % (ch, ord(ch)))
    print()
    print('把这些字补进 charset 后重新生成字体，例如：')
    print('  python tools/make_ui_font.py --font assets/DreamHanSansSC-W17.ttf \\')
    print('         --add-chars-from <缺字的那个文件>')
    return 1


if __name__ == '__main__':
    sys.exit(main())
