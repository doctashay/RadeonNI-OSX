#!/usr/bin/env python3
"""Write a patched Leopard SPDisplaysReporter copy with unsigned VRAM conversion."""
import argparse
import hashlib
from pathlib import Path

ORIGINAL = '3c0bceb9f62bffa39c944b336e449ea214c3aa9f97f2bda31163254252091105'
PATCHED = '771c8f950ac474856d94a7bce4e3298f3a7d6b2caea64a2a9c90230f9478902a'
# Replace signed division by 1 MiB with an unsigned shift; preserve byte counts.
WORDS = [(0x16550, '7c66a670', '5466653e'),
         (0x16554, '7cc60194', '60000000'),
         (0x168f8, '7cc6a670', '54c6653e'),
         (0x16a98, '7c66a670', '5466653e'),
         (0x16a9c, '7cc60194', '60000000')]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = bytearray(args.original.read_bytes())
    if hashlib.sha256(data).hexdigest() != ORIGINAL:
        parser.error('unsupported reporter version; original SHA-256 does not match')
    for offset, before, after in WORDS:
        if data[offset:offset+4] != bytes.fromhex(before):
            parser.error('unexpected PowerPC instruction')
        data[offset:offset+4] = bytes.fromhex(after)
    if hashlib.sha256(data).hexdigest() != PATCHED:
        parser.error('patched checksum mismatch')
    with args.output.open('xb') as output:
        output.write(data)
    print(PATCHED)

if __name__ == '__main__':
    main()
