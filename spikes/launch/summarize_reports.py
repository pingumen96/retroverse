#!/usr/bin/env python3
"""Summarizes spike_launch reports into the Markdown tables of docs/spikes/i1-launch.md.

    python -I summarize_reports.py <report.json>...

For every report (one session of --repeat launches): a row per launch with the session outcome, the six return times
in milliseconds after the emulator's exit (restored, foreground, fullscreen, first frame, 60 FPS, input), the
remedies the foreground needed, whether Windows' foreground lock was active and the machine's CPU load during the
launch. Then the median of each time per session, and over every launch the median, best and worst.
"""

import json
import ntpath
import statistics
import sys

KEYS = ['restored', 'foreground', 'fullscreen', 'firstFrame', 'steady60', 'input', 'latest']


def other_app_gaps(launch):
    """Milliseconds another application (neither Retroverse nor the target) held the foreground between Retroverse
    and the emulator at the launch, and between the emulator and Retroverse at the return."""
    target = ntpath.basename(launch.get('target', {}).get('executable', '')).lower()
    changes = launch.get('handover', {}).get('foregroundChanges', [])
    owner = ['ours' if c['ours'] else ('target' if c['image'].lower() == target else 'other') for c in changes]
    times = [c['atMsFromLaunch'] for c in changes]
    targets = [i for i, kind in enumerate(owner) if kind == 'target']
    if not targets:
        return None, None
    first, last = targets[0], targets[-1]
    launch_gap = 0.0
    others = [i for i in range(first) if owner[i] == 'other']
    if others:
        launch_gap = times[first] - times[others[0]]
    return_gap = 0.0
    after = [i for i in range(last + 1, len(owner))]
    others = [i for i in after if owner[i] == 'other']
    ours = [i for i in after if owner[i] == 'ours']
    if others and (not ours or others[0] < ours[0]):
        return_gap = (times[ours[0]] if ours else times[others[-1]]) - times[others[0]]
    return launch_gap, return_gap


def fmt(value):
    return '-' if value is None else f'{value:.1f}'


def main(paths):
    sessions = []
    for path in paths:
        with open(path, encoding='utf-8') as file:
            sessions.append((path, json.load(file)))

    print('| Session | Launch | Outcome | Restored | Foreground | Fullscreen | First frame | 60 FPS | Input | '
          'Latest | Remedies | Lock | Other app at launch / return | CPU |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    all_values = {key: [] for key in KEYS}
    gap_values = {'launch gap': [], 'return gap': []}
    session_medians = []
    succeeded = 0
    total = 0
    for number, (path, report) in enumerate(sessions, start=1):
        values = {key: [] for key in KEYS}
        for launch in report.get('launches', []):
            total += 1
            succeeded += 1 if launch.get('success') else 0
            times = launch.get('returnMs', {})
            remedies = ', '.join(a['remedy'] for a in launch.get('handover', {}).get('attempts', [])) or 'none'
            before = launch.get('beforeLaunch', {}).get('remedies', [])
            if before:
                remedies += f" (before the launch: {', '.join(before)})"
            outcome = launch.get('session', {}).get('state', launch.get('error', '?'))
            gaps = other_app_gaps(launch)
            for name, gap in zip(('launch gap', 'return gap'), gaps):
                if gap is not None:
                    gap_values[name].append(gap)
            for key in KEYS:
                if times.get(key) is not None:
                    values[key].append(times[key])
                    all_values[key].append(times[key])
            print(f"| {number} | {launch['index']} | {outcome}{'' if launch.get('success') else ' (criteria missed)'} | "
                  + ' | '.join(fmt(times.get(key)) for key in KEYS)
                  + f" | {remedies} | {'yes' if launch.get('foregroundLockActive') else 'no'} | "
                  f"{fmt(gaps[0])} / {fmt(gaps[1])} | {launch.get('cpuLoad', 0) * 100:.0f}% |")
        session_medians.append({key: statistics.median(v) if v else None for key, v in values.items()})

    print()
    print('| Session | ' + ' | '.join(KEYS) + ' |')
    print('|---|' + '---|' * len(KEYS))
    for number, medians in enumerate(session_medians, start=1):
        print(f'| {number} (median) | ' + ' | '.join(fmt(medians[key]) for key in KEYS) + ' |')

    print()
    print('| Over every launch | ' + ' | '.join(KEYS) + ' |')
    print('|---|' + '---|' * len(KEYS))
    for label, function in (('median', statistics.median), ('best', min), ('worst', max)):
        print(f'| {label} | ' + ' | '.join(fmt(function(all_values[key]) if all_values[key] else None)
                                          for key in KEYS) + ' |')
    print()
    for name, values in gap_values.items():
        if values:
            print(f'Another application in the foreground, {name}: median {statistics.median(values):.1f} ms, '
                  f'max {max(values):.1f} ms, in {sum(1 for v in values if v > 0)} of {len(values)} launches')
    print(f'{succeeded} of {total} launches met the criteria')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
