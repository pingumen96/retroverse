#!/usr/bin/env python3
"""Summarizes spike_launch reports into the tables and figures of docs/spikes/i1-launch.md.

    python -I summarize_reports.py <report.json>...

The reports given are one set (sessions of --repeat launches, measure.ps1's <name>-<n>.json). Printed:
- a row per launch: the session outcome, the six return times in milliseconds after the emulator's exit (restored,
  foreground, fullscreen, first frame submitted, 60 FPS, input) and the latest of them, the remedies the foreground
  needed at the return, the time another application held the foreground at the launch and at the return, the CPU load;
- the median of the latest time per session, and over every launch the median, best and worst of each time;
- the figures the text quotes: detection and wake-up latencies, the frame stalls after the return, the warm-up frame
  times, who received the last input, every foreground change by an application other than Retroverse and the target
  (outside activity), and the hook events that the polled foreground contradicts.

Foreground facts come from the polled state where it exists (GetForegroundWindow, once per frame after the return)
and from hook events that were confirmed at delivery (the window really was the foreground window then): a refused
SetForegroundWindow still produces a hook event, and late events can be overtaken.
"""

import json
import ntpath
import statistics
import sys

KEYS = ['restored', 'foreground', 'fullscreen', 'firstFrame', 'steady60', 'input', 'latest']
LABELS = ['Restored', 'Foreground', 'Fullscreen', 'First frame', '60 FPS', 'Input', 'Latest']
STALL_MS = 20.8  # an interval longer than this misses a vblank at 60 Hz


def fmt(value, digits=1):
    return '-' if value is None else f'{value:.{digits}f}'


def span(values, digits=1):
    """'median M (min-max)' of a list, or '-'."""
    if not values:
        return '-'
    return f'median {statistics.median(values):.{digits}f} ({min(values):.{digits}f}-{max(values):.{digits}f})'


def exit_time(launch):
    times = launch.get('timesFromLaunchMs', {})
    return times.get('exited') if times.get('exited') is not None else times.get('noticed')


def owner_kind(change, target_image):
    if change.get('ours'):
        return 'ours'
    if change.get('image', '').lower() == target_image:
        return 'target'
    return 'other'


def foreground_gaps(launch):
    """Milliseconds another application held the foreground (a) between Retroverse and the target at the launch, from
    the hook, and (b) at the return: from the first other application's change after the target's last one to
    Retroverse's first confirmed hook event or first poll in the foreground, whichever came first. Nothing for (a) when
    the target never took the foreground; nothing for (b) when Retroverse's return was never recorded. (Processes are
    told apart by image name: a copy of the target standing for another application counts as the target.)"""
    target = ntpath.basename(launch.get('target', {}).get('executable', '')).lower()
    hook = launch.get('handover', {}).get('foregroundChanges', [])
    polled = launch.get('handover', {}).get('polledChanges', [])
    kinds = [owner_kind(c, target) for c in hook]
    targets = [i for i, kind in enumerate(kinds) if kind == 'target']
    if not targets:
        return None, None
    first, last = targets[0], targets[-1]
    launch_gap = 0.0
    # Only confirmed events: the others never had the foreground (late, or activated in the background).
    others = [i for i in range(first) if kinds[i] == 'other' and hook[i].get('confirmed', True)]
    if others:
        launch_gap = hook[first]['atMsFromLaunch'] - hook[others[0]]['atMsFromLaunch']
    # The return: the first application after the target's last change, then Retroverse's first confirmed one.
    after = hook[last + 1:]
    other = next((c for c in after if owner_kind(c, target) == 'other' and c.get('confirmed', True)), None)
    if other is None:
        return launch_gap, 0.0
    since = other['atMsFromLaunch']
    back = [c['atMsFromLaunch'] for c in after if c.get('ours') and c.get('confirmed', True) and c['atMsFromLaunch'] >= since]
    back += [c['atMsFromLaunch'] for c in polled if c.get('ours') and c['atMsFromLaunch'] >= since]
    return launch_gap, (min(back) - since) if back else None


def polled_losses(launch):
    """Polled foreground changes away from Retroverse after it was first seen in the foreground at the return."""
    polled = launch.get('handover', {}).get('polledChanges', [])
    seen = False
    losses = []
    for change in polled:
        if change.get('ours'):
            seen = True
        elif seen:
            losses.append(change)
    return losses


