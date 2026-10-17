#!/usr/bin/env python3
"""Census of an ES-DE collection: what ES-DE 3.4.1 lists of each system, and how Retroverse sorts those files and
gamelist entries into entry states and disc groups (ADR-0008 sections 3 and 10).

    python -I scripts/esde_census.py --esde-home <ES-DE home> [--esde-install <folder>] [--rom-root <folder>]
                                     [--system <name>]... [--json] [--output <file>]

An oracle of rv::esde::EsdeLibrarySource (I2 step 7), written apart from it: Python's standard library only, read-only
(folders are listed, the configuration, the gamelists, the MAME lists and the cue sheets and playlists that are games
are read; nothing is ever written but --output). It ports the discovery emulation of the ES-DE survey (a listing of each
system folder with ES-DE's rules, which reproduced ES-DE's own "Total game count") and adds what Retroverse does on top:
  - ES-DE's rules: extensions verbatim and case-sensitive ("." meaning none; a directory without one is never a game),
    every sub-folder, a directory named with a configured extension is a game and is not entered, links followed unless
    they loop, hidden files and folders as ShowHiddenFiles says, a folder holding a noload.txt not entered (one in the
    system folder itself keeps the system from loading), MAME BIOS and device sets skipped in the arcade and neogeo
    systems, files whose ES-DE name is empty skipped; then the gamelist: an entry for an existing file applies to it (a
    path that differs in case only too), with ES-DE's checks (hidden file or parent, noload, extension, BIOS set,
    <hidden> with ShowHiddenGames off); ParseGamelistOnly lists only gamelist entries;
  - Retroverse's states: game, missing file (a <game> entry whose file is gone), not a game with its reason, disc member
    (a game that a .cue or .m3u game of the same system references; m3u -> cue -> tracks, cycles cut), folder (a
    <folder> entry of an existing folder), outside the system folder; entries below a folder that cannot be listed are
    "source unavailable".
The ES-DE home's parent is ES-DE's '~'. The ROM root is --rom-root, else ROMDirectory ('~' and %ESPATH% expanded), else
<home folder>\\ROMs. Only the settings Retroverse uses are extracted from es_settings.xml (never the scraper's).

It prints one line per system that has a ROM folder or entries: ES-DE's count (the games it lists, disc tracks included,
plus the second entries it shows for gamelist paths that differ from their file in case only), then Retroverse's
entries by state, the disc groups and the folders that could not be listed; then the totals and the reasons of the
entries that are not games. --json prints the same as one object. The table is the one rv_tests' [.real-library] test
of EsdeLibrarySource prints, so the two can be compared line by line.
"""

import argparse
import html
import json
import os
import re
import stat
import sys
import time

FILE_ATTRIBUTE_HIDDEN = 0x2
FILE_ATTRIBUTE_DIRECTORY = 0x10
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
WANTED_SETTINGS = {
    'ROMDirectory': 'string',
    'ShowHiddenFiles': 'bool',
    'ShowHiddenGames': 'bool',
    'ParseGamelistOnly': 'bool',
    'MAMENameStripExtraInfo': 'bool',
}
DEFAULT_SETTINGS = {'ROMDirectory': '', 'ShowHiddenFiles': True, 'ShowHiddenGames': True, 'ParseGamelistOnly': False,
                    'MAMENameStripExtraInfo': True}
STATES = ('game', 'missing-file', 'not-a-game', 'disc-member', 'folder', 'outside-system-folder')
COLUMNS = ('esde', 'entries', 'games', 'missing', 'notgame', 'members', 'folders', 'outside', 'groups', 'unavail')
MAX_SHEET_BYTES = 1024 * 1024


# --- Paths and text ---------------------------------------------------------------------------------------------------

def extended(path):
    """The \\\\?\\ form of an absolute Windows path: any length, names kept as they are."""
    path = os.path.abspath(path)
    if os.name != 'nt' or path.startswith('\\\\?\\'):
        return path
    if path.startswith('\\\\'):
        return '\\\\?\\UNC\\' + path[2:]
    return '\\\\?\\' + path


