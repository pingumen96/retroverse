#!/usr/bin/env python3
"""Independent oracle of the room selection's title keys, version tags and version choice (ADR-0010 section 3, I3
step 2): the golden tables of tests/unit/world/test_title_key.cpp.

    python -I scripts/title_key_oracle.py                         print the golden tables
    python -I scripts/title_key_oracle.py --check <file> [<file>]  exit 1 unless the files hold every line of them

Title key ("title-key/1") of a display title:
  1. every group removed: from an opening '(' (or '[') that lies in no earlier group, scanned from the left, to the
     ')' (or ']') that balances it, only characters of its own kind counted; an opening character nothing balances and
     a closing one outside a group stay as text;
  2. ASCII white space (space, tab, CR, LF, FF, VT) trimmed and every run of it made one space;
  3. the separators " : ", " - " and " – " replaced from the left by ": " (": " stays);
  4. the catalog's sort key (version 1): white space again, then lower case (the invariant table; the tables here are
     ASCII, where it is str.lower()).
  An empty result takes the title through step 4 alone.

Version tags of a file name (the last element of its relative path):
  - region rank: the first group in parentheses made only of No-Intro region names separated by ", ", or that is a
    GoodTools country code (a code of GOODTOOLS_CODES, or two or more distinct letters of GOODTOOLS_LETTERS); 0 when it
    names USA or World (U, W, 4 among the codes), 1 when it names Europe (E), 2 otherwise and without one;
  - status: a group in parentheses with an item (", " between items) whose first word is Beta, Proto, Demo, Sample or
    Pirate, or a group in brackets starting with 'h', 'b' or 'a' not followed by a lower-case letter;
  - revision: the highest N of the items "Rev N" (1 to 9 decimal digits) of the groups in parentheses, else 0.
The representative of a title key among the eligible games is the first by (region rank, status, -revision, lookup
key, GameId); the lookup key is the relative path upper-cased (ASCII here).

Selection of the table (selectRoom): games are the rows in state game or missing that are not hidden; a missing one is
"unavailable"; when some game has a cover, a game without one is "no-cover"; the others are grouped by title key, the
representative is a "title" and its other versions "duplicate-title". Rows that are no games are "none". A row's title
is its gamelist title, else its file name's stem (the last extension removed) trimmed. GameIds are the row index + 1.

The C++ code (src/world/src/title_key.cpp, room_selection.cpp) is checked against this script, which shares nothing
with it but the definitions above. The tables are printed as C++ initializers (UTF-8 bytes beyond ASCII as \\xNN), to be
pasted into the test as they are; --check finds every line there.
"""

import argparse
import sys

NO_INTRO_REGIONS = {
    'Argentina', 'Asia', 'Australia', 'Austria', 'Belgium', 'Brazil', 'Canada', 'Chile', 'China', 'Croatia', 'Czech',
    'Denmark', 'Europe', 'Finland', 'France', 'Germany', 'Greece', 'Hong Kong', 'Hungary', 'India', 'Ireland',
    'Israel', 'Italy', 'Japan', 'Korea', 'Latin America', 'Mexico', 'Netherlands', 'New Zealand', 'Norway', 'Poland',
    'Portugal', 'Russia', 'Scandinavia', 'Slovakia', 'South Africa', 'Spain', 'Sweden', 'Switzerland', 'Taiwan',
    'Turkey', 'UK', 'United Kingdom', 'USA', 'World',
}
GOODTOOLS_CODES = {'1', '4', 'A', 'B', 'C', 'E', 'F', 'FC', 'FN', 'G', 'GR', 'HK', 'I', 'J', 'K', 'NL', 'S', 'SW', 'U',
                   'UK', 'W'}
GOODTOOLS_LETTERS = set('ABCEFGIJKSUW')
STATUS_WORDS = {'Beta', 'Proto', 'Demo', 'Sample', 'Pirate'}
WHITESPACE = ' \t\r\n\f\v'
EN_DASH = '–'

