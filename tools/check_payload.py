#!/usr/bin/env python3
"""Check that the 64-bit image embedded in the universal df.exe is byte-for-byte
identical to the standalone 64-bit build.

Usage: check_payload.py <universal.exe> <reference-64bit.exe> [resource-id]

Exits non-zero on mismatch. No third-party modules: it walks the PE resource
directory by hand so it runs on a bare CI runner.
"""

import struct
import sys

RT_RCDATA = 10


def read_sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    rsrc_rva, _ = struct.unpack_from("<II", d, ddir + 2 * 8)
    sec = opt + optsz
    secs = []
    for i in range(nsec):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, vsz, raw, rsz))
    return secs, rsrc_rva


def rva_to_off(secs, rva):
    for va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return raw + (rva - va)
    raise ValueError("RVA 0x%x is not mapped" % rva)


def extract_rcdata(d, res_id):
    secs, rsrc_rva = read_sections(d)
    base = rva_to_off(secs, rsrc_rva)

    def walk(off, depth, path):
        nnamed, nid = struct.unpack_from("<HH", d, off + 12)
        for i in range(nnamed + nid):
            nameid, offv = struct.unpack_from("<II", d, off + 16 + i * 8)
            name = nameid if not (nameid & 0x80000000) else None
            child = base + (offv & 0x7FFFFFFF)
            if depth < 2:
                yield from walk(child, depth + 1, path + [name])
            else:
                drva, dsz, _, _ = struct.unpack_from("<IIII", d, child)
                if path[0] == RT_RCDATA and path[1] == res_id:
                    doff = rva_to_off(secs, drva)
                    yield d[doff:doff + dsz]

    yield from walk(base, 0, [])


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    uni_path, ref_path = argv[1], argv[2]
    res_id = int(argv[3]) if len(argv) > 3 else 101

    uni = open(uni_path, "rb").read()
    ref = open(ref_path, "rb").read()

    try:
        payload = next(extract_rcdata(uni, res_id))
    except (StopIteration, ValueError) as e:
        print("FAIL: no RCDATA resource %d in %s (%s)" % (res_id, uni_path, e))
        return 1

    if payload != ref:
        print("FAIL: payload (%d bytes) != %s (%d bytes)"
              % (len(payload), ref_path, len(ref)))
        return 1

    print("OK: payload in %s is identical to %s (%d bytes)"
          % (uni_path, ref_path, len(payload)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