def fold(text):
    """A lookup key: every character of the Basic Multilingual Plane upper-cased when it has a one-character upper case,
    as the Windows file systems compare names."""
    out = []
    for ch in text:
        if ord(ch) <= 0xFFFF:
            upper = ch.upper()
            out.append(upper if len(upper) == 1 else ch)
        else:
            out.append(ch)
    return ''.join(out)


def esde_extension(name):
    dot = name.rfind('.')
    return '.' if dot < 0 else name[dot:]


def esde_stem(name, directory):
    if directory or name == '.':
        return name
    dot = name.rfind('.')
    return name if dot < 0 else name[:dot]


def remove_parenthesis(text):
    """ES-DE's Utils::String::removeParenthesis."""
    result = text
    done = False
    while not done:
        done = True
        for opening, closing in (('(', ')'), ('[', ']')):
            end = result.find(closing)
            if end < 0:
                continue
            start = result.rfind(opening, 0, end + 1)
            if start >= 0:
                result = result[:start] + result[end + 1:]
                done = False
    return result.strip(' \t\n\v\f\r')


def read_text(path, limit=None):
    """A file's text: UTF-8 (a byte order mark skipped), Windows-1252 when it is not UTF-8."""
    with open(extended(path), 'rb') as f:
        data = f.read() if limit is None else f.read(limit + 1)
    if limit is not None and len(data) > limit:
        raise OSError('larger than %d bytes' % limit)
    if data.startswith(b'\xef\xbb\xbf'):
        data = data[3:]
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        return data.decode('cp1252', errors='replace')


def xml_fragment(text):
    """Parses an XML file that may have several top-level elements (ES-DE's), inside a synthetic root."""
    import xml.etree.ElementTree as ElementTree
    text = re.sub(r'^\s*<\?xml[^>]*\?>', '', text)
    return ElementTree.fromstring('<census-root>' + text + '</census-root>')


# --- ES-DE's configuration --------------------------------------------------------------------------------------------

def read_settings(home):
    """The settings Retroverse uses, as ES-DE reads them (the last element of a key wins; another element type is not
    that setting); nothing else of the file is kept."""
    settings = dict(DEFAULT_SETTINGS)
    path = os.path.join(home, 'settings', 'es_settings.xml')
    if not os.path.isfile(extended(path)):
        legacy = os.path.join(home, 'es_settings.xml')
        if not os.path.isfile(extended(legacy)):
            return settings
        path = legacy
    text = read_text(path)
    for kind, name, value in re.findall(r'<(bool|int|float|string)\s+name="([^"]*)"\s+value="([^"]*)"', text):
        if WANTED_SETTINGS.get(name) != kind:
            continue
        value = html.unescape(value)
        settings[name] = value if kind == 'string' else (value[:1] in ('1', 't', 'T', 'y', 'Y'))
    return settings


def resource(home, install, relative):
    for base in (home, install):
        if base:
            path = os.path.join(base, 'resources', *relative.split('/'))
            if os.path.exists(extended(path)):
                return path
    return None


def rom_root(settings, home, install, override):
    if override:
        return os.path.abspath(override)
    home_folder = os.path.dirname(os.path.abspath(home))
    text = settings['ROMDirectory']
    if not text:
        return os.path.join(home_folder, 'ROMs')
    text = text.replace('~', home_folder)
    if install:
        text = text.replace('%ESPATH%', install)
    return os.path.normpath(text)