# The selection table: (relative path, gamelist title or '' for an unscraped game, cover extension or '', state,
# hidden, favourite, ES-DE play count, Retroverse sessions, rating in tenths or -1). The priority data are the C++
# test's (this script does not rank titles); the first 40 rows are the invented file names of the roadmap's table.
ROWS = [
    ('Pixel Quest (USA).sfc', 'Pixel Quest', '.png', 'game', False, False, 0, 0, -1),
    ('Pixel Quest (Europe).sfc', 'Pixel Quest', '.png', 'game', False, True, 0, 0, -1),
    ('Pixel Quest (Japan).sfc', 'Pixel Quest', '', 'game', False, False, 0, 0, -1),
    ('Star Harbor (Europe).sfc', 'Star Harbor', '.png', 'game', False, False, 0, 0, -1),
    ('Star Harbor (Europe) (Rev 1).sfc', 'Star Harbor', '.png', 'game', False, False, 0, 0, -1),
    ('Thunder Lanes (USA, Europe).sfc', 'Thunder Lanes', '.png', 'game', False, False, 0, 0, -1),
    ('Thunder Lanes (Japan).sfc', 'Thunder Lanes', '.png', 'game', False, False, 7, 0, -1),
    ('Moon Garden (Japan, USA).sfc', 'Moon Garden', '.png', 'game', False, False, 0, 0, -1),
    ('Moon Garden (Europe).sfc', 'Moon Garden', '.png', 'game', False, False, 0, 0, -1),
    ('Crystal Depths (World).sfc', 'Crystal Depths', '.png', 'game', False, False, 0, 0, -1),
    ('Crystal Depths (USA).sfc', 'Crystal Depths', '.png', 'game', False, False, 0, 0, -1),
    ('River Run (USA) (Beta).sfc', 'River Run', '.png', 'game', False, False, 0, 0, -1),
    ('River Run (Japan).sfc', 'River Run', '.png', 'game', False, False, 0, 0, -1),
    ('Glass Tower (Europe) (Proto).sfc', 'Glass Tower', '.png', 'game', False, False, 0, 0, 10),
    ('Glass Tower (Europe).sfc', 'Glass Tower', '.png', 'game', False, False, 0, 0, 9),
    ('Bolt Brigade (U) [!].smc', 'Bolt Brigade', '.png', 'game', False, False, 0, 0, -1),
    ('Bolt Brigade (U) [h1C].smc', 'Bolt Brigade', '.png', 'game', False, False, 0, 0, -1),
    ('Bolt Brigade (E).smc', 'Bolt Brigade', '.png', 'game', False, False, 0, 0, -1),
    ('Iron Sky (JU).smc', 'Iron Sky', '.png', 'game', False, False, 0, 0, -1),
    ('Iron Sky (UE) [a1].smc', 'Iron Sky', '.png', 'game', False, False, 0, 0, -1),
    ('Lucky Lagoon (UK).smc', 'Lucky Lagoon', '.png', 'game', False, False, 0, 0, 8),
    ('Dungeon Story - The Lost Key (USA).sfc', 'Dungeon Story: The Lost Key', '.png', 'game', False, False, 0, 0, -1),
    ('Dungeon Story - The Lost Key (Europe).sfc', 'Dungeon Story - The Lost Key', '.png', 'game', False, False, 0, 0,
     -1),
    ('Neon Drift - Turbo (Europe).sfc', 'Neon Drift : Turbo', '.png', 'game', False, False, 0, 0, -1),
    ('Neon Drift - Turbo (Europe) (Rev 1).sfc', 'Neon Drift: Turbo', '.png', 'game', False, False, 0, 0, -1),
    ('Sky Pirates - Gold (Japan).sfc', 'Sky Pirates ' + EN_DASH + ' Gold', '.png', 'game', False, False, 0, 0, -1),
    ('Sky Pirates - Gold (Asia).sfc', 'Sky Pirates - Gold', '.jpg', 'game', False, False, 0, 0, -1),
    ('SUPER BLOCKS (USA).sfc', 'SUPER BLOCKS', '.png', 'game', False, False, 0, 0, -1),
    ('Super Blocks (Europe).sfc', 'Super Blocks', '.png', 'game', False, False, 0, 0, -1),
    ('Ace Racer (USA).sfc', 'Ace Racer', '.png', 'game', False, False, 0, 0, -1),
    ('Ace Racer (USA) (Rev 1).sfc', 'Ace Racer (Rev 1)', '.png', 'game', False, False, 0, 0, -1),
    ('Ace Racer (USA) (Rev 2).sfc', '', '', 'game', False, False, 0, 0, -1),
    ('Brass Engine (Japan) (Rev 2).sfc', 'Brass Engine', '.png', 'game', False, False, 0, 0, -1),
    ('Brass Engine (Japan) (Rev 10).sfc', 'Brass Engine', '.png', 'game', False, False, 0, 3, -1),
    ('Quiet Forest (Europe).sfc', '', '.png', 'game', False, False, 0, 0, -1),
    ('Quiet Forest (USA) (Demo).sfc', 'Quiet Forest', '.png', 'game', False, False, 0, 0, -1),
    ('Hidden Fort (Pirate).sfc', 'Hidden Fort', '.png', 'game', False, False, 0, 0, -1),
    ('Hidden Fort (Asia).sfc', 'Hidden Fort', '.png', 'game', False, False, 0, 0, -1),
    ('Echo Valley (Europe) (Sample) (Rev A).sfc', 'Echo Valley', '.png', 'game', False, False, 0, 0, -1),
    ('Fable Hill (Rev 1) (USA) (En,Fr).sfc', 'Fable Hill', '.png', 'game', False, False, 0, 0, -1),
    # Entries of the other states: a missing file with its cover, a hidden game, a file that is no game, a folder.
    ('Mirror Maze (Europe) (En,Fr,De).sfc', 'Mirror Maze', '.png', 'missing', False, False, 0, 0, -1),
    ('Gone Hiking (USA).sfc', 'Gone Hiking', '.png', 'game', True, False, 0, 0, -1),
    ('Pixel Quest (USA).txt', '', '.png', 'not-a-game', False, False, 0, 0, -1),
    ('Pixel Collection', 'Pixel Collection', '.png', 'folder', False, False, 0, 0, -1),
]

