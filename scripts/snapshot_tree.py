#!/usr/bin/env python3
"""Read-only snapshots of folder trees, to prove that a run wrote nothing where it must not (ADR-0008 sections 7, 10).

    python -I scripts/snapshot_tree.py snapshot <out.json> [--hash <folder|file>]... [--listing <folder>]...
    python -I scripts/snapshot_tree.py compare <before.json> <after.json>

A snapshot records every folder and file under each root, and the root itself:
  --hash      roots that hold configuration (an ES-DE home, its install folder): files get their size, modification time
              and, for the kinds a program changes without changing their size (XML, configurations, saves, playlists,
              logs, text), a SHA-256 of their content;
  --listing   big trees (a ROM folder, a media folder: hundreds of thousands of files): what their folder listings say,
              size and modification time, and no file is ever opened.
Folders are recorded with their modification time, so a folder created, removed or renamed shows as well as a file.
Links (junctions and symbolic links: reparse points whose tag is a name surrogate, told from the listing's attributes on
every Python version) are recorded with their target and never entered; what a link leads to is recorded as a linked
root of its own, in the mode of the root it was found in, unless a root covers it already (rv_core's walker follows
links, so a run may read there). Paths are read through their \\\\?\\ form (any length). Nothing is ever written under
the roots. Roots given after <out.json> without an option are --hash roots. Output is UTF-8 whatever the console or the
redirection.

`compare` prints every added, removed and changed entry, root by root (linked roots too), and exits with 1 when there is
any (2 when the two snapshots do not cover the same roots in the same modes or have different formats), 0 otherwise.
"""

import argparse
import hashlib
import json
import os
import stat
import sys

FORMAT = 3
HASHED_SUFFIXES = {'.cfg', '.opt', '.lpl', '.srm', '.sav', '.state', '.rmp', '.cache', '.log', '.json', '.xml',
                   '.ini', '.txt'}
HASH_LIMIT = 64 * 1024 * 1024
# A reparse tag with this bit names another file or folder (IsReparseTagNameSurrogate): junctions, symbolic links.
NAME_SURROGATE = 0x20000000


def extended(path):
    """The \\\\?\\ form of an absolute path on Windows (paths longer than MAX_PATH), the path elsewhere."""
    path = os.path.abspath(path)
    if os.name != 'nt' or path.startswith('\\\\?\\'):
        return path
    if path.startswith('\\\\'):
        return '\\\\?\\UNC\\' + path[2:]
    return '\\\\?\\' + path


def plain(path):
    """A path without its \\\\?\\ prefix, as the roots are recorded."""
    if path.startswith('\\\\?\\UNC\\'):
        return '\\\\' + path[8:]
    if path.startswith('\\\\?\\'):
        return path[4:]
    return path


def digest(path):
    sha = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            sha.update(block)
    return sha.hexdigest()


def link_tag(entry):
    """The reparse tag of a link (a name surrogate: a junction, a symbolic link), else None. Read from the attributes
    the folder listing returns, which every Python version has on Windows (DirEntry.is_junction exists only from 3.12:
    before it, is_dir(follow_symlinks=False) is true for a junction)."""
    if os.name != 'nt':
        return 0 if entry.is_symlink() else None
    status = entry.stat(follow_symlinks=False)
    if not status.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        return None
    return status.st_reparse_tag if status.st_reparse_tag & NAME_SURROGATE else None


def describe_path(path, hashed):
    """The record of one file or folder (links are described by describe_link)."""
    try:
        status = os.stat(path, follow_symlinks=False)
        if stat.S_ISDIR(status.st_mode):
            return {'kind': 'folder', 'mtime_ns': status.st_mtime_ns}
        record = {'kind': 'file', 'size': status.st_size, 'mtime_ns': status.st_mtime_ns}
        if hashed and os.path.splitext(path)[1].lower() in HASHED_SUFFIXES and status.st_size <= HASH_LIMIT:
            record['sha256'] = digest(path)
        return record
    except OSError as error:
        return {'kind': 'error', 'error': str(error)}


def describe_link(path, tag):
    try:
        target = plain(os.readlink(path))
    except OSError as error:
        target = 'unreadable: ' + str(error)
    return {'kind': 'link', 'tag': f'0x{tag:08X}', 'target': target}


def describe_entry(entry, hashed):
    """describe_path() of a listed entry, and the reparse tag when it is a link. The files of a --listing root are
    described from the listing alone (no file opened). Folders, and the files of a --hash root, are queried themselves:
    NTFS updates the copy of a folder's times that its parent's listing holds lazily, so a folder changed shortly
    before a snapshot would show a stale time there, and a difference between two snapshots of an unchanged tree."""
    try:
        tag = link_tag(entry)
        if tag is not None:
            return describe_link(entry.path, tag), tag
        if entry.is_dir(follow_symlinks=False) or hashed:
            return describe_path(entry.path, hashed), None
        status = entry.stat(follow_symlinks=False)
    except OSError as error:
        return {'kind': 'error', 'error': str(error)}, None
    return {'kind': 'file', 'size': status.st_size, 'mtime_ns': status.st_mtime_ns}, None


def snapshot_root(root, hashed):
    """({relative path: record} of a root, '' being the root itself and '/' between the names, [paths of the links
    found])."""
    top = extended(root)
    entries = {'': describe_path(top, hashed)}
    links = []
    if entries['']['kind'] != 'folder':
        return entries, links
    pending = [('', top)]
    while pending:
        relative, folder = pending.pop()
        try:
            with os.scandir(folder) as listing:
                children = list(listing)
        except OSError as error:
            entries[relative]['listing'] = 'error: ' + str(error)
            continue
        for child in children:
            child_relative = child.name if not relative else relative + '/' + child.name
            record, tag = describe_entry(child, hashed)
            entries[child_relative] = record
            if tag is not None:
                links.append(child.path)
            elif record['kind'] == 'folder':
                pending.append((child_relative, child.path))
    return entries, links


