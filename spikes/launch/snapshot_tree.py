#!/usr/bin/env python3
"""Read-only snapshots of directory trees, to prove that a launch wrote nothing outside its scratch directory.

    python -I snapshot_tree.py snapshot <out.json> <directory|file>...
    python -I snapshot_tree.py compare <before.json> <after.json>

A snapshot records, for every file under the given roots: size, modification time and, for the files a run could
change without changing their size (configurations, saves, playlists, logs), a SHA-256 of the content. Nothing is
ever written under the roots. `compare` lists added, removed and changed files and exits with 1 when there are any.
Used by docs/spikes/i1-launch.md around the RetroArch runs (its installation folder and the ROM's folder).
"""

import hashlib
import json
import os
import sys

HASHED_SUFFIXES = {'.cfg', '.opt', '.lpl', '.srm', '.sav', '.state', '.rmp', '.cache', '.log', '.json', '.xml',
                   '.ini', '.txt'}
HASH_LIMIT = 64 * 1024 * 1024


def describe(path):
    status = os.stat(path)
    entry = {'size': status.st_size, 'mtime_ns': status.st_mtime_ns}
    suffix = os.path.splitext(path)[1].lower()
    if suffix in HASHED_SUFFIXES and status.st_size <= HASH_LIMIT:
        digest = hashlib.sha256()
        with open(path, 'rb') as file:
            for block in iter(lambda: file.read(1 << 20), b''):
                digest.update(block)
        entry['sha256'] = digest.hexdigest()
    return entry


def snapshot(roots):
    files = {}
    for root in roots:
        if os.path.isfile(root):
            files[os.path.abspath(root)] = describe(root)
            continue
        for directory, _, names in os.walk(root):
            for name in names:
                path = os.path.join(directory, name)
                try:
                    files[os.path.abspath(path)] = describe(path)
                except OSError as error:
                    files[os.path.abspath(path)] = {'error': str(error)}
    return {'roots': [os.path.abspath(root) for root in roots], 'files': files}


def compare(before, after):
    old = before['files']
    new = after['files']
    differences = []
    for path in sorted(set(old) | set(new)):
        if path not in old:
            differences.append(('added', path))
        elif path not in new:
            differences.append(('removed', path))
        elif old[path] != new[path]:
            differences.append(('changed', path))
    return differences


def main(arguments):
    if len(arguments) >= 3 and arguments[0] == 'snapshot':
        result = snapshot(arguments[2:])
        with open(arguments[1], 'w', encoding='utf-8') as out:
            json.dump(result, out, indent=1, sort_keys=True)
        print(f"{len(result['files'])} files recorded in {arguments[1]}")
        return 0
    if len(arguments) == 3 and arguments[0] == 'compare':
        with open(arguments[1], encoding='utf-8') as file:
            before = json.load(file)
        with open(arguments[2], encoding='utf-8') as file:
            after = json.load(file)
        differences = compare(before, after)
        for kind, path in differences:
            print(f'{kind}: {path}')
        print(f"{len(differences)} difference(s) among {len(after['files'])} files")
        return 1 if differences else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
