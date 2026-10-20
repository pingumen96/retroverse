#!/usr/bin/env python3
"""Independent oracle of the world generator's seeds, random sequences and canonical JSON (ADR-0010 sections 7 and 8,
I3 step 0b): the golden values of tests/unit/core/test_seed_random.cpp and tests/unit/core/test_canonical_json.cpp.

    python -I scripts/world_seed_oracle.py                         print the golden tables
    python -I scripts/world_seed_oracle.py --check <file> [<file>]  exit 1 unless the files hold every line of them

Seeds (core::deriveSeed): SHA-256 over the UTF-8 bytes of the tuple ("rv-seed/1", parts...), its elements separated by
NUL characters (no NUL after the last); an integer part is its decimal ASCII digits, with '-' before a negative one;
the seed is the digest's first 8 bytes read big-endian.

Random (core::Random, "splitmix64/1"): SplitMix64 (Steele, Lea and Flood, 2014; Vigna's splitmix64.c): the state
advances by 0x9E3779B97F4A7C15 (mod 2^64) and each output is the advanced state mixed by
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9, z = (z ^ (z >> 27)) * 0x94D049BB133111EB, z ^ (z >> 31). Its distributions:
  - below(n), n >= 1: outputs x are drawn until x >= (2^64 - n) mod n, the result is x mod n (unbiased; at least one
    output drawn, n = 1 included); below(0) is 0 and draws nothing;
  - between(lo, hi), signed 64-bit, both included: lo + below(hi - lo + 1) in 64-bit two's complement, for the whole
    range (hi - lo + 1 = 2^64) lo + one output; lo when hi < lo, nothing drawn;
  - unit(): (x >> 11) * 2^-53, in [0, 1), one output;
  - shuffle(items): Fisher-Yates from the last item, for i = n - 1 down to 1 swap items i and below(i + 1).

Canonical JSON (core::writeCanonicalJson): object members sorted by their keys' code points (the UTF-8 bytes in
order), integers in decimal (no fraction, no exponent; a floating-point number is refused), strings in UTF-8 with
exactly '"', '\\' and the control characters U+0000 to U+001F escaped (\\b \\t \\n \\f \\r, the others \\u00XX in
lowercase hexadecimal), no white space between tokens, except that a top-level array (the document when it is an
array, else a member of the document's object) that is not empty holds one element per line: '[', a line feed, the
elements separated by ',' and a line feed, a line feed, ']'. The text ends with one line feed. Python's json module
writes nested values here (sort_keys, ensure_ascii=False, separators without spaces): it escapes exactly those
characters, as the C++ code does on its own.

The C++ code (src/core/src/seed.cpp, random.cpp, canonical_json.cpp, on rv_core's own SHA-256) is checked against
this script, which shares nothing with it but the definitions: Python's hashlib and json compute the values here. The
tables are printed as C++ initializers, strings escaped as C++ narrow literals of their UTF-8 bytes, so that they are
pasted into the tests as they are and --check finds them there.
"""

import argparse
import hashlib
import json
import sys

SEED_TAG = 'rv-seed/1'
MASK64 = (1 << 64) - 1
GOLDEN_GAMMA = 0x9E3779B97F4A7C15

# Tuples of seed parts, strings only (an integer part is its decimal digits: the C++ test checks that equivalence).
SEED_TUPLES = [
    # No part (the tag alone), an empty part, and parts that would run together without their separators.
    (),
    ('',),
    ('', ''),
    ('ab', 'c'),
    ('a', 'bc'),
    ('abc',),
    # The world's default seed and the local seeds of ADR-0010 section 7 (the parents' seeds in decimal).
    ('world', 'esde'),
    ('world', 'main'),
    ('room', 'room:snes'),
    ('fixture', 'room:snes/fx:w03'),
    ('prop', 'fx:w03/l:2'),
    ('place', 'g3c374d5e06145c9b', 'cartridge_home_16bit'),
    # The room ids of the building (ADR-0013 section 5): an expansion of a console room, a floor's shared room and its
    # expansion, the arcade room and its expansion, and a fixture of a shared room; seeds derive from the id's text.
    ('room', 'room:snes:2'),
    ('room', 'area:shared:gen4'),
    ('room', 'area:shared:gen4:2'),
    ('room', 'area:arcade'),
    ('room', 'area:arcade:3'),
    ('fixture', 'area:shared:gen4/fx:w03'),
    # Integers as their decimal digits: zero, negative, the 64-bit limits.
    ('0',),
    ('-42',),
    ('-9223372036854775808', '9223372036854775807', '18446744073709551615'),
    # Text beyond ASCII: a composed and a decomposed e with acute accent (apart), a character outside the BMP.
    ('café',),
    ('café',),
    ('\U0001f3ae arcade',),
]