def read_systems(home, install, root):
    """The definitions of es_systems.xml, the custom file's first, in reading order: name, path, extensions,
    platforms."""
    files = []
    custom = os.path.join(home, 'custom_systems', 'es_systems.xml')
    exclusive = False
    if os.path.isfile(extended(custom)):
        files.append(custom)
        exclusive = xml_fragment(read_text(custom)).find('loadExclusive') is not None
    bundled = resource(home, install, 'systems/windows/es_systems.xml')
    if bundled is None and not files:
        sys.exit('esde_census: no es_systems.xml in the ES-DE home or install folder')
    if bundled is not None and not exclusive:
        files.append(bundled)
    definitions = []
    for path in files:
        system_list = xml_fragment(read_text(path)).find('systemList')
        if system_list is None:
            continue
        for system in system_list.findall('system'):
            def text_of(tag):
                node = system.find(tag)
                return (node.text or '') if node is not None else ''
            name = text_of('name').replace('\n', '')
            fullname = text_of('fullname').replace('\n', '')
            path_text = text_of('path')
            extensions = [e for e in re.split(r'[ \t\r\n,]+', text_of('extension')) if e]
            commands = system.findall('command')
            if not name or not fullname or not path_text or not extensions or not commands:
                continue
            platforms = [p for p in re.split(r'[ \t\r\n,]+', text_of('platform').lower()) if p]
            folder = path_text.replace('%ROMPATH%', root + os.sep)
            folder = folder.replace('~', os.path.dirname(os.path.abspath(home)))
            definitions.append({'name': name, 'folder': os.path.normpath(folder.replace('/', os.sep)),
                                'extensions': extensions, 'platforms': platforms})
    return definitions


def is_arcade(platforms):
    if 'ignore' in platforms:
        return False
    return 'arcade' in platforms or 'neogeo' in platforms


def read_mame(home, install):
    """ES-DE's MAME lists: names first, then BIOS sets, then devices; a missing list ends the reading."""
    names, bioses, devices = {}, set(), set()
    path = resource(home, install, 'MAME/mamenames.xml')
    if path is None:
        return names, bioses, devices
    text = read_text(path)
    for game in re.findall(r'<game>(.*?)</game>', text, re.S):
        mame = re.search(r'<mamename>([^<]*)</mamename>', game)
        real = re.search(r'<realname>([^<]*)</realname>', game)
        if mame:
            names[html.unescape(mame.group(1))] = html.unescape(real.group(1)) if real else ''
    path = resource(home, install, 'MAME/mamebioses.xml')
    if path is None:
        return names, bioses, devices
    bioses = {html.unescape(b) for b in re.findall(r'<bios>([^<]*)</bios>', read_text(path))}
    path = resource(home, install, 'MAME/mamedevices.xml')
    if path is not None:
        devices = {html.unescape(d) for d in re.findall(r'<device>([^<]*)</device>', read_text(path))}
    return names, bioses, devices


# --- One system -------------------------------------------------------------------------------------------------------