# Title keys of titles beyond the table's: groups nested, unbalanced or of both kinds, separators, white space, a title
# made only of groups, accents kept.
TITLE_KEY_CASES = [
    'Pixel Quest',
    '  Pixel   Quest  ',
    'Pixel\tQuest\n',
    'Pixel Quest (USA)',
    'Pixel Quest [!]',
    'Pixel Quest (Hack (v2)) Extra',
    'Pixel Quest (a [b) c]',
    'Pixel Quest [a (b] c)',
    'Pixel Quest (unbalanced',
    'Pixel Quest ) stray',
    'Pixel Quest ]] stray [',
    '(Untitled)',
    '[Untitled] (Beta)',
    'Dungeon Story: The Lost Key',
    'Dungeon Story - The Lost Key',
    'Dungeon Story : The Lost Key',
    'Dungeon Story ' + EN_DASH + ' The Lost Key',
    'Dungeon Story:The Lost Key',
    'Dungeon Story-The Lost Key',
    'Quest - Part : Two',
    'Quest  -  Part',
    'Quest -',
    'Quest (Disc 1) - Part Two',
    'Spider-Man 2',
    'Quest II',
    'Quest 2',
    'Café Racer',
    'CAFÉ RACER',
]

# Version tags of file names beyond the table's.
TAG_CASES = [
    'Pixel Quest.sfc',
    'Pixel Quest (USA).sfc',
    'Pixel Quest (World).sfc',
    'Pixel Quest (Europe).sfc',
    'Pixel Quest (Japan).sfc',
    'Pixel Quest (USA, Europe).sfc',
    'Pixel Quest (Europe, USA).sfc',
    'Pixel Quest (Japan, Europe).sfc',
    'Pixel Quest (Japan, Korea).sfc',
    'Pixel Quest (USA,Europe).sfc',
    'Pixel Quest (En,Fr,De) (Europe).sfc',
    'Pixel Quest (Europe) (USA).sfc',
    'Pixel Quest (U).smc',
    'Pixel Quest (E).smc',
    'Pixel Quest (J).smc',
    'Pixel Quest (W).smc',
    'Pixel Quest (4).smc',
    'Pixel Quest (JU).smc',
    'Pixel Quest (UE).smc',
    'Pixel Quest (JE).smc',
    'Pixel Quest (JUE).smc',
    'Pixel Quest (UU).smc',
    'Pixel Quest (UK).smc',
    'Pixel Quest (Unl).smc',
    'Pixel Quest (PD).smc',
    'Pixel Quest (u).smc',
    'Pixel Quest (USA) (Beta).sfc',
    'Pixel Quest (USA) (Beta 2).sfc',
    'Pixel Quest (USA) (Proto 1).sfc',
    'Pixel Quest (USA) (Demo, Kiosk).sfc',
    'Pixel Quest (USA) (Kiosk Demo).sfc',
    'Pixel Quest (Sample).sfc',
    'Pixel Quest (Pirate).sfc',
    'Pixel Quest (Betamax).sfc',
    'Pixel Quest (U) [h].smc',
    'Pixel Quest (U) [h1C].smc',
    'Pixel Quest (U) [hI].smc',
    'Pixel Quest (U) [b1].smc',
    'Pixel Quest (U) [a2].smc',
    'Pixel Quest (U) [!].smc',
    'Pixel Quest (U) [T+Eng].smc',
    'Pixel Quest (U) [hack].smc',
    'Pixel Quest (USA) (Rev 1).sfc',
    'Pixel Quest (USA) (Rev 2) (Rev 10).sfc',
    'Pixel Quest (USA) (Rev A).sfc',
    'Pixel Quest (USA) (Rev 1, Beta).sfc',
    'Pixel Quest (USA) (Rev1).sfc',
    'Pixel Quest (USA) (V1.1).sfc',
    'Pixel Quest (USA) (Rev 1234567890).sfc',
    'Pixel Quest ((USA)).sfc',
    'Pixel Quest (USA',
    'Pixel Quest [USA] (Europe).sfc',
]