# The chain of ADR-0010 section 7 from one library: (parent part or None, parts...) evaluated in order, each parent
# the seed of the line it names.
CHAIN = [
    ('world', None, ('world', 'esde')),
    ('room', 'world', ('room', 'room:snes')),
    ('fixture', 'room', ('fixture', 'room:snes/fx:w03')),
    ('prop', 'room', ('prop', 'fx:w03/l:2')),
    ('place', 'room', ('place', 'g3c374d5e06145c9b', 'cartridge_home_16bit')),
]

# Seeds of the generator's sequences: zero, one, all ones, the gamma, and a derived seed.
SEQUENCE_SEEDS = [0, 1, MASK64, GOLDEN_GAMMA, None]  # None: deriveSeed("world", "esde")

# below(bound) cases: a bound of 1, small bounds, a power of two, and bounds just above 2^63 where about half the
# outputs are rejected.
BELOW_BOUNDS = [1, 2, 3, 6, 10, 1000, 1 << 32, (1 << 63) + 1, MASK64]

# between(lo, hi) cases.
BETWEEN_RANGES = [
    (0, 0),
    (-5, 5),
    (1, 6),
    (0, 4),
    (-150, 150),
    (-(1 << 63), (1 << 63) - 1),
    (-(1 << 63), 0),
    (10, 3),
]

DRAWS = 6

# Canonical JSON cases: input texts (any valid JSON without floating-point numbers), parsed by both sides.
CANONICAL_INPUTS = [
    '0',
    '-7',
    '18446744073709551615',
    '-9223372036854775808',
    'true',
    'null',
    '""',
    '[]',
    '{}',
    '[1, 2, 3]',
    '[[1, 2], [], {"b": 1, "a": [3, 4]}]',
    '{"b": 1, "a": 2, "c": {"z": [], "y": {}, "x": [1, [2, 3]]}}',
    '{"placements": [{"slot": "s:1.2", "game": "g0000000000000001"}, {"slot": "s:0.0", "game": "g2"}], '
    '"schemaVersion": 1, "empty": [], "flags": [true, false, null]}',
    '{"a": 1, "B": 2, "_": 3, "~": 4, "é": 5, "é": 6, "\U0001f3ae": 7, "￿": 8, "": 9}',
    '{"text": "quote \\" backslash \\\\ slash / tab \\t line \\n return \\r back \\b feed \\f"}',
    '["\\u0000\\u0001\\u001f\\u007f", "é  ", "\\ud83c\\udfae"]',
    '{"nested": {"array": [{"deep": [1, 2]}]}, "top": [{"b": 2, "a": 1}]}',
]


def derive_seed(parts):
    data = '\0'.join((SEED_TAG,) + tuple(str(part) for part in parts)).encode('utf-8')
    return int.from_bytes(hashlib.sha256(data).digest()[:8], 'big')


class SplitMix64:
    def __init__(self, seed):
        self.state = seed & MASK64

    def next(self):
        self.state = (self.state + GOLDEN_GAMMA) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)

    def below(self, bound):
        if bound == 0:
            return 0
        limit = ((1 << 64) - bound) % bound
        while True:
            x = self.next()
            if x >= limit:
                return x % bound

    def between(self, low, high):
        if high < low:
            return low
        span = high - low + 1
        offset = self.next() if span == 1 << 64 else self.below(span)
        value = (low + offset) & MASK64
        return value - (1 << 64) if value >= 1 << 63 else value

    def unit(self):
        return (self.next() >> 11) * 2.0 ** -53

    def shuffle(self, items):
        for i in range(len(items) - 1, 0, -1):
            j = self.below(i + 1)
            items[i], items[j] = items[j], items[i]


def canonical(value):
    """The canonical text of a parsed document."""
    def nested(element):
        return json.dumps(element, sort_keys=True, ensure_ascii=False, separators=(',', ':'), allow_nan=False)

    def array_lines(array):
        if not array:
            return '[]'
        return '[\n' + ',\n'.join(nested(element) for element in array) + '\n]'

    if isinstance(value, list):
        text = array_lines(value)
    elif isinstance(value, dict):
        members = []
        for key in sorted(value):
            member = value[key]
            members.append(nested(key) + ':' + (array_lines(member) if isinstance(member, list) else nested(member)))
        text = '{' + ','.join(members) + '}'
    else:
        text = nested(value)
    return text + '\n'