class System:
    def __init__(self, definition, settings, mame):
        self.name = definition['name']
        self.folder = definition['folder']
        self.extensions = set(definition['extensions'])
        self.arcade = is_arcade(definition['platforms'])
        self.settings = settings
        self.names, self.bioses, self.devices = mame
        self.items = []          # dicts in walk order
        self.by_key = {}         # key -> [item]
        self.unavailable = {}    # folder key -> reason
        self.entries = []        # dicts: key (None outside the folder), rel, state, reason, dir, gamelist
        self.groups = 0
        self.available = True
        self.hidden_folder = False
        self.noload_root = False

    def configured(self, name, directory):
        extension = esde_extension(name)
        return not (directory and extension == '.') and extension in self.extensions

    def asset(self, name, directory):
        stem = esde_stem(name, directory)
        return self.arcade and (stem in self.bioses or stem in self.devices)

    def esde_name(self, name, directory):
        if self.arcade:
            real = self.names.get(remove_parenthesis(esde_stem(name, directory)), '')
            real = real or remove_parenthesis(esde_stem(name, directory))
            return remove_parenthesis(real) if self.settings['MAMENameStripExtraInfo'] else real
        return esde_stem(name, directory)

    def game_directory(self, name):
        return self.configured(name, True) and not self.asset(name, True) and self.esde_name(name, True) != ''

    # The listing.
    def walk(self):
        try:
            st = os.stat(extended(self.folder))
        except OSError:
            self.available = False
            return
        if not stat.S_ISDIR(st.st_mode):
            self.available = False
            return
        attributes = getattr(st, 'st_file_attributes', 0)
        self.hidden_folder = bool(attributes & FILE_ATTRIBUTE_HIDDEN) or os.path.basename(self.folder).startswith('.')
        # Depth first; each folder with the identities of the folders above it (links that loop are not entered).
        stack = [('', None, [(st.st_dev, st.st_ino)])]
        first = True
        while stack:
            relative, parent, identities = stack.pop()
            full = self.folder if not relative else os.path.join(self.folder, *relative.split('/'))
            try:
                with os.scandir(extended(full)) as listing:
                    children = sorted(listing, key=lambda e: e.name.encode('utf-8', 'surrogatepass'))
            except OSError as error:
                if first:
                    self.available = False
                    return
                self.unavailable[fold(relative)] = str(error)
                continue
            first = False
            for entry in children:
                rel = entry.name if not relative else relative + '/' + entry.name
                path = extended(os.path.join(full, entry.name))
                try:
                    own = entry.stat(follow_symlinks=False)
                    attributes = getattr(own, 'st_file_attributes', 0)
                    # A folder or a link to one by its own attributes (as rv_core's walker tells them); a link to a
                    # file whose target cannot be opened is not listed.
                    directory = bool(attributes & FILE_ATTRIBUTE_DIRECTORY)
                    if not directory and attributes & FILE_ATTRIBUTE_REPARSE_POINT:
                        os.stat(path)
                except OSError:
                    continue
                item = {'rel': rel, 'key': fold(rel), 'name': entry.name, 'dir': directory, 'parent': parent,
                        'hidden': bool(attributes & FILE_ATTRIBUTE_HIDDEN) or entry.name.startswith('.'),
                        'noload': False}
                self.items.append(item)
                index = len(self.items) - 1
                if parent is not None and fold(entry.name) == 'NOLOAD.TXT':
                    self.items[parent]['noload'] = True
                if parent is None and entry.name == 'noload.txt':
                    self.noload_root = True
                if not directory or self.game_directory(entry.name):
                    continue
                try:
                    target = os.stat(path)  # a DirEntry's stat has no file id on Windows
                except OSError as error:
                    self.unavailable[fold(rel)] = str(error)
                    continue
                identity = (target.st_dev, target.st_ino)
                if attributes & FILE_ATTRIBUTE_REPARSE_POINT and identity in identities:
                    self.unavailable[fold(rel)] = 'a link that loops'
                    continue
                stack.append((rel, index, identities + [identity]))
        # A folder is listed before its content (the rules below need that order).
        for item in self.items:
            self.by_key.setdefault(item['key'], []).append(item)

    def classify(self):
        """populateFolder's verdict of each item: 'game', 'excluded' (with a reason), 'empty-name', 'not-configured' or
        'folder'; 'above' is the outermost folder above it that ES-DE does not enter, and why."""
        show_hidden = self.settings['ShowHiddenFiles']
        for item in self.items:
            parent = self.items[item['parent']] if item['parent'] is not None else None
            item['parent_hidden'] = parent['hidden'] if parent else self.hidden_folder
            item['above'] = parent['blocks'] if parent else None
            item['blocks'] = item['above']
            item['configured'] = self.configured(item['name'], item['dir'])
            item['asset'] = self.asset(item['name'], item['dir'])
            item['reason'] = None
            skipped = not show_hidden and item['hidden']
            if item['dir']:
                game = self.game_directory(item['name'])
                if item['above']:
                    item['verdict'], item['reason'] = ('excluded', item['above']) if game else ('folder', None)
                elif skipped:
                    item['verdict'], item['reason'] = ('excluded' if game else 'folder'), 'hidden'
                    item['blocks'] = 'hidden'
                elif game:
                    item['verdict'] = 'game'
                else:
                    item['verdict'] = 'folder'
                    if item['noload']:
                        item['reason'] = 'noload'
                        item['blocks'] = 'noload'
            elif not item['configured']:
                item['verdict'] = 'not-configured'
            elif item['above']:
                item['verdict'], item['reason'] = 'excluded', item['above']
            elif skipped:
                item['verdict'], item['reason'] = 'excluded', 'hidden'
            elif self.esde_name(item['name'], False) == '':
                item['verdict'] = 'empty-name'
            elif item['asset']:
                item['verdict'], item['reason'] = 'excluded', 'bios'
            else:
                item['verdict'] = 'game'

    def read_gamelist(self, home):
        """The <game> and <folder> entries with their relative path, kind and <hidden>; one per kind and path, the last
        counting; entries outside the folder as written."""
        path = os.path.join(home, 'gamelists', self.name, 'gamelist.xml')
        if not os.path.isfile(extended(path)):
            return []
        root = xml_fragment(read_text(path))
        game_list = root.find('gameList')
        if game_list is None:
            return []
        entries = {}
        order = []
        folder_text = self.folder.replace('\\', '/').rstrip('/')
        for node in game_list:
            if node.tag not in ('game', 'folder'):
                continue
            path_node = node.find('path')
            text = path_node.text if path_node is not None else None
            if not text or not text.strip():
                continue
            hidden_node = node.find('hidden')
            hidden = hidden_node is not None and hidden_node.text == 'true'
            relative = self.resolve(text, folder_text)
            if relative is False:
                continue  # malformed: skipped
            ident = (node.tag, relative if relative is not None else text)
            if ident not in entries:
                order.append(ident)
            entries[ident] = {'kind': node.tag, 'rel': relative, 'path': text, 'hidden': hidden}
        return [entries[i] for i in order]

    @staticmethod
    def resolve(text, folder_text):
        """A gamelist path relative to the system folder; None when it leads out of it; False when malformed."""
        t = text.replace('\\', '/')
        if re.match(r'^[A-Za-z]:/', t) or t.startswith('//'):
            prefix = folder_text + '/'
            if fold(t).startswith(fold(prefix)):
                t = t[len(prefix):]
            else:
                return None
        elif t.startswith('/') or re.match(r'^[A-Za-z]:', t):
            return False
        elements = []
        depth_out = False
        for element in t.split('/'):
            if element in ('', '.'):
                continue
            if element == '..':
                if not elements:
                    depth_out = True
                    break
                return False
            if ':' in element or any(ord(c) < 0x20 for c in element):
                return False
            elements.append(element)
        if depth_out:
            return None
        return '/'.join(elements) if elements else False

    # The entries.
    def merge(self, gamelist):
        settings = self.settings
        show_hidden_files = settings['ShowHiddenFiles']
        show_hidden_games = settings['ShowHiddenGames']
        only_gamelist = settings['ParseGamelistOnly']
        block = None
        if not show_hidden_files and self.hidden_folder:
            block = ('excluded-by-setting', 'ShowHiddenFiles')
        elif not only_gamelist and self.noload_root:
            block = ('noload-txt', '')
        by_key = {}
        for entry in gamelist:
            if entry['rel'] is not None:
                by_key.setdefault(fold(entry['rel']), []).append(entry)
        used = set()
        for item in sorted(self.items, key=lambda i: (i['key'], i['rel'])):
            kind = 'folder' if item['verdict'] == 'folder' else 'game'
            same = by_key.get(item['key'], [])
            chosen = None
            for entry in same:
                if entry['kind'] == kind and entry['rel'] == item['rel']:
                    chosen = entry
            if chosen is None:
                of_kind = [e for e in same if e['kind'] == kind]
                chosen = of_kind[-1] if of_kind else None
            for entry in same:
                used.add(id(entry))
            state, reason = 'game', ''
            variant = False
            if kind == 'folder':
                if chosen is None:
                    continue
                state = 'folder'
                if block:
                    state, reason = 'not-a-game', block[0]
                elif not show_hidden_files and (item['hidden'] or item['parent_hidden'] or item['above'] == 'hidden'):
                    state, reason = 'not-a-game', 'excluded-by-setting'
                elif not only_gamelist and 'noload' in (item['above'], item['reason']):
                    state, reason = 'not-a-game', 'noload-txt'
                elif not show_hidden_games and chosen['hidden']:
                    state, reason = 'not-a-game', 'excluded-by-setting'
            elif chosen is not None:
                variant = chosen['rel'] != item['rel']
                if block:
                    state, reason = 'not-a-game', block[0]
                elif not show_hidden_files and (item['hidden'] or item['parent_hidden']):
                    state, reason = 'not-a-game', 'excluded-by-setting'
                elif not only_gamelist and item['above'] == 'noload':
                    state, reason = 'not-a-game', 'noload-txt'
                elif not item['configured']:
                    state, reason = 'not-a-game', 'extension-not-configured'
                elif item['asset']:
                    state, reason = 'not-a-game', 'bios-or-device-set'
                elif not show_hidden_games and chosen['hidden']:
                    state, reason = 'not-a-game', 'excluded-by-setting'
            else:
                if item['verdict'] in ('not-configured', 'empty-name'):
                    continue
                if block and block[1]:
                    state, reason = 'not-a-game', block[0]
                elif only_gamelist:
                    state, reason = 'not-a-game', 'excluded-by-setting'
                elif block:
                    state, reason = 'not-a-game', block[0]
                elif item['verdict'] == 'excluded':
                    state = 'not-a-game'
                    reason = {'hidden': 'excluded-by-setting', 'noload': 'noload-txt',
                              'bios': 'bios-or-device-set'}[item['reason']]
            self.entries.append({'key': item['key'], 'rel': item['rel'], 'state': state, 'reason': reason,
                                 'dir': item['dir'], 'gamelist': chosen is not None, 'variant': variant})
        # The gamelist entries that name nothing listed.
        handled = set()
        for entry in gamelist:
            if id(entry) in used:
                continue
            if entry['rel'] is None:
                used.add(id(entry))
                self.entries.append({'key': None, 'rel': entry['path'], 'state': 'outside-system-folder',
                                     'reason': '', 'dir': False, 'gamelist': True})
                continue
            key = fold(entry['rel'])
            if key in handled:
                continue
            handled.add(key)
            same = by_key[key]
            elements = key.split('/')
            unavailable = inside_game = False
            for depth in range(1, len(elements)):
                prefix = '/'.join(elements[:depth])
                if prefix in self.unavailable:
                    unavailable = True
                    break
                if any(i['dir'] and i['verdict'] != 'folder' for i in self.by_key.get(prefix, [])):
                    inside_game = True
                    break
            if unavailable:
                games = [e for e in same if e['kind'] == 'game']
                chosen = games[-1] if games else same[-1]
                self.add_unavailable(chosen)
                continue
            if inside_game:
                continue
            games = [e for e in same if e['kind'] == 'game']
            if games:
                self.entries.append({'key': key, 'rel': games[-1]['rel'], 'state': 'missing-file', 'reason': '',
                                     'dir': False, 'gamelist': True})

    def add_unavailable(self, entry):
        settings = self.settings
        name = entry['rel'].rsplit('/', 1)[-1]
        parent = entry['rel'].rsplit('/', 1)[0] if '/' in entry['rel'] else ''
        dotted = name.startswith('.') or parent.rsplit('/', 1)[-1].startswith('.')
        state, reason = ('folder' if entry['kind'] == 'folder' else 'game'), ''
        if not settings['ShowHiddenFiles'] and dotted:
            state, reason = 'not-a-game', 'excluded-by-setting'
        elif entry['kind'] == 'game' and not self.configured(name, False):
            state, reason = 'not-a-game', 'extension-not-configured'
        elif entry['kind'] == 'game' and self.asset(name, False):
            state, reason = 'not-a-game', 'bios-or-device-set'
        elif not settings['ShowHiddenGames'] and entry['hidden']:
            state, reason = 'not-a-game', 'excluded-by-setting'
        self.entries.append({'key': fold(entry['rel']), 'rel': entry['rel'], 'state': state, 'reason': reason,
                             'dir': entry['kind'] == 'folder', 'gamelist': True, 'unavailable': True})

    # The disc groups.
    def group_discs(self):
        by_key = {}
        for entry in self.entries:
            if entry['key'] is not None:
                by_key.setdefault(entry['key'], []).append(entry)
        links = {}
        for entry in self.entries:
            if entry['state'] != 'game' or entry['dir'] or entry.get('unavailable'):
                continue
            extension = esde_extension(entry['rel'].rsplit('/', 1)[-1]).lower()
            if extension not in ('.cue', '.m3u'):
                continue
            try:
                text = read_text(os.path.join(self.folder, *entry['rel'].split('/')), MAX_SHEET_BYTES)
            except OSError:
                continue
            references = cue_files(text) if extension == '.cue' else m3u_files(text)
            folder = entry['rel'].split('/')[:-1]
            members = []
            for reference in references:
                relative = sheet_reference(reference, folder)
                if relative is None:
                    continue
                candidates = by_key.get(fold(relative), [])
                exact = [c for c in candidates if c['rel'] == relative]
                for candidate in exact + candidates:
                    if candidate is not entry and candidate['state'] == 'game':
                        if not any(candidate is m for m in members):
                            members.append(candidate)
                        break
            links[entry['key'] + '\0' + entry['rel']] = (entry, members)
        # Depth first in key order; a reference back to a head on the path is cut.
        order = sorted(links)
        head_of = {id(entry): name for name, (entry, _) in links.items()}
        marks = {}
        kept = {name: [] for name in order}

        def visit(name):
            marks[name] = 'path'
            entry, members = links[name]
            for member in members:
                target = head_of.get(id(member))
                if target is not None and marks.get(target) == 'path':
                    continue
                kept[name].append(member)
                if target is not None and target not in marks:
                    visit(target)
            marks[name] = 'done'

        sys.setrecursionlimit(max(10000, sys.getrecursionlimit()))
        for name in order:
            if name not in marks:
                visit(name)
        for name in order:
            if kept[name]:
                self.groups += 1
                for member in kept[name]:
                    member['state'] = 'disc-member'

    def counts(self):
        row = dict.fromkeys(COLUMNS, 0)
        reasons = {}
        for entry in self.entries:
            row['entries'] += 1
            state = entry['state']
            key = {'game': 'games', 'missing-file': 'missing', 'not-a-game': 'notgame', 'disc-member': 'members',
                   'folder': 'folders', 'outside-system-folder': 'outside'}[state]
            row[key] += 1
            if state == 'not-a-game':
                reasons[entry['reason']] = reasons.get(entry['reason'], 0) + 1
        # ES-DE also shows a second entry for a gamelist path that differs from its file in case only.
        variants = sum(1 for e in self.entries if e.get('variant') and e['state'] in ('game', 'disc-member'))
        row['esde'] = row['games'] + row['members'] + variants
        row['groups'] = self.groups
        row['unavail'] = len(self.unavailable)
        return row, reasons


