/*
 * windf - df for Windows
 *
 * A native, human-readable disk usage report. It groups output by PHYSICAL
 * disk and lists every logical volume (drive letter) underneath its disk,
 * in the style of coreutils df:
 *
 *     Volume  Type  Size  Used  Avail  Use%  Mounted on
 *
 * Network and cloud mounts (rclone, Mega, OneDrive, WebDAV, mapped network
 * drives, ...) are printed in a separate block and tagged [NET] / [CLOUD].
 *
 * Build: MinGW-w64 (x86_64-w64-mingw32-gcc / i686-w64-mingw32-gcc).
 * Target: Windows Vista and later. No .NET, no runtime dependencies.
 *
 * Uses only stable Win32 APIs present on Vista/7/8/10/11:
 *   - GetLogicalDrives / GetDriveType / GetVolumeInformation
 *   - GetDiskFreeSpaceEx / WNetGetConnection
 *   - CreateFile("\\.\PhysicalDriveN") + DeviceIoControl for disk info
 *       IOCTL_DISK_GET_LENGTH_INFO
 *       IOCTL_STORAGE_QUERY_PROPERTY
 *   - CreateFile("\\.\X:") + IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
 *   - FindFirstVolumeW / GetVolumePathNamesForVolumeNameW
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600 /* Vista */

#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "df_format.h"

#define MAX_DISKS   64
#define MAX_VOLUMES 128
#define MAX_STR     512

typedef DfOptions Options;

typedef struct {
    int       number;
    WCHAR     name[MAX_STR];
    WCHAR     serial[MAX_STR];
    ULONGLONG size;
    WCHAR     bus[MAX_STR];
    WCHAR     media[MAX_STR];
    int       used;
} PhysicalDisk;

typedef struct {
    WCHAR     letter;
    WCHAR     label[MAX_STR];
    WCHAR     fs[MAX_STR];
    ULONGLONG size;
    ULONGLONG free_bytes;
    int       drive_type;
    WCHAR     provider[MAX_STR];
    int       disk_number;
} Volume;

/* ------------------------------------------------------------------ */
/* Console output                                                      */
/* ------------------------------------------------------------------ */

static int stdout_is_console(void)
{
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

/* The console attribute in effect before we touch anything. Captured once so
 * the background (blue in cmd/PowerShell, black in some setups) survives and
 * is restored exactly, instead of being forced to the classic black. */
static int  color_orig_saved = 0;
static WORD color_orig_attr  = 0x07;

static WORD console_orig_attr(void)
{
    if (!color_orig_saved) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        color_orig_attr = 0x07;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
            color_orig_attr = csbi.wAttributes;
        color_orig_saved = 1;
    }
    return color_orig_attr;
}

/* Write a wide string to stdout. Uses WriteConsoleW on a real console and
 * UTF-8 bytes when redirected, which avoids all CRT locale problems. */
static void outw(const WCHAR *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

    if (stdout_is_console()) {
        DWORD written = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &written, NULL);
    } else {
        int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        if (len > 1) {
            char *buf = (char *)malloc((size_t)len);
            if (buf) {
                WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, len, NULL, NULL);
                fwrite(buf, 1, (size_t)(len - 1), stdout);
                free(buf);
            }
        }
    }
}

static void outa(const char *s)
{
    fputs(s, stdout);
}

static void set_color(const Options *o, WORD attr)
{
    if (o->no_color || !stdout_is_console()) return;
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
                            (WORD)df_merge_attr(console_orig_attr(), attr));
}

static void reset_color(const Options *o)
{
    if (o->no_color || !stdout_is_console()) return;
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), console_orig_attr());
}

/* Thin adapters over the shared helpers in df_format.h. */
static void format_size(ULONGLONG bytes, const Options *o, WCHAR *out, size_t outlen)
{
    df_format_size((unsigned long long)bytes, o, out, outlen);
}

