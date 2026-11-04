/*
 * windf - df for Windows
 *
 * A native, human-readable disk usage report. It groups output by PHYSICAL
 * disk and lists every logical volume (drive letter) underneath its disk,
 * in the style of coreutils df:
 *
 *     Filesystem  Type  Size  Used  Avail  Use%  Mounted on
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
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), attr);
}

static void reset_color(const Options *o)
{
    if (o->no_color || !stdout_is_console()) return;
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
                            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
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

            {
                size_t L;
                L = wcslen(vendor);  while (L && vendor[L-1]  == L' ') vendor[--L]  = 0;
                L = wcslen(product); while (L && product[L-1] == L' ') product[--L] = 0;
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

static int enumerate_volumes(Volume *vols, int max)
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

    /* Letterless volumes (recovery partitions etc.). */
    {
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
"  -T, --print-type       print the filesystem type column\n"
"  -a, --all              include volumes without a drive letter\n"
"      --no-color         do not colorize output\n"
"      --help             display this help and exit\n"
"\n"
"Output blocks:\n"
"  Each physical disk is printed as a block, with all of its logical\n"
"  volumes (drive letters) listed underneath it. Network and cloud\n"
"  mounts are printed in a separate block and marked [NET] or [CLOUD].\n");
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
        else if (!strcmp(a, "--help") || !strcmp(a, "-?")) { print_help(); return 1; }
        else if (a[0] == '-' && a[1] && a[1] != '-') {
            const char *p;
            for (p = a + 1; *p; ++p) {
                if (*p == 'h') o->human = 1;
                else if (*p == 'H') { o->si = 1; o->blocks = 0; }
                else if (*p == 'k') { o->blocks = 1; o->si = 0; }
                else if (*p == 'T') o->show_type = 1;
                else if (*p == 'a') o->all = 1;
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

static void print_columns(const Options *o)
{
    if (o->show_type)
        outw(L"  Filesystem   Type        Size      Used     Avail  Use%  Mounted on\n");
    else
        outw(L"  Filesystem        Size      Used     Avail  Use%  Mounted on\n");
}

static void print_date_line(const Options *o)
{
    WCHAR wbuf[128];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(wbuf, 128, L"\ndf for Windows  -  %04d-%02d-%02d %02d:%02d\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    set_color(o, FOREGROUND_INTENSITY);
    outw(wbuf);
    reset_color(o);
}

static void print_volume_row(const Volume *v, const Options *o)
{
    WCHAR sizeS[64], usedS[64], availS[64], mount[MAX_STR], fsname[16];
    WCHAR row[MAX_STR * 2];
    ULONGLONG used;
    int pct = -1;
    const WCHAR *tag;

    format_size(v->size, o, sizeS, 64);
    used = v->size > v->free_bytes ? v->size - v->free_bytes : 0;
    format_size(used, o, usedS, 64);
    format_size(v->free_bytes, o, availS, 64);

    if (v->size > 0)
        pct = (int)(((double)used / (double)v->size) * 100.0 + 0.5);

    if (v->letter) _snwprintf(fsname, 16, L"%c:", v->letter);
    else           wcscpy(fsname, L"(none)");

    wcsncpy(mount, v->label[0] ? v->label
                    : (v->provider[0] ? v->provider : L"-"), MAX_STR - 1);
    mount[MAX_STR - 1] = 0;

    tag = kind_tag(classify_volume(v));
    if (tag[0] && wcslen(mount) + wcslen(tag) + 1 < MAX_STR)
        wcscat(mount, tag);

    if (o->show_type) {
        const WCHAR *fst = v->fs[0] ? v->fs : L"-";
        if (pct >= 0)
            _snwprintf(row, sizeof(row)/sizeof(WCHAR),
                       L"  %-12s %-6s %9s %9s %9s %5d  %s\n",
                       fsname, fst, sizeS, usedS, availS, pct, mount);
        else
            _snwprintf(row, sizeof(row)/sizeof(WCHAR),
                       L"  %-12s %-6s %9s %9s %9s %5s  %s\n",
                       fsname, fst, sizeS, usedS, availS, L"-", mount);
    } else {
        if (pct >= 0)
            _snwprintf(row, sizeof(row)/sizeof(WCHAR),
                       L"  %-12s %9s %9s %9s %5d  %s\n",
                       fsname, sizeS, usedS, availS, pct, mount);
        else
            _snwprintf(row, sizeof(row)/sizeof(WCHAR),
                       L"  %-12s %9s %9s %9s %5s  %s\n",
                       fsname, sizeS, usedS, availS, L"-", mount);
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
    nvols = enumerate_volumes(volumes, MAX_VOLUMES);

    print_date_line(o);

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
        print_columns(o);

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
            print_columns(o);
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
            print_columns(o);
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

int main(int argc, char **argv)
{
    Options o;
    int rc;

    rc = parse_args(argc, argv, &o);
    if (rc != 0)
        return rc > 0 ? 0 : 2;

    return report(&o);
}