def cue_files(text):
    files = []
    for line in text.splitlines():
        line = line.strip(' \t')
        if len(line) < 4 or line[:4].upper() != 'FILE' or (len(line) > 4 and line[4] not in ' \t'):
            continue
        rest = line[4:].strip(' \t')
        if rest.startswith('"'):
            close = rest.find('"', 1)
            name = rest[1:] if close < 0 else rest[1:close]
        else:
            parts = rest.rsplit(None, 1)
            name = parts[0].strip(' \t') if len(parts) == 2 else rest
        if name:
            files.append(name)
    return files


def m3u_files(text):
    lines = [line.strip(' \t') for line in text.splitlines()]
    return [line for line in lines if line and not line.startswith('#')]


def sheet_reference(reference, folder):
    """A sheet's reference as a path below the system folder, or None (outside, absolute, malformed)."""
    if not reference or any(ord(c) < 0x20 for c in reference) or ':' in reference or reference[0] in '/\\':
        return None
    elements = list(folder)
    for element in re.split(r'[/\\]', reference):
        if element in ('', '.'):
            continue
        if element == '..':
            if not elements:
                return None
            elements.pop()
        else:
            elements.append(element)
    return '/'.join(elements) if elements else None


# --- The census -------------------------------------------------------------------------------------------------------