def contradicted_hook_events(launch):
    """Hook events of other applications after Retroverse's first confirmed return that the polled state never showed
    (late events overtaken, or windows activated in the background)."""
    target = ntpath.basename(launch.get('target', {}).get('executable', '')).lower()
    hook = launch.get('handover', {}).get('foregroundChanges', [])
    polled_images = {c.get('image') for c in polled_losses(launch)}
    back = next((i for i, c in enumerate(hook) if c.get('ours') and c.get('confirmed', True)
                 and i > 0), None)
    if back is None:
        return []
    return [c for c in hook[back + 1:] if owner_kind(c, target) != 'ours' and c.get('image') not in polled_images]


def outside_changes(launch):
    """Foreground changes by applications that are neither Retroverse nor the target, with the phase they fell in."""
    target = ntpath.basename(launch.get('target', {}).get('executable', '')).lower()
    exited = exit_time(launch)
    result = []
    for source, changes in (('hook', launch.get('handover', {}).get('foregroundChanges', [])),
                            ('polled', launch.get('handover', {}).get('polledChanges', []))):
        for change in changes:
            if owner_kind(change, target) != 'other':
                continue
            at = change['atMsFromLaunch']
            relative = at - exited if exited is not None else None
            result.append((source, change.get('image') or '?', at, relative, change.get('confirmed', True)))
    return result


def stalls(launch):
    return [v for v in launch.get('frames', {}).get('firstIntervalsMs', []) if v > STALL_MS]


