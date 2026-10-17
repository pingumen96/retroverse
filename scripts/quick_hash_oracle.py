#!/usr/bin/env python3
"""Independent oracle of the catalog's quick hash (ADR-0008 section 4, I2 step 11b): the golden vectors of
tests/unit/catalog/test_fingerprint.cpp.

    python -I scripts/quick_hash_oracle.py                 print the golden table
    python -I scripts/quick_hash_oracle.py --check <file>  exit 1 unless <file> holds every line of the table

The quick hash of a file of n bytes (rule 1):
  - n <= 131072 (2 x 64 KiB): SHA-256 of the whole content, each byte once (the empty file: SHA-256 of nothing);
  - n > 131072: SHA-256 of the bytes [0, 65536) followed by the bytes [n - 65536, n).
Nothing else enters the digest (neither the size nor the name). The C++ code (src/catalog/src/fingerprint.cpp, on
rv_core's own SHA-256) is checked against this script, which shares nothing with it but the definition: Python's
hashlib computes the digests here.

The contents are generated, never committed: byte i of a content is the top byte of the low 24 bits of a linear
congruential generator (state = state * 1103515245 + 12345 mod 2^32, starting from 0x52564648, stepped once before each
byte: byte = (state >> 16) & 0xFF), a sequence without a short period, so that a file's two ends differ and a hash of
the wrong range shows. The table is printed as C++ initializers {size, "digest"}, so that it is pasted into the test as
it is and --check finds it there.
"""

import argparse
import hashlib
import sys

PART = 64 * 1024
WHOLE = 2 * PART
SEED = 0x52564648

# The sizes of the golden contents: empty, tiny, around one part, around the whole-file limit, beyond it.
SIZES = [0, 1, 3, PART - 1, PART, PART + 1, WHOLE - 1, WHOLE, WHOLE + 1, 3 * PART, 1024 * 1024 + 7]


def content(size):
    """The generated content of `size` bytes."""
    state = SEED
    out = bytearray(size)
    for i in range(size):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (state >> 16) & 0xFF
    return bytes(out)


def quick_hash(data):
    """The quick hash of a file whose whole content is `data`, as lowercase hexadecimal."""
    if len(data) <= WHOLE:
        return hashlib.sha256(data).hexdigest()
    return hashlib.sha256(data[:PART] + data[-PART:]).hexdigest()


def table():
    return ['{%d, "%s"},' % (size, quick_hash(content(size))) for size in SIZES]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', metavar='FILE', help='exit 1 unless FILE holds every line of the table')
    args = parser.parse_args()
    lines = table()
    if not args.check:
        sys.stdout.write('\n'.join(lines) + '\n')
        return 0
    with open(args.check, encoding='utf-8') as source:
        held = {line.strip() for line in source}
    missing = [line for line in lines if line not in held]
    for line in missing:
        sys.stdout.write('missing: %s\n' % line)
    sys.stdout.write('%d of %d golden quick hashes found in %s\n' % (len(lines) - len(missing), len(lines),
                                                                      args.check))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