def census(home, install, override, only):
    settings = read_settings(home)
    root = rom_root(settings, home, install, override)
    definitions = read_systems(home, install, root)
    mame = None
    names = []
    by_name = {}
    for definition in definitions:
        if definition['name'] not in by_name:
            names.append(definition['name'])
        by_name.setdefault(definition['name'], []).append(definition)
    for name in only:
        if name not in by_name:
            sys.exit('esde_census: no system named %s' % name)
    rows = {}
    reasons = {}
    for name in sorted(names):
        if only and name not in only:
            continue
        chosen = None
        for definition in by_name[name]:
            if is_arcade(definition['platforms']) and mame is None:
                mame = read_mame(home, install)
            system = System(definition, settings, mame or ({}, set(), set()))
            system.walk()
            gamelist = system.read_gamelist(home)
            if system.available:
                system.classify()
                system.merge(gamelist)
            else:
                seen = set()
                for entry in gamelist:
                    if entry['rel'] is None:
                        system.entries.append({'key': None, 'rel': entry['path'], 'state': 'outside-system-folder',
                                               'reason': '', 'dir': False, 'gamelist': True})
                        continue
                    key = fold(entry['rel'])
                    if key in seen:
                        continue
                    seen.add(key)
                    same = [e for e in gamelist if e['rel'] is not None and fold(e['rel']) == key]
                    games = [e for e in same if e['kind'] == 'game']
                    system.add_unavailable(games[-1] if games else same[-1])
            system.group_discs()
            row, why = system.counts()
            if chosen is None or (chosen[0]['esde'] == 0 and row['esde'] > 0):
                chosen = (row, why, system.available)
            if row['esde'] > 0:
                break
        row, why, available = chosen
        if row['entries'] or available:
            rows[name] = row
            if why:
                reasons[name] = why
    return root, rows, reasons