def groups_of(text):
    """The groups of `text`: (opening character, content, start, end past the closing character)."""
    found = []
    i = 0
    while i < len(text):
        opening = text[i]
        if opening not in '([':
            i += 1
            continue
        closing = ')' if opening == '(' else ']'
        depth = 0
        end = None
        for j in range(i, len(text)):
            if text[j] == opening:
                depth += 1
            elif text[j] == closing:
                depth -= 1
                if depth == 0:
                    end = j
                    break
        if end is None:
            i += 1
            continue
        found.append((opening, text[i + 1:end], i, end + 1))
        i = end + 1
    return found


def collapse(text):
    words = []
    current = ''
    for character in text:
        if character in WHITESPACE:
            if current:
                words.append(current)
            current = ''
        else:
            current += character
    if current:
        words.append(current)
    return ' '.join(words)


def sort_key(title):
    return collapse(title).lower()


def title_key(title):
    kept = ''
    position = 0
    for _, _, start, end in groups_of(title):
        kept += title[position:start]
        position = end
    kept += title[position:]
    text = collapse(kept)
    unified = ''
    i = 0
    while i < len(text):
        for separator in (' : ', ' - ', ' ' + EN_DASH + ' '):
            if text.startswith(separator, i):
                unified += ': '
                i += len(separator)
                break
        else:
            unified += text[i]
            i += 1
    return sort_key(unified if unified else title)


def region_rank(content):
    if content == '':
        return None
    if content in GOODTOOLS_CODES:
        return 0 if content in ('U', 'W', '4') else 1 if content == 'E' else 2
    if len(content) >= 2 and all(c in GOODTOOLS_LETTERS for c in content) and len(set(content)) == len(content):
        return 0 if ('U' in content or 'W' in content) else 1 if 'E' in content else 2
    items = content.split(', ')
    if not all(item in NO_INTRO_REGIONS for item in items):
        return None
    return 0 if ('USA' in items or 'World' in items) else 1 if 'Europe' in items else 2


def version_tags(name):
    rank, status, revision, have_region = 2, False, 0, False
    for opening, content, _, _ in groups_of(name):
        if opening == '(' and not have_region:
            found = region_rank(content)
            if found is not None:
                rank, have_region = found, True
                continue
        if opening == '[':
            if content[:1] in ('h', 'b', 'a') and not ('a' <= content[1:2] <= 'z'):
                status = True
            continue
        for item in content.split(', '):
            if item.split(' ')[0] in STATUS_WORDS:
                status = True
            if item.startswith('Rev '):
                digits = item[4:]
                if 1 <= len(digits) <= 9 and all('0' <= c <= '9' for c in digits):
                    revision = max(revision, int(digits))
    return rank, status, revision


