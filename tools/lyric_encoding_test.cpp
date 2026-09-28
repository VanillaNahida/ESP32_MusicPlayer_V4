/*
  lyric_encoding_test.cpp -- host-side harness for lib/Music/LyricEncoding.cpp

  This is NOT part of the firmware (tools/ is not compiled by PlatformIO).
  It exists so the lyric decoder can be tested against real byte streams on the
  build machine, where we can actually run it and compare results.

  Build and run it with:  python tools/test_lyric_encoding.py
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LyricEncoding.h"

static unsigned char *readAll(const char *path, size_t *lenOut)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) n = 0;
    unsigned char *p = (unsigned char *)malloc((size_t)n + 1);
    if (p == NULL) { fclose(f); return NULL; }
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    p[got] = 0;
    *lenOut = got;
    return p;
}

static int writeAll(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    fwrite(data, 1, len, f);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <manifest>\n", argv[0]);
        return 2;
    }
    FILE *mf = fopen(argv[1], "r");
    if (mf == NULL)
    {
        fprintf(stderr, "cannot open manifest %s\n", argv[1]);
        return 2;
    }

    char idx[64], mode[64], inPath[1024], outPath[1024];
    int fails = 0, cases = 0;

    while (fscanf(mf, "%63s %63s %1023s %1023s", idx, mode, inPath, outPath) == 4)
    {
        cases++;
        size_t n = 0;
        unsigned char *in = readAll(inPath, &n);
        if (in == NULL)
        {
            printf("%s FAIL(cannot-read-input)\n", idx);
            fails++;
            continue;
        }
        const char *enc = "-";

        if (strcmp(mode, "utf8") == 0)
        {
            size_t cap = n * 3 + 64;
            char *out = (char *)malloc(cap);
            size_t olen = LyricEncoding_ToUtf8(in, n, out, cap, &enc);
            writeAll(outPath, out, olen);
            printf("%-18s in=%-6u out=%-6u enc=%s\n", idx, (unsigned)n, (unsigned)olen, enc);
            free(out);
        }
        else if (strcmp(mode, "trunc") == 0)
        {
            /* Tiny buffer: the decoder must truncate on a character boundary and
               never emit half a UTF-8 sequence. */
            char small[17];
            size_t olen = LyricEncoding_ToUtf8(in, n, small, sizeof(small), &enc);
            writeAll(outPath, small, olen);
            printf("%-18s in=%-6u out=%-6u enc=%s\n", idx, (unsigned)n, (unsigned)olen, enc);
        }
        else if (strcmp(mode, "repair") == 0)
        {
            size_t cap = n * 4 + 64;
            char *out = (char *)malloc(cap);
            memcpy(out, in, n);
            out[n] = 0;
            int fixed = LyricEncoding_RepairLatin1Mojibake(out, cap) ? 1 : 0;
            size_t olen = strlen(out);
            writeAll(outPath, out, olen);
            printf("%-18s in=%-6u out=%-6u repaired=%d\n", idx, (unsigned)n, (unsigned)olen, fixed);
            free(out);
        }
        else
        {
            printf("%s FAIL(unknown-mode)\n", idx);
            fails++;
        }
        free(in);
    }

    fclose(mf);
    printf("harness_done cases=%d fails=%d\n", cases, fails);
    return fails ? 1 : 0;
}