def table(rows, reasons):
    lines = ['%-16s' % 'system' + ''.join('%9s' % c for c in COLUMNS)]
    total = dict.fromkeys(COLUMNS, 0)
    for name, row in rows.items():
        lines.append('%-16s' % name + ''.join('%9d' % row[c] for c in COLUMNS))
        for c in COLUMNS:
            total[c] += row[c]
    lines.append('%-16s' % 'total' + ''.join('%9d' % total[c] for c in COLUMNS))
    for name, why in reasons.items():
        for reason, count in sorted(why.items()):
            lines.append('not a game: %s %s %d' % (name, reason, count))
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--esde-home', required=True, help='the ES-DE home (the folder holding settings and gamelists)')
    parser.add_argument('--esde-install', help='the ES-DE install folder (resources/)')
    parser.add_argument('--rom-root', help='replaces ROMDirectory')
    parser.add_argument('--system', action='append', default=[], help='only this system (repeatable)')
    parser.add_argument('--json', action='store_true', help='print one JSON object')
    parser.add_argument('--output', help='write the result to this file instead of stdout')
    args = parser.parse_args()
    started = time.perf_counter()
    root, rows, reasons = census(args.esde_home, args.esde_install, args.rom_root, args.system)
    if args.json:
        text = json.dumps({'romRoot': root, 'systems': rows, 'notAGame': reasons}, indent=2, ensure_ascii=False) + '\n'
    else:
        text = table(rows, reasons)
    if args.output:
        with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.buffer.write(text.encode('utf-8'))
    print('esde_census: %d system(s) in %.1f s' % (len(rows), time.perf_counter() - started), file=sys.stderr)


if __name__ == '__main__':
    main()