static int classify_volume(const Volume *v)
{
    return df_classify(v->label, v->provider, v->drive_type);
}

static const WCHAR *kind_tag(int cls)
{
    if (cls == 2) return L" [CLOUD]";
    if (cls == 1) return L" [NET]";
    return L"";
}

/* ------------------------------------------------------------------ */
/* Physical disks via \\.\PhysicalDriveN                               */
/* ------------------------------------------------------------------ */

static const WCHAR *bus_name(int b)
{
    static const WCHAR *names[] = {
        L"unknown", L"SCSI", L"ATAPI", L"ATA", L"IEEE1394", L"SSA",
        L"Fibre", L"USB", L"RAID", L"iSCSI", L"SAS", L"SATA", L"SD",
        L"MMC", L"Virtual", L"FileBacked", L"Spaces", L"NVMe", L"SCM",
        L"UFS"
    };
    if (b >= 0 && b < (int)(sizeof(names)/sizeof(names[0]))) return names[b];
    return L"unknown";
}

static int enumerate_physical_disks(PhysicalDisk *disks, int max)
{
    int n, count = 0;

    for (n = 0; n < max; ++n) {
        WCHAR path[64];
        HANDLE h;
        PhysicalDisk *d;
        GET_LENGTH_INFORMATION leninfo;
        STORAGE_PROPERTY_QUERY query;
        BYTE outbuf[1024];
        DWORD returned = 0;

        _snwprintf(path, 64, L"\\\\.\\PhysicalDrive%d", n);
        h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        d = &disks[count];
        memset(d, 0, sizeof(*d));
        d->number = n;
        wcscpy(d->bus, L"unknown");
        wcscpy(d->media, L"disk");

        memset(&leninfo, 0, sizeof(leninfo));
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                            &leninfo, sizeof(leninfo), &returned, NULL))
            d->size = (ULONGLONG)leninfo.Length.QuadPart;

        memset(&query, 0, sizeof(query));
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        memset(outbuf, 0, sizeof(outbuf));
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            outbuf, sizeof(outbuf), &returned, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)outbuf;
            WCHAR vendor[128] = L"";
            WCHAR product[256] = L"";

            wcscpy(d->bus, bus_name((int)desc->BusType));
            wcscpy(d->media, desc->RemovableMedia ? L"Removable" : L"Fixed");

            if (desc->VendorIdOffset)
                MultiByteToWideChar(CP_ACP, 0, (char *)outbuf + desc->VendorIdOffset,
                                    -1, vendor, 128);
            if (desc->ProductIdOffset)
                MultiByteToWideChar(CP_ACP, 0, (char *)outbuf + desc->ProductIdOffset,
                                    -1, product, 256);
            if (desc->SerialNumberOffset)
                MultiByteToWideChar(CP_ACP, 0, (char *)outbuf + desc->SerialNumberOffset,
                                    -1, d->serial, MAX_STR);

            {
                size_t L;
                L = wcslen(vendor);  while (L && vendor[L-1]  == L' ') vendor[--L]  = 0;
                L = wcslen(product); while (L && product[L-1] == L' ') product[--L] = 0;
                L = wcslen(d->serial); while (L && (d->serial[L-1] == L' ' ||
                                                    d->serial[L-1] == L'\t'))
                    d->serial[--L] = 0;
            }

            if (vendor[0] || product[0])
                _snwprintf(d->name, MAX_STR, L"%s %s", vendor, product);
            else
                _snwprintf(d->name, MAX_STR, L"PhysicalDrive%d", n);

            {
                size_t L = wcslen(d->name);
                while (L && d->name[L-1] == L' ') d->name[--L] = 0;
            }
        } else {
            _snwprintf(d->name, MAX_STR, L"PhysicalDrive%d", n);
        }

        d->used = 1;
        count++;
        CloseHandle(h);
    }

    return count;
}

/* ------------------------------------------------------------------ */

