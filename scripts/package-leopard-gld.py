#!/usr/bin/env python3
"""Package and verify the four PPC32/PPC64 Leopard GL renderer modules."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

MODULES = ["RadeonNIGLDriver" + suffix for suffix in ("", "1", "2", "3")]
CPUS = (18, 0x01000012)
EXPORTS = {"_RDNGLBeginVideoFrameV1", "_RDNGLPollVideoFrameV1",
           "_RDNGLCancelVideoFrameV1", "_RDNGLMakeCurrent"}


def thin(data, expected):
    magic = b"\xfe\xed\xfa\xce" if expected == 18 else b"\xfe\xed\xfa\xcf"
    if len(data) < 32 or data[:4] != magic:
        raise ValueError("expected a thin big-endian PowerPC Mach-O slice")
    cpu, subtype, kind, count, command_bytes = struct.unpack_from(">IIIII", data, 4)
    if cpu != expected or kind not in (6, 8):
        raise ValueError("wrong architecture or Mach-O file type")
    header = 28 if cpu == 18 else 32
    end = header + command_bytes
    if end > len(data):
        raise ValueError("truncated Mach-O commands")
    offset, symbols = header, set()
    for _ in range(count):
        if offset + 8 > end:
            raise ValueError("truncated load command")
        command, size = struct.unpack_from(">II", data, offset)
        if size < 8 or offset + size > end:
            raise ValueError("invalid load command size")
        if command == 2:
            if size < 24:
                raise ValueError("truncated symbol-table command")
            symoff, nsyms, stroff, strsize = struct.unpack_from(">IIII", data, offset + 8)
            entry = 12 if cpu == 18 else 16
            if symoff + nsyms * entry > len(data) or stroff + strsize > len(data):
                raise ValueError("truncated symbol table")
            strings = data[stroff:stroff + strsize]
            for i in range(nsyms):
                pos = symoff + i * entry
                index, flags = struct.unpack_from(">IB", data, pos)
                if not flags & 1 or flags & 0xe0 or not flags & 0x0e:
                    continue
                if index >= len(strings) or strings.find(b"\0", index) < 0:
                    raise ValueError("invalid symbol name")
                symbols.add(strings[index:strings.index(b"\0", index)].decode("ascii"))
        offset += size
    if offset != end or not EXPORTS <= symbols:
        raise ValueError("missing required video/context exports: " + str(sorted(EXPORTS - symbols)))
    return subtype


def universal(data):
    if len(data) < 48 or data[:8] != struct.pack(">II", 0xcafebabe, 2):
        raise ValueError("Leopard GLDs require both PPC32 and PPC64")
    result, ranges = {}, []
    for i in range(2):
        cpu, subtype, offset, size, alignment = struct.unpack_from(">IIIII", data, 8 + i * 20)
        if cpu not in CPUS or cpu in result or offset < 48 or offset + size > len(data):
            raise ValueError("invalid universal slice")
        if alignment > 30 or offset % (1 << alignment):
            raise ValueError("invalid slice alignment")
        if any(offset < b and offset + size > a for a, b in ranges):
            raise ValueError("overlapping universal slices")
        ranges.append((offset, offset + size))
        payload = data[offset:offset + size]
        if thin(payload, cpu) != subtype:
            raise ValueError("slice subtype mismatch")
        result[cpu] = hashlib.sha256(payload).hexdigest()
    return result


def package(ppc32, ppc64, output):
    prepared, manifest = {}, {}
    for name in MODULES:
        slices = [(directory / (name + ".dylib")).read_bytes() for directory in (ppc32, ppc64)]
        subtypes = [thin(payload, cpu) for payload, cpu in zip(slices, CPUS)]
        data = bytearray(struct.pack(">II", 0xcafebabe, 2) + bytes(40))
        for i, (payload, cpu, subtype) in enumerate(zip(slices, CPUS, subtypes)):
            offset = (len(data) + 4095) & ~4095
            data.extend(bytes(offset - len(data)))
            data.extend(payload)
            struct.pack_into(">IIIII", data, 8 + i * 20, cpu, subtype, offset, len(payload), 12)
        hashes = universal(data)
        prepared[name] = data
        manifest[name] = {"sha256": hashlib.sha256(data).hexdigest(),
                          "ppc32": hashes[CPUS[0]], "ppc64": hashes[CPUS[1]]}
    output.mkdir(parents=True, exist_ok=True)
    for name, data in prepared.items():
        temporary = output / (name + ".new")
        temporary.write_bytes(data)
        temporary.replace(output / name)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--ppc32", type=Path)
    parser.add_argument("--ppc64", type=Path)
    args = parser.parse_args()
    if bool(args.ppc32) != bool(args.ppc64):
        parser.error("provide both architecture directories")
    if args.ppc32:
        package(args.ppc32, args.ppc64, args.output)
    manifest = json.loads((args.output / "manifest.json").read_text())
    for name in MODULES:
        data = (args.output / name).read_bytes()
        slices = universal(data)
        if manifest[name] != {"sha256": hashlib.sha256(data).hexdigest(),
                              "ppc32": slices[CPUS[0]], "ppc64": slices[CPUS[1]]}:
            raise ValueError("manifest mismatch: " + name)
        print(name + ": PPC32 + PPC64, required exports and hashes verified")


if __name__ == "__main__":
    main()
