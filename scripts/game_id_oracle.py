#!/usr/bin/env python3
"""Independent oracle of the catalog's GameIds (ADR-0008 section 4): the golden values of
tests/unit/catalog/test_identity.cpp.

    python -I scripts/game_id_oracle.py                 print the golden table
    python -I scripts/game_id_oracle.py --check <file>  exit 1 unless <file> holds every line of the table

A GameId is derived from the tuple ("rv-game/1", library id, ES-DE system name, lookup key): SHA-256 over the UTF-8
bytes of the four elements separated by NUL characters, followed for a counter n >= 1 by a fourth NUL and n in decimal
ASCII; the first 8 bytes of the digest read big-endian with the top bit cleared, written 'g' and 16 lowercase
hexadecimal digits. A value of 0 is no GameId: it counts as a collision and the next counter is tried. The C++ code
(src/catalog/src/identity.cpp, on rv_core's own SHA-256) is checked against this script, which shares nothing with it
but the definition: Python's hashlib computes the digests here.

The table is printed as C++ initializers {library, system, key, counter, id}, the strings escaped as C++ narrow
literals of their UTF-8 bytes, so that it is pasted into the test as it is and --check finds it there. Keys are given
already folded: the lookup key is the relative path upper-cased with Windows' file-system casing, which is not Python's
str.upper() (sharp s, for one, stays), so the test checks the keys of its paths itself.
"""

import argparse
import hashlib
import sys

TAG = 'rv-game/1'
MASK = (1 << 63) - 1

# (library id, ES-DE system name, lookup key, counter).
TUPLES = [
    # Games of the standard fixture (tests/fixtures/esde/standard), library "esde".
    ('esde', 'gb', 'TINY TENNIS (EUROPE).GB', 0),
    ('esde', 'gb', 'COLLECTION A/POCKET PUZZLE (WORLD).GB', 0),
    ('esde', 'gb', 'COLLECTION B/POCKET PUZZLE (WORLD).GB', 0),
    ('esde', 'gb', 'CAFÉ RACER (EUROPE).GB', 0),
    ('esde', 'gb', '.DOT HERO (WORLD).GB', 0),
    ('esde', 'snes', 'ALPHA QUEST (USA).SFC', 0),
    ('esde', 'psx', 'TWIN TRACKS (EUROPE).CUE', 0),
    # The same keys in a second library: other ids.
    ('main', 'gb', 'TINY TENNIS (EUROPE).GB', 0),
    ('main', 'gb', 'COLLECTION A/POCKET PUZZLE (WORLD).GB', 0),
    ('main', 'gb', 'COLLECTION B/POCKET PUZZLE (WORLD).GB', 0),
    ('main', 'gb', 'CAFÉ RACER (EUROPE).GB', 0),
    ('main', 'gb', '.DOT HERO (WORLD).GB', 0),
    ('main', 'snes', 'ALPHA QUEST (USA).SFC', 0),
    ('main', 'psx', 'TWIN TRACKS (EUROPE).CUE', 0),
    # The same key in another system.
    ('esde', 'gbc', 'TINY TENNIS (EUROPE).GB', 0),
    # Keys beyond ASCII: sharp s (which the file-system casing keeps), a decomposed e with its combining acute accent
    # (NFD, apart from the composed form above), a character outside the BMP (a game controller) and one whose upper
    # case the file systems do not map (Deseret small long i).
    ('esde', 'gb', 'STRAßE (GERMANY).GB', 0),
    ('esde', 'gb', 'CAFÉ RACER (EUROPE).GB', 0),
    ('esde', 'gb', '\U0001f3ae ARCADE (WORLD).GB', 0),
    ('esde', 'gb', '\U00010428 LONG I.GB', 0),
    # The counters tried after collisions: 1, 2, and 10 (two decimal digits).
    ('esde', 'gb', 'TINY TENNIS (EUROPE).GB', 1),
    ('esde', 'gb', 'TINY TENNIS (EUROPE).GB', 2),
    ('esde', 'gb', 'TINY TENNIS (EUROPE).GB', 10),
]


def game_id(library, system, key, counter):
    """The value of the tuple: may be 0."""
    data = '\0'.join((TAG, library, system, key)).encode('utf-8')
    if counter:
        data += b'\0' + str(counter).encode('ascii')
    return int.from_bytes(hashlib.sha256(data).digest()[:8], 'big') & MASK


def cpp_literal(text):
    """A C++ narrow string literal of the UTF-8 bytes of `text`: printable ASCII as it is, other bytes as \\xNN, the
    literal split where a hexadecimal digit follows an escape (which would otherwise extend it)."""
    out = '"'
    escaped = False
    for byte in text.encode('utf-8'):
        char = chr(byte)
        if 0x20 <= byte < 0x7F and char not in '"\\':
            if escaped and char in '0123456789abcdefABCDEF':
                out += '" "'
            out += char
            escaped = False
        else:
            out += '\\x%02X' % byte
            escaped = True
    return out + '"'


def table():
    lines = []
    for library, system, key, counter in TUPLES:
        value = game_id(library, system, key, counter)
        text = 'g%016x' % value if value else 'zero: a collision'
        lines.append('{%s, %s, %s, %d, "%s"},' % (cpp_literal(library), cpp_literal(system), cpp_literal(key),
                                                   counter, text))
    return lines


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
    sys.stdout.write('%d of %d golden GameIds found in %s\n' % (len(lines) - len(missing), len(lines), args.check))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