def main(paths):
    sessions = []
    for path in paths:
        with open(path, encoding='utf-8') as file:
            sessions.append((ntpath.basename(path), json.load(file)))

    builds = sorted({report.get('build', {}).get('sourceRevision', '?') or '?' for _, report in sessions})
    print('Build: ' + '; '.join(builds))
    print()
    print('| Session | Launch | Outcome | ' + ' | '.join(LABELS) + ' | Remedies at the return | '
          'Other app at launch / return | CPU |')
    print('|---|---|---|' + '---|' * len(LABELS) + '---|---|---|')
    all_values = {key: [] for key in KEYS}
    session_latest = []
    gaps = {'launch': [], 'return': []}
    succeeded = total = 0
    detection, wake, start_ms, resumed = [], [], [], []
    first_stalls, later_stalls, first_steady, later_steady = [], [], [], []
    warmup_p99 = []
    last_input = {'before the spike': 0, 'during the spike': 0}
    fake_taps = 0
    outside = []
    contradicted = []
    losses = []
    remedy_counts = {}
    quick_exits = 0
    for number, (name, report) in enumerate(sessions, start=1):
        warmup_p99.append(report.get('startup', {}).get('frameIntervals', {}).get('p99Ms'))
        latest = []
        for position, launch in enumerate(report.get('launches', [])):
            total += 1
            succeeded += 1 if launch.get('success') else 0
            times = launch.get('returnMs', {})
            attempts = launch.get('handover', {}).get('attempts', [])
            remedies = ', '.join(f"{a['remedy']} ({'ok' if a['foregroundAfter'] else 'refused'})"
                                 for a in attempts) or 'none'
            for attempt in attempts:
                key = (attempt['remedy'], attempt['foregroundAfter'])
                remedy_counts[key] = remedy_counts.get(key, 0) + 1
            before = launch.get('beforeLaunch', {}).get('remedies', [])
            if before:
                remedies += f" (before the launch: {', '.join(before)})"
            state = launch.get('session', {}).get('state', launch.get('error', '?'))
            quick_exits += 1 if launch.get('session', {}).get('quickExit') else 0
            launch_gap, return_gap = foreground_gaps(launch)
            if launch_gap is not None:
                gaps['launch'].append(launch_gap)
            if return_gap is not None:
                gaps['return'].append(return_gap)
            for key in KEYS:
                if times.get(key) is not None:
                    all_values[key].append(times[key])
            if times.get('latest') is not None:
                latest.append(times['latest'])
            print(f"| {number} | {launch['index']} | {state}{'' if launch.get('success') else ' (criteria missed)'} | "
                  + ' | '.join(fmt(times.get(key)) for key in KEYS)
                  + f" | {remedies} | {fmt(launch_gap, 0)} / {fmt(return_gap, 0)} | "
                  f"{launch.get('cpuLoad', 0) * 100:.0f}% |")
            t = launch.get('timesFromLaunchMs', {})
            if launch.get('detectionLatencyMs') is not None:
                detection.append(launch['detectionLatencyMs'])
            if t.get('noticed') is not None and t.get('exitObserved') is not None:
                wake.append(t['noticed'] - t['exitObserved'])
            if launch.get('start', {}).get('startMs') is not None:
                start_ms.append(launch['start']['startMs'])
            if t.get('started') is not None:
                resumed.append(t['started'])
            found = stalls(launch)
            steady = times.get('steady60')
            (first_stalls if position == 0 else later_stalls).append(found)
            if steady is not None and times.get('firstFrame') is not None:
                (first_steady if position == 0 else later_steady).append(steady)
            if 'lastInputBeforeSpike' in launch:
                last_input['before the spike' if launch['lastInputBeforeSpike'] else 'during the spike'] += 1
            tap = launch.get('fake', {}).get('inputAtEnd')
            if tap and tap.get('sent') and tap.get('foreground'):
                fake_taps += 1
            for change in outside_changes(launch):
                outside.append((number, launch['index']) + change)
            for change in contradicted_hook_events(launch):
                contradicted.append((number, launch['index'], change.get('image'), change['atMsFromLaunch'] -
                                     (exit_time(launch) or 0)))
            for change in polled_losses(launch):
                losses.append((number, launch['index'], change.get('image'), change['atMsFromLaunch'] -
                               (exit_time(launch) or 0)))
        session_latest.append(statistics.median(latest) if latest else None)

    print()
    print('| | ' + ' | '.join(LABELS) + ' |')
    print('|---|' + '---|' * len(LABELS))
    for label, function in (('Median', statistics.median), ('Best', min), ('Worst', max)):
        print(f'| {label} | ' + ' | '.join(fmt(function(all_values[key]) if all_values[key] else None)
                                           for key in KEYS) + ' |')
    print()
    print('Session medians of the latest time: ' + ', '.join(fmt(value) for value in session_latest))
    print(f'{succeeded} of {total} launches met the criteria; quick exits: {quick_exits}')
    print('Remedies at the return: ' + (', '.join(f'{remedy} {"ok" if ok else "refused"} x{count}'
                                                  for (remedy, ok), count in sorted(remedy_counts.items()))
                                        or 'none'))
    for name, values in gaps.items():
        if values:
            print(f'Another application in the foreground at the {name}: {span(values, 0)} ms, '
                  f'in {sum(1 for v in values if v > 0)} of {len(values)} launches')
    print(f'Exit noticed after the system exit time (detection): {span(detection)} ms')
    print(f'Main loop woken after the exit was observed: {span(wake, 2)} ms')
    print(f'Runner start (CreateProcessW to resume, hand-over included): {span(start_ms)} ms; '
          f'resumed after the launch began: {span(resumed)} ms')
    flat_first = [max(s) for s in first_stalls if s]
    flat_later = [max(s) for s in later_stalls if s]
    print(f'Stalls (intervals > {STALL_MS} ms in the first 12 after the return), first return of a session: '
          + '; '.join('[' + ', '.join(f'{v:.0f}' for v in s) + ']' for s in first_stalls))
    print(f'  later returns: ' + '; '.join('[' + ', '.join(f'{v:.0f}' for v in s) + ']' for s in later_stalls))
    print(f'  largest stall, first returns: {span(flat_first, 0)} ms; later returns: {span(flat_later, 0)} ms '
          f'({sum(1 for s in later_stalls if not s)} of {len(later_stalls)} later returns without any)')
    print(f'  60 FPS back, first returns: {span(first_steady, 0)} ms; later returns: {span(later_steady, 0)} ms')
    print(f'Warm-up p99 frame interval per session: ' + ', '.join(fmt(v, 2) for v in warmup_p99))
    print(f'Last input: before the spike started in {last_input["before the spike"]} launches, during its run in '
          f'{last_input["during the spike"]}; the fake tapped a key in its own foreground window in {fake_taps}')
    print('Outside activity (session.launch, source, image, ms from launch, ms from exit, confirmed):')
    for row in outside:
        print(f'  {row[0]}.{row[1]} {row[2]} {row[3]} {row[4]:.0f} {fmt(row[5], 0)} {row[6]}')
    print('Hook events of other applications after the return that polling never saw (session.launch, image, ms '
          'from exit): ' + ('; '.join(f'{r[0]}.{r[1]} {r[2]} {r[3]:.0f}' for r in contradicted) or 'none'))
    print('Polled foreground lost after the return (session.launch, image, ms from exit): '
          + ('; '.join(f'{r[0]}.{r[1]} {r[2]} {r[3]:.0f}' for r in losses) or 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