static void volume_disk_number(WCHAR letter, Volume *v)
{
    WCHAR path[16];
    HANDLE h;
    VOLUME_DISK_EXTENTS extents;
    DWORD returned = 0;

    v->disk_number = -1;
    if (!letter) return;

    _snwprintf(path, 16, L"\\\\.\\%c:", letter);
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    memset(&extents, 0, sizeof(extents));
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                        &extents, sizeof(extents), &returned, NULL)) {
        if (extents.NumberOfDiskExtents >= 1)
            v->disk_number = (int)extents.Extents[0].DiskNumber;
    }
    CloseHandle(h);
}

static int enumerate_volumes(Volume *vols, int max, int include_letterless)
{
    DWORD mask = GetLogicalDrives();
    int count = 0;
    int i;

    for (i = 0; i < 26 && count < max; ++i) {
        WCHAR letter, root[4];
        UINT type;
        Volume *v;
        DWORD serial = 0, maxlen = 0, flags = 0;
        ULARGE_INTEGER fb, tb, tf;

        if (!(mask & (1u << i))) continue;

        letter = (WCHAR)(L'A' + i);
        root[0] = letter; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
        type = GetDriveTypeW(root);

        v = &vols[count];
        memset(v, 0, sizeof(*v));
        v->letter = letter;
        v->drive_type = (int)type;
        v->disk_number = -1;

        GetVolumeInformationW(root, v->label, MAX_STR, &serial, &maxlen,
                              &flags, v->fs, MAX_STR);

        if (GetDiskFreeSpaceExW(root, &fb, &tb, &tf)) {
            v->size = tb.QuadPart;
            v->free_bytes = tf.QuadPart;
        }

        if (type == DRIVE_REMOTE) {
            DWORD len = MAX_STR;
            if (!WNetGetConnectionW(root, v->provider, &len))
                v->provider[MAX_STR - 1] = 0;
        }

        volume_disk_number(letter, v);
        count++;
    }

    /* Letterless volumes (recovery partitions etc.). Hidden by default: they
     * show up as a wall of "(none) 0B" rows and are noise for a normal user.
     * RAM disks and other odd volumes land here too. */
    if (include_letterless) {
        WCHAR volname[MAX_PATH];
        HANDLE hfind = FindFirstVolumeW(volname, MAX_PATH);
        if (hfind != INVALID_HANDLE_VALUE) {
            do {
                WCHAR paths[MAX_PATH];
                BOOL has_letter = FALSE;
                size_t len = wcslen(volname);
                Volume *v;

                if (len && volname[len - 1] == L'\\') volname[len - 1] = 0;

                if (GetVolumePathNamesForVolumeNameW(volname, paths, MAX_PATH, NULL)
                        && paths[0]) {
                    if (iswalpha(paths[0]) && paths[1] == L':') has_letter = TRUE;
                }

                if (!has_letter && count < max) {
                    ULARGE_INTEGER fb, tb, tf;
                    v = &vols[count];
                    memset(v, 0, sizeof(*v));
                    v->letter = 0;
                    v->drive_type = DRIVE_FIXED;
                    v->disk_number = -1;

                    GetVolumeInformationW(volname, v->label, MAX_STR, NULL, NULL,
                                          NULL, v->fs, MAX_STR);
                    if (GetDiskFreeSpaceExW(volname, &fb, &tb, &tf)) {
                        v->size = tb.QuadPart;
                        v->free_bytes = tf.QuadPart;
                    }
                    count++;
                }
            } while (FindNextVolumeW(hfind, volname, MAX_PATH));
            FindVolumeClose(hfind);
        }
    }

    return count;
}

/* ------------------------------------------------------------------ */

