#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host-side regression test for the lyric encoding adapter.

Builds tools/lyric_encoding_test.cpp together with the real
lib/Music/LyricEncoding.cpp using a host compiler, feeds it byte-exact test
vectors (UTF-8 / GBK / GB2312 / UTF-16 / mislabelled tags), and compares the
decoder output against the expected UTF-8 bytes.

All console output is ASCII so it survives any console code page.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MUSIC = os.path.join(ROOT, 'lib', 'Music')
WORK = os.path.join(tempfile.gettempdir(), 'lyric_enc_test')

# --- lyric samples -----------------------------------------------------------
LRC_CN = (
    "[ti:Test Song]\n"
    "[ar:Test Artist]\n"
    "[00:00.00]作词：某人\n"
    "[00:12.34]这是第一行歌词，带全角标点（括号）与数字 123\n"
    "[00:15.00]第二行：月光洒在窗台上\n"
    "[01:02.50]第三行 end of line\n"
)
LRC_JP = "[00:01.00]だんだん早くなる\n[00:05.00]夜の空に\n"
LRC_GBK_EXT = "[00:01.00]镕 珮 喆 玥 頔 燚 犇\n"          # GBK-only chars, not GB2312
# Real accented Latin-1 text: must survive the mojibake repair untouched.
LRC_LATIN1 = ("Ou sont les fleurs de mon jardin d'ete? "
              "Elles etaient tres belles et parfumees, a l'ombre du vieux mur. "
              "Ou sont les chansons que nous chantions ensemble, "
              "un soir de printemps, pres de la riviere endormie?")


def latin1_expand(raw: bytes) -> bytes:
    """Reproduce AudioFileSourceID3's copyLatin1ToUTF8(): each byte -> U+00XX."""
    s = ''.join(chr(b) for b in raw)
    return s.encode('utf-8')


def utf8_sanitize(raw: bytes) -> bytes:
    """Replica of copyUtf8Sanitized(): invalid bytes become '?'."""
    out = bytearray()
    i = 0
    n = len(raw)
    while i < n:
        b = raw[i]
        if b < 0x80:
            out.append(b); i += 1; continue
        if 0xC2 <= b <= 0xDF:
            ln = 2
        elif 0xE0 <= b <= 0xEF:
            ln = 3
        elif 0xF0 <= b <= 0xF4:
            ln = 4
        else:
            out.append(0x3F); i += 1; continue
        if i + ln > n:
            out.append(0x3F); i += 1; continue
        ok = all(0x80 <= raw[i + k] <= 0xBF for k in range(1, ln))
        cp = 0
        if ok:
            if ln == 2:
                cp = ((b & 0x1F) << 6) | (raw[i + 1] & 0x3F)
                ok = cp >= 0x80
            elif ln == 3:
                cp = ((b & 0x0F) << 12) | ((raw[i + 1] & 0x3F) << 6) | (raw[i + 2] & 0x3F)
                ok = cp >= 0x800 and not (0xD800 <= cp <= 0xDFFF)
            else:
                cp = ((b & 0x07) << 18) | ((raw[i + 1] & 0x3F) << 12) | ((raw[i + 2] & 0x3F) << 6) | (raw[i + 3] & 0x3F)
                ok = 0x10000 <= cp <= 0x10FFFF
        if ok:
            out.extend(raw[i:i + ln]); i += ln
        else:
            out.append(0x3F); i += 1
    return bytes(out)


