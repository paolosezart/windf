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
df for Windows  -  2026-01-15 09:41

Physical disk 0: Example SSD 512GB
  Serial: EXAMP0123456789
  Volume   Type          Size      Used     Avail  Use%  Mounted on
  C:       NTFS        475.0G    241.3G    233.7G    51  Windows
  D:       NTFS        200.0G     88.4G    111.6G    44  Data

Physical disk 1: Sample NVMe 1TB
  Serial: SMPL1234567890AB
  Volume   Type          Size      Used     Avail  Use%  Mounted on
  E:       NTFS        931.5G    402.1G    529.4G    43  Projects

Other local volumes:
  Volume   Type          Size      Used     Avail  Use%  Mounted on
  M:       FAT32      1024.0M    232.1M    791.9M    23  RAM

Network and cloud mounts:
  Volume   Type          Size      Used     Avail  Use%  Mounted on
  O:       -            54.8G     32.9G     21.9G    60  - [NET]
  T:       -             1.0T    314.0G    710.0G    31  - [NET]
  W:       -            50.0G     36.3G     13.7G    73  MEGA [CLOUD]
```

## Two forms, same output

| Form | File | Needs | Runs on |
| --- | --- | --- | --- |
| Universal binary | `dist/df.exe` | nothing | 32-bit and 64-bit Windows |
| Native binaries | `dist/df-x64.exe`, `dist/df-x86.exe` | nothing | Windows Vista and later |
| Single script | `df.bat` | built-in PowerShell | Windows Vista and later |

`df.exe` is a single file that runs on both 32-bit and 64-bit Windows. It is
a 32-bit image that carries the 64-bit build inside it, so `--install` can
place the right code in each system directory (see below). The dedicated
`df-x64.exe` and `df-x86.exe` are the same program without the payload, if
you prefer one file per architecture.

All of them are self-contained. No .NET installation, no Visual C++ runtime,
no external DLLs. The native binaries link only against `KERNEL32`, `MPR`
and `msvcrt`, which are part of every Windows install. The batch file
carries an embedded PowerShell 2.0 script and uses only `Get-WmiObject`, so
it works on a freshly installed Vista or 7.

## Install

`df.exe` can copy itself into the Windows directory so that plain `df` works
from any folder:

```
df.exe --install        copy df into System32 (and SysWOW64 on 64-bit)
df.exe -i               same thing, short form
df.exe --install -r     overwrite an existing df
df.exe -i -r            same, short forms combined as -ri
```

What gets written, and where:

| System | `System32` | `SysWOW64` |
| --- | --- | --- |
| 64-bit Windows | 64-bit build | 32-bit build |
| 32-bit Windows | 32-bit build | (not used) |

The 64-bit code is taken from a resource embedded in `df.exe` and written
straight into `System32` — nothing is ever dropped into a temporary folder.
If the process is not elevated it asks for administrator rights once
(a UAC prompt) and repeats the same request elevated.

If a `df.exe`, `df.bat` or `df.cmd` already exists in the Windows directory,
the command stops and lists what it found:

```
df: a df is already installed in the Windows directory:
    C:\Windows\System32\df.exe
df: the installed copy is the same version. Use -r to replace it anyway.
```

Add `-r` (`--replace`) to overwrite. The installer never deletes a file it
did not just write; it only overwrites the `df.*` names above.

Use the binary if you can. Use `df.bat` if you want a single file with no
`exe` (some locked-down machines block unsigned executables). `df.bat`
cannot install itself; running `df.bat --install` prints a pointer to
`df.exe`.

## Options

```
-h, --human-readable   print sizes in powers of 1024 (default)
-H, --si               print sizes in powers of 1000
-k                     print sizes in 1024-byte blocks
-T, --print-type       accepted for df compatibility (Type is always shown)
-a, --all              include volumes without a drive letter
    --no-color         do not colorize output
-i, --install          copy df into the Windows system directories
-r, --replace          with --install, overwrite an existing df
    --help             display this help and exit
```

Short flags combine: `-hT` is the same as `-h -T`.

The `Type` column is always printed, so `-T` is a no-op kept for
compatibility with the Unix `df -T`. `-a` adds volumes that have no drive
letter (recovery partitions, RAM disks); without it they are hidden, because
they usually report `0B` and add noise.

## Columns

| Column | Meaning |
| --- | --- |
| Volume | the drive letter, or `-` for a letterless volume |
| Type | the filesystem: `NTFS`, `FAT32`, `exFAT`, ... (`-` when unknown, e.g. a network or cloud mount) |
| Size / Used / Avail | capacity in human units (or 1K blocks with `-k`) |
| Use% | percentage of the volume in use |
| Mounted on | the volume label, or the network/cloud provider, plus a `[NET]` / `[CLOUD]` tag |

Each physical disk also prints its model and, when the drive reports it, its
serial number on the line below.

## Colors

Output is colorized when stdout is a real console, and stays plain when you
pipe or redirect it. Only the foreground color is changed: the console
background (the classic blue in `cmd.exe` and Windows PowerShell, or whatever
you picked in Properties) is left alone and restored exactly when `df` exits.

```
df --no-color      turn coloring off
```

## Download

Grab the files from the [latest release](../../releases/latest):

| File | Architecture | Notes |
| --- | --- | --- |
| [`df.exe`](../../releases/latest/download/df.exe) | 32-bit and 64-bit | one file, can `--install` |
| [`df-x64.exe`](../../releases/latest/download/df-x64.exe) | 64-bit (x86-64) | native only |
| [`df-x86.exe`](../../releases/latest/download/df-x86.exe) | 32-bit (i386) | native only |
| [`df.bat`](../../releases/latest/download/df.bat) | any | single file, no `exe` |
| [`SHA256SUMS.txt`](../../releases/latest/download/SHA256SUMS.txt) | — | checksums |

Verify a download with:

```
sha256sum -c SHA256SUMS.txt      # Linux / Git Bash
certutil -hashfile df.exe SHA256 # Windows
```

## Install

### One command (recommended)

Download `df.exe` and run it with administrator rights:

```
df.exe --install
```

It copies the 64-bit build to `System32` and the 32-bit build to `SysWOW64`
(on 32-bit Windows, just `System32`), so plain `df` works everywhere. See
[Install](#install) above for the overwrite rules. This is the only form
that installs itself.

### Manual: native binary

1. Download `df.exe` (both architectures), `df-x64.exe` (64-bit) or
   `df-x86.exe` (32-bit).
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
make            # builds dist/df.exe, dist/df-x64.exe and dist/df-x86.exe
make test       # runs the option/format self-tests
```

