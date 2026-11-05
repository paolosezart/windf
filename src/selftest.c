/*
 * selftest.c - host tests for the pure helpers in df_format.h.
 * Run with `make test`. No Windows required.
 */

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "df_format.h"

static int failures = 0;

static void check_size(unsigned long long bytes, int si, int blocks,
                       const wchar_t *want)
{
    DfOptions o;
    wchar_t got[64];
    memset(&o, 0, sizeof(o));
    o.si = si;
    o.blocks = blocks;
    df_format_size(bytes, &o, got, 64);
    if (wcscmp(got, want) != 0) {
        wprintf(L"FAIL size(%llu, si=%d, k=%d): got [%ls] want [%ls]\n",
                bytes, si, blocks, got, want);
        failures++;
    }
}

static void check_join(const wchar_t *dir, const wchar_t *child,
                       const wchar_t *want)
{
    wchar_t got[512];
    df_join_path(got, 512, dir, child);
    if (wcscmp(got, want) != 0) {
        wprintf(L"FAIL join(%ls, %ls): got [%ls] want [%ls]\n",
                dir, child, got, want);
        failures++;
    }
}

static void check_class(const wchar_t *label, const wchar_t *provider,
                        int drive_type, int want)
{
    int got = df_classify(label, provider, drive_type);
    if (got != want) {
        wprintf(L"FAIL classify(label=%ls, provider=%ls, type=%d): got %d want %d\n",
                label ? label : L"", provider ? provider : L"", drive_type, got, want);
        failures++;
    }
}

static void check_merge(unsigned orig, unsigned fg, unsigned want)
{
    unsigned got = df_merge_attr(orig, fg);
    if (got != want) {
        wprintf(L"FAIL merge(orig=0x%02X, fg=0x%02X): got 0x%02X want 0x%02X\n",
                orig, fg, got, want);
        failures++;
    }
}

static void check_dirname(const wchar_t *path, const wchar_t *want)
{
    wchar_t got[512];
    df_dirname(got, 512, path);
    if (wcscmp(got, want) != 0) {
        wprintf(L"FAIL dirname(%ls): got [%ls] want [%ls]\n", path, got, want);
        failures++;
    }
}

int main(void)
{
    /* 1024-based, the default */
    check_size(0ULL,          0, 0, L"0B");
    check_size(1023ULL,       0, 0, L"1023B");
    check_size(1024ULL,       0, 0, L"1.0K");
    check_size(1536ULL,       0, 0, L"1.5K");
    check_size(1048576ULL,    0, 0, L"1.0M");
    check_size(4294967296ULL, 0, 0, L"4.0G");
    check_size(317322100736ULL, 0, 0, L"295.5G");

    /* SI, powers of 1000 */
    check_size(1000ULL,       1, 0, L"1.0k");
    check_size(1000000ULL,    1, 0, L"1.0M");
    check_size(317322100736ULL, 1, 0, L"317.3G");

    /* 1K blocks */
    check_size(1024ULL,       0, 1, L"1");
    check_size(2048ULL,       0, 1, L"2");
    check_size(1025ULL,       0, 1, L"2");
    check_size(0ULL,          0, 1, L"0");

    /* classification */
    check_class(L"Windows 10", L"", 3, 0);        /* local NTFS */
    check_class(L"",           L"", 3, 0);        /* local, no label */
    check_class(L"Mega",       L"Mega", 4, 2);    /* cloud by label */
    check_class(L"rclone-ssh", L"rclone", 4, 2);  /* cloud by provider */
    check_class(L"OneDrive",   L"", 3, 2);        /* cloud, fixed type */
    check_class(L"Data",       L"\\\\server\\share", 4, 1); /* plain net */
    check_class(L"BOX",        L"", 3, 2);        /* case-insensitive */

    /* attribute merging: keep the background, take only the foreground */
    check_merge(0x17, 0x0A, 0x1A);  /* blue bg + green fg -> blue bg + green fg */
    check_merge(0x17, 0x07, 0x17);  /* reset on a blue console stays blue      */
    check_merge(0x07, 0x0E, 0x0E);  /* black console, yellow fg                */
    check_merge(0x07, 0x07, 0x07);  /* classic black console unchanged         */
    check_merge(0x1F, 0x0D, 0x1D);  /* bright white bg + magenta fg            */
    check_merge(0x00, 0x0F, 0x0F);  /* zero original, bright white fg          */

    /* path joining */
    check_join(L"C:\\Windows",        L"System32", L"C:\\Windows\\System32");
    check_join(L"C:\\Windows\\",      L"System32", L"C:\\Windows\\System32");
    check_join(L"C:\\Windows/",       L"System32", L"C:\\Windows/System32");
    check_join(L"C:\\",               L"df.exe",   L"C:\\df.exe");
    check_join(L"",                   L"df.exe",   L"df.exe");
    check_join(L"C:\\Windows",        L"",        L"C:\\Windows\\");

    /* dirname: the directory part, for placing the temp file next to target */
    check_dirname(L"C:\\Windows\\System32\\df.exe", L"C:\\Windows\\System32");
    check_dirname(L"C:\\df.exe",                    L"C:");
    check_dirname(L"df.exe",                        L".");
    check_dirname(L"C:\\Windows\\System32\\",       L"C:\\Windows\\System32");

    if (failures == 0) {
        wprintf(L"all tests passed\n");
        return 0;
    }
    wprintf(L"%d test(s) failed\n", failures);
    return 1;
}