def build_cases():
    """Return a list of (name, mode, input_bytes, expected_bytes_or_None)."""
    cases = []
    utf = LRC_CN.encode('utf-8')

    cases.append(('utf8_plain', 'utf8', utf, utf))
    cases.append(('ascii_only', 'utf8', b"[00:01.00]Hello world\n", b"[00:01.00]Hello world\n"))
    cases.append(('empty', 'utf8', b"", b""))
    cases.append(('gbk_lrc', 'utf8', LRC_CN.encode('gbk'), utf))
    cases.append(('gbk_japanese', 'utf8', LRC_JP.encode('gbk'), LRC_JP.encode('utf-8')))
    cases.append(('gbk_only_chars', 'utf8', LRC_GBK_EXT.encode('gbk'), LRC_GBK_EXT.encode('utf-8')))

    # GB2312 is a strict subset for the characters used here
    gb2312_text = "[00:01.00]春眠不觉晓，处处闻啼鸟。\n"
    cases.append(('gb2312_lrc', 'utf8', gb2312_text.encode('gb2312'), gb2312_text.encode('utf-8')))

    cases.append(('utf8_with_bom', 'utf8', b'\xef\xbb\xbf' + utf, utf))
    cases.append(('utf16le_bom', 'utf8', b'\xff\xfe' + LRC_CN.encode('utf-16-le'), utf))
    cases.append(('utf16be_bom', 'utf8', b'\xfe\xff' + LRC_CN.encode('utf-16-be'), utf))

    # UTF-8 file with one stray GBK sequence in the middle.
    # Per-line detection now recovers the GBK part properly ("测") instead of
    # degrading it to "??", so the expectation is the recovered text.
    long_utf8 = ("[ti:Long]\n" + "".join(
        "[%02d:%02d.00]%s\n" % (m, s, LRC_CN.splitlines()[2 + (m % 2)]) for m in range(40) for s in (0, 30)
    )).encode('utf-8')
    bad = long_utf8 + '\u6d4b'.encode('gbk') + b" tailline\n" + long_utf8
    cases.append(('utf8_one_bad', 'utf8', bad,
                  long_utf8 + '\u6d4b tailline\n'.encode('utf-8') + long_utf8))
    # short mixed file: below the tolerance threshold, ASCII parts still survive
    short_bad = "[00:01.00]ok".encode('utf-8') + '\u6d4b'.encode('gbk') + " tail".encode('utf-8')
    cases.append(('short_bad_mixed', 'utf8', short_bad,
                  '[00:01.00]ok'.encode('utf-8') + '\u6d4b'.encode('utf-8') + " tail".encode('utf-8')))

    # ------------------------------------------------------------------
    # The real-world failure: ONE FILE with a UTF-8 body but GBK metadata
    # lines. A whole-file decision cannot serve this.
    # Observed on device as:  "u<U+016F>... Hot Spring Affection - HOYO-MiX"
    # (GBK C5AF C8AA = "暖泉" happens to be VALID UTF-8 -> U+016F U+01EA,
    #  which is why a byte-ratio test alone never notices.)
    # ------------------------------------------------------------------
    gbk_head = ("[00:00.00]暖泉的遐仰 Hot Spring Affection - HOYO-MiX\n"
                "[00:01.00]作曲 Composer：Xin Zhao (HOYO-MiX)\n"
                "[00:02.00]编曲 Arranger：Xin Zhao (HOYO-MiX)\n")
    utf8_body = "".join("[%02d:%02d.00]这是正文第%d行，编码是 UTF-8\n" % (m, s, m)
                        for m in range(30) for s in (0, 30))
    mixed = gbk_head.encode('gbk') + utf8_body.encode('utf-8')
    cases.append(('mixed_gbk_meta', 'utf8', mixed,
                  gbk_head.encode('utf-8') + utf8_body.encode('utf-8')))

    # same idea, but the GBK part is a single short line whose bytes are ALL
    # valid UTF-8 -> only the "suspicious code point" rule can catch it
    amb = "[00:01.00]暖泉\n".encode('gbk') + utf8_body.encode('utf-8')
    cases.append(('gbk_meta_all_valid_utf8', 'utf8', amb,
                  "[00:01.00]暖泉\n".encode('utf-8') + utf8_body.encode('utf-8')))

    # a genuinely non-CJK UTF-8 file must survive untouched
    ru = "[ti:Песня]\n[00:01.00]Привет мир, это тест\n".encode('utf-8')
    cases.append(('utf8_cyrillic', 'utf8', ru, ru))
    gr = "[00:01.00]Καλημέρα κόσμε\n".encode('utf-8')
    cases.append(('utf8_greek', 'utf8', gr, gr))

    # --- the "declared ISO-8859-1 but actually GBK" tag case ---
    cases.append(('uslt_mojibake', 'repair', latin1_expand(LRC_CN.encode('gbk')), utf))
    # genuine Latin-1 text must be left alone
    fr = latin1_expand(LRC_LATIN1.encode('latin-1'))
    cases.append(('latin1_real', 'repair', fr, fr))
    # UTF-8 Chinese must not be touched by the repair step
    cases.append(('repair_noop_utf8', 'repair', utf, utf))

    # truncation must land on a character boundary
    cases.append(('trunc_gbk', 'trunc', LRC_CN.encode('gbk'), None))
    cases.append(('trunc_utf8', 'trunc', utf, None))
    return cases