`make` builds the standalone 64-bit and 32-bit binaries, then embeds the
64-bit one into the 32-bit one as resource 101 to produce the universal
`df.exe`. `tools/check_payload.py` verifies that the embedded copy matches
the standalone build; CI runs it on every push.

Or directly:

```
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o dist/df-x64.exe src/df.c -static -s -lmpr
i686-w64-mingw32-gcc   -O2 -Wall -Wextra -o dist/df-x86.exe src/df.c -static -s -lmpr
```

`.github/workflows/build.yml` builds all three binaries on every push and
attaches them to a release when a tag is pushed.

## How it works

The binary (`src/df.c`) uses only stable Win32 APIs:

- `GetLogicalDrives`, `GetDriveType`, `GetVolumeInformation` — enumerate
  drive letters and labels.
- `GetDiskFreeSpaceEx` — real free/total bytes per volume.
- `CreateFile("\\.\PhysicalDriveN")` plus `IOCTL_DISK_GET_LENGTH_INFO` and
  `IOCTL_STORAGE_QUERY_PROPERTY` — physical disk list, size, bus, media type
  and serial number.
- `CreateFile("\\.\X:")` plus `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` — map a
  volume to its physical disk number.
- `FindFirstVolumeW`, `GetVolumePathNamesForVolumeNameW` — volumes without a
  drive letter (recovery partitions); shown only with `-a`.

The installer uses `GetModuleFileNameW` (self path), `GetWindowsDirectoryW`
(`System32`/`SysWOW64`), `FindResourceW`/`LoadResource` (the embedded
64-bit image), `ShellExecuteExW` with the `runas` verb (UAC elevation),
`GetTokenInformation` (`TokenElevation`), and
`Wow64DisableWow64FsRedirection` so a 32-bit process writes the real
`System32` rather than being redirected to `SysWOW64`.

The batch file reaches the same result through WMI: `Win32_DiskDrive`,
`Win32_DiskDriveToDiskPartition`, `Win32_LogicalDiskToPartition`,
`Win32_LogicalDisk` and (with `-a`) `Win32_Volume`.

Volumes are classified as `LOCAL`, `NET` or `CLOUD`. `NET` means a mapped
network drive. `CLOUD` means the label or provider matches a known cloud
client (Mega, rclone, OneDrive, Dropbox, Google Drive, Yandex, WebDAV,
Nextcloud, iCloud, pCloud, Box).

## Windows compatibility

| Version | Status |
| --- | --- |
| Windows 11 / 10 | tested |
| Windows 8 / 8.1 | expected to work (same APIs) |
| Windows 7 | expected to work (same APIs) |
| Windows Vista | expected to work (same APIs) |

The binary targets `_WIN32_WINNT 0x0600` (Vista) and uses only APIs that
exist on Vista and later: `GetLogicalDrives`, `GetDriveType`,
`GetVolumeInformation`, `GetDiskFreeSpaceEx`, `WNetGetConnection`,
`FindFirstVolumeW` / `GetVolumePathNamesForVolumeNameW`, and
`IOCTL_STORAGE_QUERY_PROPERTY` (Vista added the storage property query).
Serial numbers come from the same query, so on drives that do not report one
the `Serial:` line is simply omitted. It is a 32/64-bit static binary with no
runtime to install.

The batch form relies on Windows PowerShell 2.0, which ships with Windows 7.
On Vista it needs PowerShell 2.0 installed (it is available for Vista).

## Limitations

- Path filters (`df C:\`) are accepted but only produce a warning; the full
  report is shown.
- Cloud detection is name-based. A cloud mount named something else falls
  into the `NET` block without the `[CLOUD]` tag. Cloud and network mounts
  report `-` for `Type`, because the filesystem is decided by the remote
  server, not the local machine.
- Physical disk sizes are the vendor-stated sizes, so they can differ
  slightly from the sum of the volumes.
- Serial numbers are only shown when the drive firmware reports them (common
  for SATA/NVMe SSDs, less so for USB sticks).
- `--install` needs administrator rights. It writes only `df.exe` (and
  detects an existing `df.bat` / `df.cmd`) in the Windows directory; it does
  not touch the registry or `PATH`.
- The universal `df.exe` is a 32-bit image. On 64-bit Windows it runs under
  WOW64 when you launch it directly, which is fine for a read-only report;
  the copy installed into `System32` is the native 64-bit build.

## Documentation

The `docs/` folder holds two Russian-language guides written for a low-end
Windows machine:

- `docs/local-llm-guide.txt` — installing and tuning local LLMs with Ollama.
- `docs/cloud-models-guide.txt` — free cloud LLM tiers and building release
  binaries with GitHub Actions.

## License

MIT. See [LICENSE](LICENSE).
