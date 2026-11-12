#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the HakiOS authors (Scion).
"""mkmetallib.py OUT NAME:TYPE:FILE.bc ... : a .metallib from AIR bitcode, without Xcode.

Xcode's `metallib` is the only maker of Metal libraries, and it runs on a Mac. The Linux build host has
`llvm-as`, which turns hand-written AIR (tools/metal/air/*.ll) into bitcode; this puts that bitcode in the
MTLB container (version 1.2, as iOS 16.5's own libraries; the layout is tools/merge_metallib.py's), so that
`-newLibraryWithData:` / `-newLibraryWithURL:` take it like any other library.

TYPE is vertex, fragment or kernel. Each function is its own module, as Apple's libraries have them. Per
function: NAME, TYPE, HASH (the SHA-256 of its bitcode, which metal2vulkan checks), MDSZ, OFFT, VERS (AIR
2.5, Metal 3.0, as the .ll files declare), and empty public and private metadata records; the header's
extension is an empty reflection list (RLST) and a UUID made from the contents. Nothing Apple made is in
the output.
"""
import hashlib
import struct
import sys

TYPES = {'vertex': 0, 'fragment': 1, 'kernel': 2}


def tag(name, value):
    return name + struct.pack('<H', len(value)) + value


def build(functions):
    entries, pub, prv, bc, refl = [], b'', b'', b'', b''
    for name, kind, code in functions:
        empty = struct.pack('<I', 8) + b'ENDT'          # a record: its size, counting itself, and no tags
        body = (tag(b'NAME', name.encode() + b'\0') + tag(b'TYPE', bytes([TYPES[kind]]))
                + tag(b'HASH', hashlib.sha256(code).digest()) + tag(b'MDSZ', struct.pack('<Q', len(code)))
                + tag(b'OFFT', struct.pack('<QQQ', len(pub), len(prv), len(bc)))
                + tag(b'VERS', struct.pack('<HHHH', 2, 5, 3, 0)) + b'ENDT')
        entries.append(struct.pack('<I', 4 + len(body)) + body)
        pub += empty
        prv += empty
        bc += code
    funcs_off = 0x58
    listing = struct.pack('<I', len(functions)) + b''.join(entries)
    reflection = struct.pack('<I', 0) + refl
    uuid = hashlib.sha256(b''.join(code for _, _, code in functions)).digest()[:16]
    ext_len = (6 + 16) + (6 + 16) + 4
    pub_off = funcs_off + len(listing) + ext_len
    prv_off = pub_off + len(pub)
    bc_off = prv_off + len(prv)
    refl_off = bc_off + len(bc)
    total = refl_off + len(reflection)
    ext = tag(b'RLST', struct.pack('<QQ', refl_off, len(reflection))) + tag(b'UUID', uuid) + b'ENDT'
    # MTLB, platform word (64-bit), container 1.2, an executable library, iOS 16.0
    head = b'MTLB' + struct.pack('<HHHBBHH', 0x8001, 1, 2, 0, 2, 16, 0)
    head += struct.pack('<QQQQQQQQQ', total, funcs_off, sum(map(len, entries)), pub_off, len(pub),
                        prv_off, len(prv), bc_off, len(bc))
    data = head + listing + ext + pub + prv + bc + reflection
    assert len(head) == 0x58 and len(data) == total
    return data


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    functions = []
    for spec in argv[2:]:
        name, kind, path = spec.split(':', 2)
        if kind not in TYPES:
            sys.exit('mkmetallib.py: %s: the type is vertex, fragment or kernel' % spec)
        code = open(path, 'rb').read()
        if code[:4] not in (b'BC\xc0\xde', b'\xde\xc0\x17\x0b'):
            sys.exit('mkmetallib.py: %s is not LLVM bitcode' % path)
        functions.append((name, kind, code))
    data = build(functions)
    open(argv[1], 'wb').write(data)
    print('mkmetallib.py: %s, %d bytes, %s' % (argv[1], len(data), ' '.join(f[0] for f in functions)))


if __name__ == '__main__':
    main(sys.argv)