static void print_help(void)
{
    outa(
"Usage: df [OPTION]...\n"
"\n"
"Show disk space usage, grouped by physical disk.\n"
"\n"
"  -h, --human-readable   print sizes in powers of 1024 (default)\n"
"  -H, --si               print sizes in powers of 1000\n"
"  -k                     print sizes in 1024-byte blocks\n"
"  -T, --print-type       accepted for df compatibility (Type is always shown)\n"
"  -a, --all              include volumes without a drive letter\n"
"      --no-color         do not colorize output\n"
"  -i, --install          copy df into the Windows system directories\n"
"  -r, --replace          with --install, overwrite an existing df\n"
"      --help             display this help and exit\n"
"\n"
"Install:\n"
"  df --install copies the right build into the system directories so that\n"
"  plain 'df' works from any folder: 64-bit goes to System32, 32-bit goes\n"
"  to SysWOW64. On 32-bit Windows only System32 is used. If a df already\n"
"  exists there, the command stops and says so; add -r to overwrite.\n"
"\n"
"Output blocks:\n"
"  Each physical disk is printed as a block, with all of its logical\n"
"  volumes (drive letters) listed underneath it. Network and cloud\n"
"  mounts are printed in a separate block and marked [NET] or [CLOUD].\n"
"\n"
"Columns:\n"
"  Volume       the drive letter (or '-' for a letterless volume)\n"
"  Type         the filesystem (NTFS, FAT32, exFAT, ...)\n"
"  Size/Used/Avail/Use%\n"
"               space in bytes, and the percentage in use\n"
"  Mounted on   the volume label, or the network/cloud provider\n");
}