def covers(root, path):
    """True when `path` is `root` or lies inside it, as Windows compares paths (case-insensitively)."""
    root = os.path.normcase(plain(root)).rstrip('\\/')
    path = os.path.normcase(plain(path))
    return path == root or path.startswith(root + os.sep)


def snapshot(hash_roots, listing_roots):
    roots = []
    for root, mode in [(root, 'hash') for root in hash_roots] + [(root, 'listing') for root in listing_roots]:
        roots.append({'root': os.path.abspath(root), 'mode': mode})
    covered = [root['root'] for root in roots]
    linked = []
    queue = []  # (link path, mode) of the links met so far
    for root in roots:
        root['entries'], links = snapshot_root(root['root'], root['mode'] == 'hash')
        queue += [(link, root['mode']) for link in links]
    # What links lead to, unless a root covers it already; linked roots may hold links in turn (a loop ends here).
    while queue:
        link, mode = queue.pop(0)
        try:
            target = plain(os.path.realpath(link, strict=True))
        except (OSError, ValueError):
            continue  # a dangling link: its record names the target
        if any(covers(root, target) for root in covered):
            continue
        covered.append(target)
        entries, links = snapshot_root(target, mode == 'hash')
        linked.append({'root': target, 'mode': mode, 'via': plain(link), 'entries': entries})
        queue += [(inner, mode) for inner in links]
    for root in roots + linked:
        files = sum(1 for record in root['entries'].values() if record['kind'] == 'file')
        folders = sum(1 for record in root['entries'].values() if record['kind'] == 'folder')
        via = f', linked from {root["via"]}' if 'via' in root else ''
        print(f'{root["root"]} ({root["mode"]}{via}): {files} files, {folders} folders')
    return {'format': FORMAT, 'roots': roots, 'linked': linked}


def compare_roots(old_roots, new_roots, differences, label):
    """Appends the differences of two lists of roots; True when they cover the same roots in the same modes."""
    old = {(root['root'], root['mode']): root['entries'] for root in old_roots}
    new = {(root['root'], root['mode']): root['entries'] for root in new_roots}
    for key in sorted(set(old) | set(new)):
        root, _mode = key
        if key not in old or key not in new:
            differences.append((label, root, 'only in one snapshot'))
            continue
        for relative in sorted(set(old[key]) | set(new[key])):
            path = os.path.join(root, relative.replace('/', os.sep)) if relative else root
            before = old[key].get(relative)
            after = new[key].get(relative)
            if before is None:
                differences.append(('added', path, after['kind']))
            elif after is None:
                differences.append(('removed', path, before['kind']))
            elif before != after:
                fields = sorted(name for name in set(before) | set(after) if before.get(name) != after.get(name))
                differences.append(('changed', path, ', '.join(fields)))
    return set(old) == set(new)


def compare(before, after):
    """(kind, path, detail) for every difference, and whether the snapshots cover the same roots."""
    differences = []
    same_roots = compare_roots(before['roots'], after['roots'], differences, 'root')
    compare_roots(before.get('linked', []), after.get('linked', []), differences, 'linked root')
    return differences, same_roots


def main(arguments):
    # Paths hold any character: print them as UTF-8 even when the output is redirected, where Windows' Python would use
    # the ANSI code page (and fail on a character outside it). -I ignores PYTHONIOENCODING, so it is set here.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf-8', errors='backslashreplace')
    if sys.version_info < (3, 10):
        print('snapshot_tree.py needs Python 3.10 or later', file=sys.stderr)
        return 2
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    take = commands.add_parser('snapshot', help='record the roots')
    take.add_argument('out', help='the snapshot file (JSON) to write')
    take.add_argument('roots', nargs='*', help='roots recorded as --hash roots')
    take.add_argument('--hash', action='append', default=[], metavar='ROOT',
                      help='a folder or file whose configuration-like files are hashed')
    take.add_argument('--listing', action='append', default=[], metavar='ROOT',
                      help='a folder recorded from its listings only')
    check = commands.add_parser('compare', help='compare two snapshots')
    check.add_argument('before')
    check.add_argument('after')
    options = parser.parse_args(arguments)

    if options.command == 'snapshot':
        hash_roots = options.roots + options.hash
        if not hash_roots and not options.listing:
            parser.error('name at least one root')
        result = snapshot(hash_roots, options.listing)
        with open(options.out, 'w', encoding='utf-8') as out:
            json.dump(result, out, separators=(',', ':'), sort_keys=True)
        entries = sum(len(root['entries']) for root in result['roots'] + result['linked'])
        print(f'{entries} entries recorded in {options.out}')
        return 0

    with open(options.before, encoding='utf-8') as file:
        before = json.load(file)
    with open(options.after, encoding='utf-8') as file:
        after = json.load(file)
    if before.get('format') != after.get('format'):
        print(f'the snapshots have different formats ({before.get("format")} and {after.get("format")}): take both '
              'with the same script')
        return 2
    differences, same_roots = compare(before, after)
    for kind, path, detail in differences:
        print(f'{kind}: {path} ({detail})')
    entries = sum(len(root['entries']) for root in after['roots'] + after.get('linked', []))
    print(f'{len(differences)} difference(s) among {entries} entries')
    if not same_roots:
        return 2
    return 1 if differences else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
