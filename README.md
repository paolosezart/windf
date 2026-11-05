# windf — `df` for Windows

A human-readable disk usage report for Windows, in the style of the Unix
`df` command. It groups output by **physical disk** and lists every logical
volume (drive letter) underneath its disk. Network and cloud mounts
(rclone, Mega, OneDrive, WebDAV, mapped network drives, ...) are shown in a
separate block and tagged `[NET]` / `[CLOUD]`.

This exists because the common `df` ports for Windows report `Avail 0` and
`Use% 100` on NTFS volumes, and print raw device paths
(`\Device\HarddiskVolumeN`) instead of drive letters. windf reads the real
numbers through the Win32 API.

```
df for Windows  -  2026-11-05 01:50

Physical disk 0: Lexar 240GB SSD  (223.6G, SSD, SATA)
  Filesystem   Type        Size      Used     Avail  Use%  Mounted on
  C:           NTFS      295.5G    153.7G    141.8G    52  Windows 10

Physical disk 1: WALRAM 128G  (119.2G, SSD, SATA)
  Filesystem   Type        Size      Used     Avail  Use%  Mounted on
  G:           NTFS      223.6G    151.8G     71.8G    68  SSD OLD w10

Physical disk 2: CT1000BX500SSD1  (931.5G, SSD, SATA)
  Filesystem   Type        Size      Used     Avail  Use%  Mounted on
  D:           NTFS      636.1G    519.5G    116.6G    82  SSD 1TB
  E:           NTFS      119.1G     51.5G     67.6G    43  SSD w7

Network and cloud mounts:
  Filesystem   Type        Size      Used     Avail  Use%  Mounted on
  M:           -          50.0G     40.0G     10.0G    80  Mega [CLOUD]
  R:           -             0B        0B        0B     -  rclone-ssh [CLOUD]
```

## Two forms, same output

| Form | File | Needs | Runs on |
| --- | --- | --- | --- |
| Native binary | `dist/df-x64.exe`, `dist/df-x86.exe` | nothing | Windows Vista and later |
| Single script | `df.bat` | built-in PowerShell | Windows Vista and later |

Both are self-contained. No .NET installation, no Visual C++ runtime, no
external DLLs. The native binaries link only against `KERNEL32`, `MPR` and
`msvcrt`, which are part of every Windows install. The batch file carries
an embedded PowerShell 2.0 script and uses only `Get-WmiObject`, so it works
on a freshly installed Vista or 7.

Use the binary if you can. Use `df.bat` if you want a single file with no
`exe` (some locked-down machines block unsigned executables).

## Options

```
-h, --human-readable   print sizes in powers of 1024 (default)
-H, --si               print sizes in powers of 1000
-k                     print sizes in 1024-byte blocks
-T, --print-type       print the filesystem type column
-a, --all              include volumes without a drive letter
    --no-color         do not colorize output
    --help             display this help and exit
```

Short flags combine: `-hT` is the same as `-h -T`.

## Download

Grab the files from the [latest release](../../releases/latest):

| File | Architecture | Notes |
| --- | --- | --- |
| [`df-x64.exe`](../../releases/latest/download/df-x64.exe) | 64-bit (x86-64) | most common |
| [`df-x86.exe`](../../releases/latest/download/df-x86.exe) | 32-bit (i386) | 32-bit Windows |
| [`df.bat`](../../releases/latest/download/df.bat) | any | single file, no `exe` |
| [`SHA256SUMS.txt`](../../releases/latest/download/SHA256SUMS.txt) | — | checksums |

Verify a download with:

```
sha256sum -c SHA256SUMS.txt      # Linux / Git Bash
certutil -hashfile df-x64.exe SHA256   # Windows
```

## Install

### Native binary

1. Download `df-x64.exe` (64-bit Windows) or `df-x86.exe` (32-bit Windows).
2. Rename to `df.exe`, put it in a folder on your `PATH` (for example
   `C:\tools`), and run `df` from any prompt.

```
mkdir C:\tools
copy df-x64.exe C:\tools\df.exe
setx PATH "%PATH%;C:\tools"
```

Open a new terminal and type `df -T`.

### Batch file

Put `df.bat` anywhere and call it:

```
df.bat -T
```

To call it as plain `df`, add its folder to `PATH` as above.

## Build from source

Only a MinGW-w64 toolchain is needed.

```
make            # builds dist/df-x64.exe and dist/df-x86.exe
make test       # runs the option/format self-tests
```

Or directly:

```
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o dist/df-x64.exe src/df.c -static -s -lmpr
i686-w64-mingw32-gcc   -O2 -Wall -Wextra -o dist/df-x86.exe src/df.c -static -s -lmpr
```

`.github/workflows/build.yml` builds both binaries on every push and
attaches them to a release when a tag is pushed.

## How it works

The binary (`src/df.c`) uses only stable Win32 APIs:

- `GetLogicalDrives`, `GetDriveType`, `GetVolumeInformation` — enumerate
  drive letters and labels.
- `GetDiskFreeSpaceEx` — real free/total bytes per volume.
- `CreateFile("\\.\PhysicalDriveN")` plus `IOCTL_DISK_GET_LENGTH_INFO` and
  `IOCTL_STORAGE_QUERY_PROPERTY` — physical disk list, size, bus and media
  type.
- `CreateFile("\\.\X:")` plus `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` — map a
  volume to its physical disk number.
- `FindFirstVolumeW`, `GetVolumePathNamesForVolumeNameW` — volumes without a
  drive letter (recovery partitions).

The batch file reaches the same result through WMI: `Win32_DiskDrive`,
`Win32_DiskDriveToDiskPartition`, `Win32_LogicalDiskToPartition` and
`Win32_LogicalDisk`.

Volumes are classified as `LOCAL`, `NET` or `CLOUD`. `NET` means a mapped
network drive. `CLOUD` means the label or provider matches a known cloud
client (Mega, rclone, OneDrive, Dropbox, Google Drive, Yandex, WebDAV,
Nextcloud, iCloud, pCloud, Box).

## Limitations

- Path filters (`df C:\`) are accepted but only produce a warning; the full
  report is shown.
- Cloud detection is name-based. A cloud mount named something else falls
  into the `NET` block without the `[CLOUD]` tag.
- Physical disk sizes are the vendor-stated sizes, so they can differ
  slightly from the sum of the volumes.

## Documentation

The `docs/` folder holds two Russian-language guides written for a low-end
Windows machine:

- `docs/local-llm-guide.txt` — installing and tuning local LLMs with Ollama.
- `docs/cloud-models-guide.txt` — free cloud LLM tiers and building release
  binaries with GitHub Actions.

## License

MIT. See [LICENSE](LICENSE).
