#!/usr/bin/env python3
"""machosign.py: ad-hoc sign a thin arm64 Mach-O with entitlements, where ldid is not installed.

    machosign.py BINARY [ENTITLEMENTS.plist] [IDENTIFIER]

Writes what ldid -S does: a SuperBlob of a SHA-256 CodeDirectory (version 0x20400, ad hoc, the main binary's
exec segment), an empty requirement set, the entitlements as XML (slot 5) and as DER (slot 7, which iOS 15+
reads). Any signature the linker left is replaced. Only thin 64-bit files; enough for our own programs.
"""
import hashlib, plistlib, struct, sys

LC_SEGMENT_64, LC_CODE_SIGNATURE = 0x19, 0x1d
PAGE = 4096


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def der(tag, content):
    return bytes([tag]) + der_len(len(content)) + content


def der_value(v):
    if isinstance(v, bool):
        return der(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        return der(0x02, v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big', signed=True))
    if isinstance(v, str):
        return der(0x0c, v.encode())
    if isinstance(v, (list, tuple)):
        return der(0x30, b''.join(der_value(x) for x in v))
    if isinstance(v, dict):
        return der(0xb0, b''.join(der(0x30, der(0x0c, k.encode()) + der_value(v[k])) for k in sorted(v)))
    raise TypeError(f'entitlement value {v!r}')


def blob(magic, payload):
    return struct.pack('>II', magic, 8 + len(payload)) + payload


def sign(path, ents=None, ident=None):
    data = bytearray(open(path, 'rb').read())
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
    assert magic == 0xfeedfacf, 'thin 64-bit Mach-O only'
    off, text, linkedit, sigcmd, first_sect = 32, None, None, None, len(data)
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b'\0').decode()
            fileoff, filesize = struct.unpack_from('<QQ', data, off + 40)
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            for s in range(nsects):
                so = struct.unpack_from('<I', data, off + 72 + s * 80 + 48)[0]
                if so:
                    first_sect = min(first_sect, so)
            if name == '__TEXT':
                text = (fileoff, filesize)
            if name == '__LINKEDIT':
                linkedit = off
        elif cmd == LC_CODE_SIGNATURE:
            sigcmd = off
        off += size
    assert linkedit is not None and text is not None
    if sigcmd is None:  # room for a new load command in the header's padding
        assert off + 16 <= first_sect, 'no room in the header for LC_CODE_SIGNATURE'
        sigcmd = off
        struct.pack_into('<II', data, sigcmd, LC_CODE_SIGNATURE, 16)
        struct.pack_into('<II', data, 16, ncmds + 1, sizeofcmds + 16)
        le_off, le_size = struct.unpack_from('<QQ', data, linkedit + 40)
        dataoff = (le_off + le_size + 15) & ~15
    else:
        dataoff = struct.unpack_from('<I', data, sigcmd + 8)[0]
    del data[dataoff:]
    data += b'\0' * (dataoff - len(data))

    ident = (ident or path.rsplit('/', 1)[-1]).encode() + b'\0'
    ents = ents or {}
    special = {2: blob(0xfade0c01, struct.pack('>I', 0))}
    if ents:
        special[5] = blob(0xfade7171, plistlib.dumps(ents, fmt=plistlib.FMT_XML))
        special[7] = blob(0xfade7172, der(0x70, der(0x02, b'\x01') + der_value(ents)))
    nspecial = max(special)
    ncode = (dataoff + PAGE - 1) // PAGE
    hdr = 88
    hash_off = hdr + len(ident) + nspecial * 32
    cd_len = hash_off + ncode * 32
    sb_count = 1 + len(special)
    sb_len = 12 + 8 * sb_count + cd_len + sum(len(b) for b in special.values())
    sig_size = (sb_len + 15) & ~15
    # the header as it will be, before the pages are hashed
    struct.pack_into('<II', data, sigcmd + 8, dataoff, sig_size)
    le_off = struct.unpack_from('<Q', data, linkedit + 40)[0]
    le_vm = struct.unpack_from('<Q', data, linkedit + 32)[0]
    le_file = dataoff + sig_size - le_off
    struct.pack_into('<Q', data, linkedit + 48, le_file)
    if le_vm < le_file:
        struct.pack_into('<Q', data, linkedit + 32, (le_file + 0x3fff) & ~0x3fff)

    cd = struct.pack('>IIIIIIIIIBBBBIIIIQQQQ', 0xfade0c02, cd_len, 0x20400, 0x2, hash_off, hdr, nspecial, ncode,
                     dataoff, 32, 2, 0, 12, 0, 0, 0, 0, 0, text[0], text[1], 1)
    cd += ident
    for slot in range(nspecial, 0, -1):
        cd += hashlib.sha256(special[slot]).digest() if slot in special else b'\0' * 32
    for p in range(ncode):
        cd += hashlib.sha256(bytes(data[p * PAGE:min((p + 1) * PAGE, dataoff)])).digest()
    assert len(cd) == cd_len
    entries, body = [(0, cd)] + sorted(special.items()), b''
    index, pos = b'', 12 + 8 * sb_count
    for typ, b in entries:
        index += struct.pack('>II', typ, pos)
        body += b
        pos += len(b)
    sb = struct.pack('>III', 0xfade0cc0, sb_len, sb_count) + index + body
    data += sb + b'\0' * (sig_size - len(sb))
    open(path, 'wb').write(data)


if __name__ == '__main__':
    ents = plistlib.load(open(sys.argv[2], 'rb')) if len(sys.argv) > 2 else None
    sign(sys.argv[1], ents, sys.argv[3] if len(sys.argv) > 3 else None)