SHIM = r'''/* Host-build stand-in for ESP-IDF's esp_heap_caps.h.
   On the device the lyric scratch buffers come from PSRAM; on the host we just
   use malloc, so the very same LyricEncoding.cpp compiles unmodified. */
#ifndef ESP_HEAP_CAPS_SHIM_H
#define ESP_HEAP_CAPS_SHIM_H
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void  heap_caps_free(void *p) { free(p); }
#endif
'''


def compile_harness(exe):
    shim_dir = os.path.join(WORK, 'shim')
    os.makedirs(shim_dir, exist_ok=True)
    with open(os.path.join(shim_dir, 'esp_heap_caps.h'), 'w', encoding='ascii', newline='\n') as fp:
        fp.write(SHIM)

    src = [os.path.join(ROOT, 'tools', 'lyric_encoding_test.cpp'),
           os.path.join(MUSIC, 'LyricEncoding.cpp')]
    cmd = ['g++', '-std=c++17', '-O1', '-Wall', '-I', shim_dir, '-I', MUSIC, '-o', exe] + src
    print('CC: ' + ' '.join(cmd))
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = r.stdout.decode('utf-8', 'replace')
    if out.strip():
        print(out)
    if r.returncode != 0:
        print('compile FAILED')
        return False
    return True


def main():
    os.makedirs(WORK, exist_ok=True)
    exe = os.path.join(WORK, 'lyrtest.exe')
    if not compile_harness(exe):
        return 1

    cases = build_cases()
    manifest = os.path.join(WORK, 'manifest.txt')
    lines = []
    for i, (name, mode, data, _exp) in enumerate(cases):
        ip = os.path.join(WORK, '%02d_%s.in' % (i, name))
        op = os.path.join(WORK, '%02d_%s.out' % (i, name))
        with open(ip, 'wb') as f:
            f.write(data)
        lines.append('%02d_%s %s %s %s' % (i, name, mode, ip, op))
    with open(manifest, 'w', encoding='ascii', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')

    print('--- decoder output ---')
    r = subprocess.run([exe, manifest], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(r.stdout.decode('utf-8', 'replace'))

    print('--- comparison ---')
    passed = failed = 0
    for i, (name, mode, data, exp) in enumerate(cases):
        op = os.path.join(WORK, '%02d_%s.out' % (i, name))
        with open(op, 'rb') as f:
            got = f.read()

        if exp is None:
            # truncation: must be a valid UTF-8 prefix and non-empty
            try:
                got.decode('utf-8')
                ok = len(got) > 0
                note = 'valid-utf8-prefix len=%d' % len(got)
            except UnicodeDecodeError:
                ok = False
                note = 'BROKEN utf-8 (split sequence!)'
        else:
            ok = (got == exp)
            note = 'ok' if ok else 'len got=%d want=%d' % (len(got), len(exp))

        if ok:
            passed += 1
            print('  PASS %-18s %s' % (name, note))
        else:
            failed += 1
            print('  FAIL %-18s %s' % (name, note))
            if exp is not None:
                print('       got : %s' % got[:80])
                print('       want: %s' % exp[:80])

    print('--- result: %d passed, %d failed ---' % (passed, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
