/*
 * df_format.h - pure helpers shared by the Windows program and the tests.
 *
 * Nothing here touches the Windows API, so the same code compiles on a host
 * (for `make test`) and inside the Windows build.
 */

#ifndef WINDF_FORMAT_H
#define WINDF_FORMAT_H

#include <wchar.h>
#include <stddef.h>
#include <wctype.h>

typedef struct {
    int human;
    int si;
    int blocks;
    int show_type;
    int all;
    int no_color;
} DfOptions;

#ifdef _WIN32
#define DF_SNWPRINTF _snwprintf
#else
#define DF_SNWPRINTF swprintf
#endif

/* Human-readable size. blocks -> 1K units; si -> powers of 1000. */
static void df_format_size(unsigned long long bytes, const DfOptions *o,
                           wchar_t *out, size_t outlen)
{
    static const wchar_t *u1024[] = { L"B", L"K", L"M", L"G", L"T", L"P" };
    static const wchar_t *u1000[] = { L"B", L"k", L"M", L"G", L"T", L"P" };
    const wchar_t **units = o->si ? u1000 : u1024;
    double base = o->si ? 1000.0 : 1024.0;
    double v;
    int i = 0;

    if (o->blocks) {
        unsigned long long blocks = (bytes + 1023) / 1024;
        DF_SNWPRINTF(out, outlen, L"%llu", blocks);
        return;
    }

    v = (double)bytes;
    while (v >= base && i < 5) { v /= base; i++; }

    if (i == 0) DF_SNWPRINTF(out, outlen, L"%.0f%s", v, units[i]);
    else        DF_SNWPRINTF(out, outlen, L"%.1f%s", v, units[i]);
}

static int df_contains_ci(const wchar_t *haystack, const wchar_t *needle)
{
    wchar_t h[512], n[512];
    size_t i;
    wcsncpy(h, haystack, 511); h[511] = 0;
    wcsncpy(n, needle, 511);   n[511] = 0;
    for (i = 0; h[i]; ++i) h[i] = (wchar_t)towlower(h[i]);
    for (i = 0; n[i]; ++i) n[i] = (wchar_t)towlower(n[i]);
    return wcsstr(h, n) != NULL;
}

/* 0 = LOCAL, 1 = NET, 2 = CLOUD */
static int df_classify(const wchar_t *label, const wchar_t *provider,
                       int drive_type)
{
    static const wchar_t *kw[] = {
        L"mega", L"rclone", L"onedrive", L"dropbox", L"google drive", L"gdrive",
        L"yandex", L"webdav", L"nextcloud", L"icloud", L"pcloud", L"box", NULL
    };
    int i;

    for (i = 0; kw[i]; ++i) {
        if (label && label[0] && df_contains_ci(label, kw[i])) return 2;
        if (provider && provider[0] && df_contains_ci(provider, kw[i])) return 2;
    }
    if (drive_type == 4 /* DRIVE_REMOTE */) return 1;
    if (provider && provider[0]) return 1;
    return 0;
}

#endif /* WINDF_FORMAT_H */