def display_title(row):
    path, title = row[0], row[1]
    if title:
        return title
    name = path.split('/')[-1]
    stem = name[:name.rfind('.')] if '.' in name else name
    return stem.strip(WHITESPACE) or name.strip(WHITESPACE)


def select(rows):
    """Per row: (outcome, representative row index or -1)."""
    outcomes = [('none', -1)] * len(rows)
    games = [i for i, row in enumerate(rows) if row[3] in ('game', 'missing') and not row[4]]
    any_cover = any(rows[i][2] for i in games)
    groups = {}
    for i in games:
        row = rows[i]
        if row[3] == 'missing':
            outcomes[i] = ('unavailable', -1)
        elif any_cover and not row[2]:
            outcomes[i] = ('no-cover', -1)
        else:
            groups.setdefault(title_key(display_title(row)), []).append(i)
    for members in groups.values():
        def order(i):
            rank, status, revision = version_tags(rows[i][0].split('/')[-1])
            return (rank, status, -revision, rows[i][0].upper(), i + 1)
        members.sort(key=order)
        outcomes[members[0]] = ('title', -1)
        for i in members[1:]:
            outcomes[i] = ('duplicate-title', members[0])
    return outcomes


def cpp_literal(text):
    """A C++ narrow string literal of the UTF-8 bytes of `text`: printable ASCII as it is, other bytes (and the quote
    and the backslash) as \\xNN, the literal split where a hexadecimal digit follows an escape (which would otherwise
    extend it)."""
    out = '"'
    escaped = False
    for byte in text.encode('utf-8'):
        character = chr(byte)
        if 0x20 <= byte < 0x7F and character not in '"\\':
            if escaped and character in '0123456789abcdefABCDEF':
                out += '" "'
            out += character
            escaped = False
        else:
            out += '\\x%02X' % byte
            escaped = True
    return out + '"'


def flag(value):
    return 'true' if value else 'false'


def rank_name(rank):
    return ('RegionRank::UsaOrWorld', 'RegionRank::Europe', 'RegionRank::Other')[rank]


def tables():
    lines = ['// Selection rows: path, title, cover, state, hidden, favourite, ES-DE plays, sessions, rating.']
    for row in ROWS:
        lines.append('{%s, %s, %s, %s, %s, %s, %d, %d, %d},' % (
            cpp_literal(row[0]), cpp_literal(row[1]), cpp_literal(row[2]), cpp_literal(row[3]), flag(row[4]),
            flag(row[5]), row[6], row[7], row[8]))
    lines.append('// Selection outcomes: row, title key, region rank, status, revision, outcome, representative.')
    for i, (outcome, representative) in enumerate(select(ROWS)):
        rank, status, revision = version_tags(ROWS[i][0].split('/')[-1])
        lines.append('{%d, %s, %s, %s, %d, %s, %d},' % (
            i, cpp_literal(title_key(display_title(ROWS[i]))), rank_name(rank), flag(status), revision,
            cpp_literal(outcome), representative))
    lines.append('// Title keys: title, key.')
    for title in TITLE_KEY_CASES:
        lines.append('{%s, %s},' % (cpp_literal(title), cpp_literal(title_key(title))))
    lines.append('// Version tags: file name, region rank, status, revision.')
    for name in TAG_CASES:
        rank, status, revision = version_tags(name)
        lines.append('{%s, %s, %s, %d},' % (cpp_literal(name), rank_name(rank), flag(status), revision))
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', metavar='FILE', nargs='+', help='exit 1 unless the FILEs hold every line of the '
                        'golden tables')
    args = parser.parse_args()
    lines = [line for line in tables() if not line.startswith('//')]
    if not args.check:
        sys.stdout.write('\n'.join(tables()) + '\n')
        return 0
    text = ''
    for path in args.check:
        with open(path, encoding='utf-8') as file:
            text += file.read()
    held = {line.strip() for line in text.splitlines()}
    missing = [line for line in lines if line not in held]
    for line in missing:
        sys.stdout.write('missing: %s\n' % line)
    sys.stdout.write('%d of %d golden lines found in %s\n' % (len(lines) - len(missing), len(lines),
                                                             ', '.join(args.check)))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