static int parse_args(int argc, char **argv, Options *o)
{
    int i;
    memset(o, 0, sizeof(*o));
    o->human = 1;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--human-readable")) o->human = 1;
        else if (!strcmp(a, "-H") || !strcmp(a, "--si")) { o->si = 1; o->blocks = 0; }
        else if (!strcmp(a, "-k")) { o->blocks = 1; o->si = 0; }
        else if (!strcmp(a, "-T") || !strcmp(a, "--print-type")) o->show_type = 1;
        else if (!strcmp(a, "-a") || !strcmp(a, "--all")) o->all = 1;
        else if (!strcmp(a, "--no-color")) o->no_color = 1;
        else if (!strcmp(a, "--install")) o->install = 1;
        else if (!strcmp(a, "-i")) o->install = 1;
        else if (!strcmp(a, "--replace") || !strcmp(a, "-r")) o->replace = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-?")) { print_help(); return 1; }
        else if (a[0] == '-' && a[1] && a[1] != '-') {
            const char *p;
            for (p = a + 1; *p; ++p) {
                if (*p == 'h') o->human = 1;
                else if (*p == 'H') { o->si = 1; o->blocks = 0; }
                else if (*p == 'k') { o->blocks = 1; o->si = 0; }
                else if (*p == 'T') o->show_type = 1;
                else if (*p == 'a') o->all = 1;
                else if (*p == 'i') o->install = 1;
                else if (*p == 'r') o->replace = 1;
                else {
                    char msg[64];
                    _snprintf(msg, sizeof(msg), "df: invalid option -- '%c'\n", *p);
                    outa(msg);
                    outa("Try 'df --help' for more information.\n");
                    return -1;
                }
            }
        }
        else if (a[0] == '-') {
            char msg[256];
            _snprintf(msg, sizeof(msg), "df: unrecognized option '%s'\n", a);
            outa(msg);
            outa("Try 'df --help' for more information.\n");
            return -1;
        }
        else {
            char msg[256];
            _snprintf(msg, sizeof(msg), "df: '%s': path filters are not supported yet\n", a);
            outa(msg);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */

/* One format string for both the header and the rows, so the columns can
 * never drift apart. Volume is the drive letter; Type is the filesystem. */
#define DF_ROW_FMT L"  %-8s %-8s %9s %9s %9s %5s  %s\n"

static void print_columns(void)
{
    WCHAR row[MAX_STR * 2];
    _snwprintf(row, sizeof(row)/sizeof(WCHAR), DF_ROW_FMT,
               L"Volume", L"Type", L"Size", L"Used", L"Avail", L"Use%",
               L"Mounted on");
    outw(row);
}

static void print_date_line(void)
{
    WCHAR wbuf[128];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(wbuf, 128, L"\ndf for Windows  -  %04d-%02d-%02d %02d:%02d\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    outw(wbuf);
}

static void print_volume_row(const Volume *v, const Options *o)
{
    WCHAR sizeS[64], usedS[64], availS[64], mount[MAX_STR], volname[16];
    WCHAR row[MAX_STR * 2];
    ULONGLONG used;
    int pct = -1;
    const WCHAR *tag;
    const WCHAR *fst;

    format_size(v->size, o, sizeS, 64);
    used = v->size > v->free_bytes ? v->size - v->free_bytes : 0;
    format_size(used, o, usedS, 64);
    format_size(v->free_bytes, o, availS, 64);

    if (v->size > 0)
        pct = (int)(((double)used / (double)v->size) * 100.0 + 0.5);

    if (v->letter) _snwprintf(volname, 16, L"%c:", v->letter);
    else           wcscpy(volname, L"-");

    fst = v->fs[0] ? v->fs : L"-";

    wcsncpy(mount, v->label[0] ? v->label
                    : (v->provider[0] ? v->provider : L"-"), MAX_STR - 1);
    mount[MAX_STR - 1] = 0;

    tag = kind_tag(classify_volume(v));
    if (tag[0] && wcslen(mount) + wcslen(tag) + 1 < MAX_STR)
        wcscat(mount, tag);

    if (pct >= 0) {
        WCHAR pcts[8];
        _snwprintf(pcts, 8, L"%d", pct);
        _snwprintf(row, sizeof(row)/sizeof(WCHAR), DF_ROW_FMT,
                   volname, fst, sizeS, usedS, availS, pcts, mount);
    } else {
        _snwprintf(row, sizeof(row)/sizeof(WCHAR), DF_ROW_FMT,
                   volname, fst, sizeS, usedS, availS, L"-", mount);
    }
    outw(row);
}

static int report(const Options *o)
{
    PhysicalDisk disks[MAX_DISKS];
    Volume volumes[MAX_VOLUMES];
    int ndisks, nvols, i, d, rendered = 0;
    int *placed;

    ndisks = enumerate_physical_disks(disks, MAX_DISKS);
    nvols = enumerate_volumes(volumes, MAX_VOLUMES, o->all);

    print_date_line();

    placed = (int *)calloc(nvols ? (size_t)nvols : 1, sizeof(int));

    for (d = 0; d < ndisks; ++d) {
        int any = 0;
        WCHAR header[MAX_STR * 2];

        for (i = 0; i < nvols; ++i)
            if (volumes[i].letter && volumes[i].disk_number == disks[d].number)
                any = 1;
        if (!any) continue;

        _snwprintf(header, sizeof(header)/sizeof(WCHAR),
                   L"\nPhysical disk %d: %s\n", disks[d].number, disks[d].name);
        set_color(o, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        outw(header);
        reset_color(o);

        if (disks[d].serial[0]) {
            _snwprintf(header, sizeof(header)/sizeof(WCHAR),
                       L"  Serial: %s\n", disks[d].serial);
            outw(header);
        }

        print_columns();

        for (i = 0; i < nvols; ++i) {
            if (volumes[i].letter && volumes[i].disk_number == disks[d].number) {
                print_volume_row(&volumes[i], o);
                placed[i] = 1;
            }
        }
        rendered++;
    }

    {
        int any = 0;
        for (i = 0; i < nvols; ++i)
            if (!placed[i] && classify_volume(&volumes[i]) == 0)
                any = 1;
        if (any) {
            set_color(o, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            outw(L"\nOther local volumes:\n");
            reset_color(o);
            print_columns();
            for (i = 0; i < nvols; ++i)
                if (!placed[i] && classify_volume(&volumes[i]) == 0) {
                    print_volume_row(&volumes[i], o);
                    placed[i] = 1;
                }
            rendered++;
        }
    }

    {
        int any = 0;
        for (i = 0; i < nvols; ++i)
            if (!placed[i] && classify_volume(&volumes[i]) != 0) any = 1;
        if (any) {
            set_color(o, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            outw(L"\nNetwork and cloud mounts:\n");
            reset_color(o);
            print_columns();
            for (i = 0; i < nvols; ++i)
                if (!placed[i] && classify_volume(&volumes[i]) != 0) {
                    print_volume_row(&volumes[i], o);
                    placed[i] = 1;
                }
            rendered++;
        }
    }

    if (rendered == 0) {
        set_color(o, FOREGROUND_RED | FOREGROUND_INTENSITY);
        outw(L"\nNo disks or volumes found.\n");
        reset_color(o);
    }

    outw(L"\n");
    free(placed);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Installer                                                           */
/*                                                                     */
/* Windows cannot load two machines from one PE image, so the single    */
/* shipped file is a 32-bit image that carries the 64-bit build as an   */
/* RCDATA resource (id 101). The 64-bit code is only materialised at    */
/* install time, straight into System32. Nothing is written to a temp   */
/* folder, ever.                                                        */
/*                                                                     */
/* Which image goes where:                                             */
/*   System32  the native image for this machine (64-bit on a 64-bit   */
/*             OS)                                                      */
/*   SysWOW64  the 32-bit image, only on a 64-bit OS                   */
/* ------------------------------------------------------------------ */

#define DF_PAYLOAD_ID 101

#ifdef _WIN64
#define DF_SELF_IS_64 1
#else
#define DF_SELF_IS_64 0
#endif

static int df_is_admin(void)
{
    HANDLE tok;
    TOKEN_ELEVATION el;
    DWORD sz = 0;
    int elevated = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return 0;
    el.TokenIsElevated = 0;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz))
        elevated = el.TokenIsElevated ? 1 : 0;
    CloseHandle(tok);
    return elevated;
}

static int df_system_is_64(void)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
        return 1;
#ifdef PROCESSOR_ARCHITECTURE_ARM64
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        return 1;
#endif
#ifdef PROCESSOR_ARCHITECTURE_IA64
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64)
        return 1;
#endif
    return 0;
}

static int df_self_path(WCHAR *buf, DWORD n)
{
    DWORD len = GetModuleFileNameW(NULL, buf, n);
    return len > 0 && len < n;
}

static int df_payload(const unsigned char **data, DWORD *size)
{
    HRSRC r;
    HGLOBAL g;
    void *p;

    r = FindResourceW(NULL, MAKEINTRESOURCEW(DF_PAYLOAD_ID), (LPCWSTR)RT_RCDATA);
    if (!r)
        return 0;
    *size = SizeofResource(NULL, r);
    if (*size == 0)
        return 0;
    g = LoadResource(NULL, r);
    if (!g)
        return 0;
    p = LockResource(g);
    if (!p)
        return 0;
    *data = (const unsigned char *)p;
    return 1;
}

static int df_read_all(const WCHAR *path, unsigned char **out, DWORD *outn)
{
    HANDLE h;
    DWORD size, got, off;
    unsigned char *buf;

    *out = NULL;
    *outn = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(h);
        return 0;
    }
    buf = (unsigned char *)malloc(size ? size : 1);
    if (!buf) {
        CloseHandle(h);
        return 0;
    }
    off = 0;
    while (off < size) {
        if (!ReadFile(h, buf + off, size - off, &got, NULL) || got == 0)
            break;
        off += got;
    }
    CloseHandle(h);
    if (off != size) {
        free(buf);
        return 0;
    }
    *out = buf;
    *outn = size;
    return 1;
}

static int df_write_all(const WCHAR *path, const unsigned char *data, DWORD n)
{
    HANDLE h;
    DWORD off = 0, wrote;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    while (off < n) {
        if (!WriteFile(h, data + off, n - off, &wrote, NULL) || wrote == 0) {
            CloseHandle(h);
            return 0;
        }
        off += wrote;
    }
    CloseHandle(h);
    return 1;
}

static int df_file_exists(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int df_file_matches(const WCHAR *path, const unsigned char *data, DWORD n)
{
    unsigned char *have;
    DWORD hn;
    int same;

    if (!df_read_all(path, &have, &hn))
        return 0;
    same = (hn == n) && (n == 0 || memcmp(have, data, n) == 0);
    free(have);
    return same;
}

/* Rebuild the command line minus the program name, so an elevated child
 * repeats exactly the same request. */
static void df_forward_args(WCHAR *out, DWORD n)
{
    const WCHAR *cl = GetCommandLineW();
    const WCHAR *p = cl;
    DWORD i = 0;

    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p == L'"') p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    while (*p == L' ' || *p == L'\t') p++;
    while (*p && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
}

static int df_elevate(const WCHAR *self, const WCHAR *args)
{
    SHELLEXECUTEINFOW sei;
    DWORD code = 1;

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        if (GetLastError() == ERROR_CANCELLED)
            outw(L"df: elevation was cancelled; nothing was installed\n");
        else
            outw(L"df: could not request administrator rights\n");
        return 1;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    return (int)code;
}

static int df_install(const Options *o)
{
    WCHAR self[MAX_PATH], windir[MAX_PATH];
    WCHAR sys32dir[MAX_PATH], syswowdir[MAX_PATH];
    WCHAR t32[MAX_PATH], twow[MAX_PATH], tbat[MAX_PATH], tcmd[MAX_PATH];
    WCHAR msg[MAX_PATH * 3];
    unsigned char *self_data = NULL;
    DWORD self_n = 0;
    const unsigned char *payload = NULL;
    DWORD payload_n = 0;
    const unsigned char *img32, *img64;
    DWORD img32_n, img64_n;
    const unsigned char *sys32_data;
    DWORD sys32_n;
    int is64, have_payload, exists = 0, same = 0;
    PVOID redir = NULL;

    if (!df_self_path(self, MAX_PATH)) {
        outw(L"df: cannot determine the path of this program\n");
        return 1;
    }
    if (!df_read_all(self, &self_data, &self_n)) {
        outw(L"df: cannot read this program's own file\n");
        return 1;
    }
    have_payload = df_payload(&payload, &payload_n);
    is64 = df_system_is_64();

    if (!GetWindowsDirectoryW(windir, MAX_PATH)) {
        outw(L"df: cannot locate the Windows directory\n");
        free(self_data);
        return 1;
    }
    df_join_path(sys32dir,  MAX_PATH, windir, L"System32");
    df_join_path(syswowdir, MAX_PATH, windir, L"SysWOW64");
    df_join_path(t32,  MAX_PATH, sys32dir,  L"df.exe");
    df_join_path(twow, MAX_PATH, syswowdir, L"df.exe");
    df_join_path(tbat, MAX_PATH, sys32dir,  L"df.bat");
    df_join_path(tcmd, MAX_PATH, sys32dir,  L"df.cmd");

    /* Decide which bytes are the 32-bit and the 64-bit image.
     *   - A universal 32-bit build carries the 64-bit image as a payload,
     *     so its own bytes are the 32-bit image.
     *   - A plain 64-bit build is itself the 64-bit image; it has no
     *     32-bit image, and it is pointless on a 32-bit OS anyway. */
    if (DF_SELF_IS_64) {
        img64 = self_data;
        img64_n = self_n;
        if (have_payload) { img32 = payload; img32_n = payload_n; }
        else              { img32 = NULL;    img32_n = 0; }
    } else {
        img32 = self_data;
        img32_n = self_n;
        if (have_payload) { img64 = payload; img64_n = payload_n; }
        else              { img64 = NULL;    img64_n = 0; }
    }

    /* System32 gets the native image: 64-bit on a 64-bit OS, otherwise the
     * 32-bit one (which is what runs on a 32-bit OS). */
    if (is64 && img64) {
        sys32_data = img64;
        sys32_n = img64_n;
    } else if (img32) {
        sys32_data = img32;
        sys32_n = img32_n;
    } else {
        outw(L"df: this build cannot produce an image for this system\n");
        free(self_data);
        return 1;
    }

    /* Look for an existing df. Redirection must be off so a 32-bit process
     * sees the real System32, not SysWOW64. */
    Wow64DisableWow64FsRedirection(&redir);
    if (df_file_exists(t32))  { exists = 1; same = df_file_matches(t32, sys32_data, sys32_n); }
    if (is64 && df_file_exists(twow)) exists = 1;
    if (df_file_exists(tbat)) exists = 1;
    if (df_file_exists(tcmd)) exists = 1;
    Wow64RevertWow64FsRedirection(redir);

    if (exists && !o->replace) {
        outw(L"df: a df is already installed in the Windows directory:\n");
        if (df_file_exists(t32))  { _snwprintf(msg, MAX_PATH*3, L"    %s\n", t32);  outw(msg); }
        if (is64 && df_file_exists(twow)) { _snwprintf(msg, MAX_PATH*3, L"    %s\n", twow); outw(msg); }
        if (df_file_exists(tbat)) { _snwprintf(msg, MAX_PATH*3, L"    %s\n", tbat); outw(msg); }
        if (df_file_exists(tcmd)) { _snwprintf(msg, MAX_PATH*3, L"    %s\n", tcmd); outw(msg); }
        if (same)
            outw(L"df: the installed copy is the same version. Use -r to replace it anyway.\n");
        else
            outw(L"df: use -r (--replace) to overwrite it.\n");
        free(self_data);
        return 1;
    }

    if (!df_is_admin()) {
        WCHAR args[4096];
        df_forward_args(args, 4096);
        outw(L"df: administrator rights are required; requesting elevation...\n");
        free(self_data);
        return df_elevate(self, args);
    }

    Wow64DisableWow64FsRedirection(&redir);

    if (!df_write_all(t32, sys32_data, sys32_n)) {
        Wow64RevertWow64FsRedirection(redir);
        _snwprintf(msg, MAX_PATH*3, L"df: could not write %s\n", t32);
        outw(msg);
        free(self_data);
        return 1;
    }
    if (o->replace) {
        _snwprintf(msg, MAX_PATH*3, L"df: replaced %s\n", t32);
        outw(msg);
    } else {
        _snwprintf(msg, MAX_PATH*3, L"df: installed %s\n", t32);
        outw(msg);
    }

    if (is64) {
        if (!img32) {
            _snwprintf(msg, MAX_PATH*3,
                       L"df: note: no 32-bit image available; %s was not written\n",
                       twow);
            outw(msg);
        } else if (df_write_all(twow, img32, img32_n)) {
            if (o->replace) {
                _snwprintf(msg, MAX_PATH*3, L"df: replaced %s\n", twow);
                outw(msg);
            } else {
                _snwprintf(msg, MAX_PATH*3, L"df: installed %s\n", twow);
                outw(msg);
            }
        } else {
            _snwprintf(msg, MAX_PATH*3, L"df: could not write %s\n", twow);
            outw(msg);
        }
    }
    Wow64RevertWow64FsRedirection(redir);

    outw(L"df: done. Type 'df' from any folder to use it.\n");
    free(self_data);
    return 0;
}

int main(int argc, char **argv)
{
    Options o;
    int rc;

    rc = parse_args(argc, argv, &o);
    if (rc != 0)
        return rc > 0 ? 0 : 2;

    if (o.install)
        return df_install(&o);

    if (o.replace)
        outw(L"df: -r/--replace only applies with --install\n");

    return report(&o);
}