def cpp_literal(text):
    """A C++ narrow string literal of the UTF-8 bytes of `text`: printable ASCII as it is, other bytes as \\xNN, the
    literal split where a hexadecimal digit follows an escape (which would otherwise extend it)."""
    out = '"'
    escaped = False
    for byte in text.encode('utf-8', 'surrogatepass'):
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


def u64(value):
    return '0x%016xull' % value


def i64(value):
    # The minimum has no literal of its own in C++: written as an expression.
    return '(-9223372036854775807ll - 1)' if value == -(1 << 63) else '%dll' % value


def seed_lines():
    lines = []
    for parts in SEED_TUPLES:
        lines.append('{{%s}, %s},' % (', '.join(cpp_literal(part) for part in parts), u64(derive_seed(parts))))
    return lines


def chain_lines():
    lines = []
    seeds = {}
    for name, parent, parts in CHAIN:
        full = ((seeds[parent],) if parent else ()) + parts
        seeds[name] = derive_seed(full)
        lines.append('{"%s", "%s", {%s}, %s},' % (name, parent or '', ', '.join(cpp_literal(part) for part in parts),
                                                 u64(seeds[name])))
    return lines


def sequence_seeds():
    return [derive_seed(('world', 'esde')) if seed is None else seed for seed in SEQUENCE_SEEDS]


def next_lines():
    lines = []
    for seed in sequence_seeds():
        generator = SplitMix64(seed)
        values = [generator.next() for _ in range(DRAWS)]
        lines.append('{%s, {%s}},' % (u64(seed), ', '.join(u64(value) for value in values)))
    return lines


def below_lines():
    lines = []
    seed = sequence_seeds()[-1]
    for bound in BELOW_BOUNDS:
        generator = SplitMix64(seed)
        values = [generator.below(bound) for _ in range(DRAWS)]
        lines.append('{%s, %s, {%s}, %s},' % (u64(seed), u64(bound), ', '.join(u64(value) for value in values),
                                              u64(generator.state)))
    return lines


def between_lines():
    lines = []
    seed = sequence_seeds()[-1]
    for low, high in BETWEEN_RANGES:
        generator = SplitMix64(seed)
        values = [generator.between(low, high) for _ in range(DRAWS)]
        lines.append('{%s, %s, %s, {%s}, %s},' % (u64(seed), i64(low), i64(high),
                                                  ', '.join(i64(value) for value in values), u64(generator.state)))
    return lines


def unit_lines():
    lines = []
    for seed in sequence_seeds():
        generator = SplitMix64(seed)
        values = [generator.unit() for _ in range(DRAWS)]
        lines.append('{%s, {%s}},' % (u64(seed), ', '.join(value.hex() for value in values)))
    return lines


def shuffle_lines():
    lines = []
    for seed in sequence_seeds():
        for size in (0, 1, 2, 10):
            generator = SplitMix64(seed)
            items = list(range(size))
            generator.shuffle(items)
            lines.append('{%s, %d, {%s}, %s},' % (u64(seed), size, ', '.join(str(item) for item in items),
                                                  u64(generator.state)))
    return lines


def canonical_lines():
    lines = []
    for text in CANONICAL_INPUTS:
        written = canonical(json.loads(text))
        digest = hashlib.sha256(written.encode('utf-8')).hexdigest()
        lines.append('{%s, %s, "%s"},' % (cpp_literal(text), cpp_literal(written), digest))
    return lines


def tables():
    return [
        ('seeds', seed_lines()),
        ('seed chain', chain_lines()),
        ('next', next_lines()),
        ('below', below_lines()),
        ('between', between_lines()),
        ('unit', unit_lines()),
        ('shuffle', shuffle_lines()),
        ('canonical JSON', canonical_lines()),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', metavar='FILE', nargs='+', help='exit 1 unless the FILEs hold every line of the '
                        'tables')
    args = parser.parse_args()
    if not args.check:
        for name, lines in tables():
            sys.stdout.write('// %s\n%s\n' % (name, '\n'.join(lines)))
        return 0
    held = set()
    for path in args.check:
        with open(path, encoding='utf-8') as source:
            held.update(line.strip() for line in source)
    total = 0
    missing = 0
    for name, lines in tables():
        for line in lines:
            total += 1
            if line not in held:
                missing += 1
                sys.stdout.write('missing (%s): %s\n' % (name, line))
    sys.stdout.write('%d of %d golden lines found in %s\n' % (total - missing, total, ', '.join(args.check)))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
