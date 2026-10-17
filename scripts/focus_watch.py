#!/usr/bin/env python3
"""Watches the Windows desktop for test processes that take the foreground or show windows (read-only).

While a wrapped command runs (or for a fixed duration), follows the foreground window (an EVENT_SYSTEM_FOREGROUND hook,
so that a foreground change shorter than a poll is seen too, and a GetForegroundWindow poll every 20 ms) and polls
EnumWindows for every top-level window that becomes visible: it records the owning process id, its image path, its
parent chain, the window's title, class, rectangle and state, with timestamps. A process is a *test process* when its
image lies under one of the --watch folders, or when an ancestor's does (a console host, a copy started by a test
program, ...); a window is also counted as a test process's when its title names a watched folder, and a terminal
window (Windows Terminal as the default terminal application: it belongs to the terminal's process, outside the test's
tree) when it owns, or appears with, the pseudo console window of a test process.

Findings, which make the exit code 1:
  - a foreground change to a window of a test process;
  - a visible top-level window of a test process on a monitor (minimized windows count: they sit in the taskbar), at
    any time while it stays visible: a window first shown off-screen, cloaked or empty is looked at again on every
    poll, and becomes a finding when it moves onto a monitor, is uncloaked or grows;
  - a system error dialog during the run: a window of WerFault.exe (Windows Error Reporting) whose command line names
    a test process, or whose process cannot be read or names a process never seen, and a dialog (class #32770) of
    csrss.exe (the loader's hard-error box, e.g. a missing DLL), which cannot be attributed to a process at all and is
    counted as the run's (a program of the user's that fails during the run fails it too: the report says why).
Recorded but allowed: windows of test processes that stayed cloaked, off every monitor or of zero size while visible.
Windows shown and hidden again within one poll (20 ms) can be missed; foreground changes cannot.

Process ids are reused: every window's process is identified by its id and its creation time, and the parent chain
takes, for each ancestor, the process of that id that existed when its child was created (exited ones included).

  python -I scripts/focus_watch.py --watch build/msvc-x64-debug --out build/focus/unit.json -- \
      pwsh -NoProfile -File scripts/test.ps1
  python -I scripts/focus_watch.py --watch build --duration 30 --out build/focus/idle.json

Exit codes: 0 nothing found, 1 findings, 2 usage error or the command could not start, 3 nothing found but the
wrapped command failed (its exit code is in the report and on stdout).

It never sends input, never activates, moves or closes a window and never ends a process: it only reads.
Python 3.10 or later, standard library only (ctypes); Windows only.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import ntpath
import os
import subprocess
import sys
import time
from ctypes import wintypes
from dataclasses import dataclass, field

if sys.platform != "win32":  # pragma: no cover - the tool is Windows-only
    print("focus_watch.py runs on Windows only", file=sys.stderr)
    sys.exit(2)

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
try:
    dwmapi = ctypes.WinDLL("dwmapi")
except OSError:  # pragma: no cover - present on every supported Windows
    dwmapi = None

TH32CS_SNAPPROCESS = 0x00000002
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
MONITOR_DEFAULTTONULL = 0
DWMWA_CLOAKED = 14
GWL_EXSTYLE = -20
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
GW_OWNER = 4
EVENT_SYSTEM_FOREGROUND = 0x0003
WINEVENT_OUTOFCONTEXT = 0x0000
WINEVENT_SKIPOWNPROCESS = 0x0002
QS_ALLINPUT = 0x04FF
PM_REMOVE = 0x0001
PROCESS_COMMAND_LINE_INFORMATION = 60  # PROCESSINFOCLASS ProcessCommandLineInformation (Windows 8.1 and later)
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
# The hidden window of a console that a terminal application shows (ConPTY), and the windows of console hosts.
PSEUDO_CONSOLE_CLASS = "PseudoConsoleWindow"
TERMINAL_CLASSES = ("CASCADIA_HOSTING_WINDOW_CLASS", "ConsoleWindowClass")
TERMINAL_WINDOW_SECONDS = 3.0
# Processes the system starts to show error dialogs for other processes, outside their process tree.
ERROR_REPORTING_IMAGES = ("werfault.exe", "werfaultsecure.exe")
HARD_ERROR_IMAGE = "csrss.exe"
DIALOG_CLASS = "#32770"


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


class UNICODE_STRING(ctypes.Structure):
    _fields_ = [("Length", wintypes.USHORT), ("MaximumLength", wintypes.USHORT), ("Buffer", ctypes.c_void_p)]


WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
WINEVENTPROC = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.DWORD, wintypes.HWND, wintypes.LONG, wintypes.LONG,
                                  wintypes.DWORD, wintypes.DWORD)

user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsIconic.argtypes = [wintypes.HWND]
user32.IsWindow.argtypes = [wintypes.HWND]
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetWindow.restype = wintypes.HWND
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.MonitorFromRect.argtypes = [ctypes.POINTER(wintypes.RECT), wintypes.DWORD]
user32.MonitorFromRect.restype = wintypes.HANDLE
user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowLongW.restype = ctypes.c_long
user32.SetWinEventHook.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.HMODULE, WINEVENTPROC, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.DWORD]
user32.SetWinEventHook.restype = wintypes.HANDLE
user32.UnhookWinEvent.argtypes = [wintypes.HANDLE]
user32.MsgWaitForMultipleObjects.argtypes = [wintypes.DWORD, ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD,
                                             wintypes.DWORD]
user32.MsgWaitForMultipleObjects.restype = wintypes.DWORD
user32.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT,
                                wintypes.UINT]
user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenThread.restype = wintypes.HANDLE
kernel32.GetProcessIdOfThread.argtypes = [wintypes.HANDLE]
kernel32.GetProcessIdOfThread.restype = wintypes.DWORD
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                ctypes.POINTER(wintypes.DWORD)]
kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
ntdll.NtQueryInformationProcess.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.ULONG,
                                            ctypes.POINTER(wintypes.ULONG)]
ntdll.NtQueryInformationProcess.restype = ctypes.c_long
if dwmapi is not None:
    dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long


@dataclass
class ProcessInfo:
    pid: int
    parent: int
    name: str
    image: str = ""
    created: int = 0  # FILETIME ticks, 0 when unknown


@dataclass
class Watch:
    folders: list[str]
    # Every process seen, per id, oldest first: an id reused by a later process gets a second entry (the creation time
    # tells them apart), and processes that have exited stay, so that a chain can pass through them.
    history: dict[int, list[ProcessInfo]] = field(default_factory=dict)
    last_snapshot: float = 0.0

    def snapshot(self) -> None:
        """Adds the running processes to the history."""
        handle = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if handle is None or handle == INVALID_HANDLE_VALUE:
            return
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
            ok = kernel32.Process32FirstW(handle, ctypes.byref(entry))
            while ok:
                pid = int(entry.th32ProcessID)
                entries = self.history.get(pid)
                latest = entries[-1] if entries else None
                # A process already known is not opened again (a reused id with the same name and parent is told
                # apart by process(), which compares creation times).
                if latest is None or latest.parent != entry.th32ParentProcessID or latest.name != entry.szExeFile:
                    image, created = query_process(pid)
                    self.record(ProcessInfo(pid, int(entry.th32ParentProcessID), entry.szExeFile, image, created))
                ok = kernel32.Process32NextW(handle, ctypes.byref(entry))
        finally:
            kernel32.CloseHandle(handle)
        self.last_snapshot = time.monotonic()

    def record(self, info: ProcessInfo) -> ProcessInfo:
        entries = self.history.setdefault(info.pid, [])
        if entries and info.created and entries[-1].created == info.created:
            entries[-1] = info  # the same process, seen again
        else:
            entries.append(info)
        return info

    def find(self, pid: int, created: int) -> ProcessInfo | None:
        """The process of `pid` created at `created` (the latest known one when the time is unknown)."""
        entries = self.history.get(pid, [])
        if not created:
            return entries[-1] if entries else None
        return next((entry for entry in reversed(entries) if entry.created == created), None)

    def process(self, pid: int) -> ProcessInfo:
        """The process that has `pid` now, checked against the live process's creation time, never a cached process
        that had the id before."""
        image, created = query_process(pid)
        found = self.find(pid, created)
        if found is None:
            self.snapshot()
            found = self.find(pid, created)
        if found is not None:
            return found
        if created:
            # Alive, but the snapshot took it for the earlier process of its id (same name and parent): the parent id
            # is the snapshot's, which is current.
            latest = self.find(pid, 0)
            parent = latest.parent if latest is not None else 0
            return self.record(ProcessInfo(pid, parent, ntpath.basename(image), image, created))
        return ProcessInfo(pid, 0, "")

    def ancestor(self, pid: int, child_created: int) -> ProcessInfo | None:
        """The process of `pid` that existed when a child created at `child_created` was created."""
        for entry in reversed(self.history.get(pid, [])):
            if not child_created or not entry.created or entry.created <= child_created:
                return entry
        return None

    def chain(self, info: ProcessInfo) -> list[ProcessInfo]:
        """The process and its ancestors, oldest last."""
        result = [info]
        seen = {(info.pid, info.created)}
        current = info
        while current.parent and len(result) < 32:
            parent = self.ancestor(current.parent, current.created)
            if parent is None or (parent.pid, parent.created) in seen:
                break
            result.append(parent)
            seen.add((parent.pid, parent.created))
            current = parent
        return result

    def under_watch(self, path: str) -> bool:
        if not path:
            return False
        normalized = os.path.normcase(os.path.abspath(path))
        return any(normalized.startswith(folder + os.sep) for folder in self.folders)

    def test_ancestry(self, chain: list[ProcessInfo]) -> str:
        """Why the chain is a test process's ('' when it is not)."""
        for depth, info in enumerate(chain):
            if self.under_watch(info.image):
                return ("image under a watched folder" if depth == 0 else
                        f"ancestor {info.name} ({info.pid}) under a watched folder")
        return ""

    def classify(self, info: ProcessInfo, title: str, window_class: str) -> tuple[bool, str, list[ProcessInfo]]:
        """Whether a window of process `info` titled `title` belongs to a test process, and why."""
        chain = self.chain(info)
        reason = self.test_ancestry(chain)
        if reason:
            return True, reason, chain
        lowered = os.path.normcase(title)
        for folder in self.folders:
            if folder in lowered:
                return True, "title names a watched folder", chain
        name = (info.name or ntpath.basename(info.image)).lower()
        if name in ERROR_REPORTING_IMAGES:
            matched, reason = self.classify_error_reporting(info)
            return matched, reason, chain
        if name == HARD_ERROR_IMAGE and window_class == DIALOG_CLASS:
            return True, "system error dialog of csrss.exe during the run (not attributable to a process)", chain
        return False, "", chain

    def classify_error_reporting(self, info: ProcessInfo) -> tuple[bool, str]:
        """A Windows Error Reporting window: the run's when the process it reports on (-p <pid>) is a test process or
        cannot be told."""
        target = error_reporting_target(info.pid)
        if target is None:
            return True, f"{info.name} window during the run (its command line cannot be read)"
        reported = self.find(target, 0)
        if reported is None:
            return True, f"{info.name} window for process {target}, never seen"
        reason = self.test_ancestry(self.chain(reported))
        if reason:
            return True, f"{info.name} window for test process {target} ({reported.name}: {reason})"
        return False, f"{info.name} window for process {target} ({reported.name}), not a test process"


def query_process(pid: int) -> tuple[str, int]:
    """The image path and creation time of a running process ('', 0 when it cannot be opened)."""
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return "", 0
    try:
        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        image = buffer.value if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)) else ""
        times = [wintypes.FILETIME() for _ in range(4)]
        created = 0
        if kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return image, created
    finally:
        kernel32.CloseHandle(handle)


def process_command_line(pid: int) -> str | None:
    """The command line of a running process (None when it cannot be read)."""
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        size = 65536
        buffer = ctypes.create_string_buffer(size)
        returned = wintypes.ULONG(0)
        if ntdll.NtQueryInformationProcess(handle, PROCESS_COMMAND_LINE_INFORMATION, buffer, size,
                                           ctypes.byref(returned)) != 0:
            return None
        text = UNICODE_STRING.from_buffer(buffer)
        if not text.Buffer or text.Length == 0:
            return ""
        return ctypes.wstring_at(text.Buffer, text.Length // 2)
    finally:
        kernel32.CloseHandle(handle)


def error_reporting_target(pid: int) -> int | None:
    """The process a WerFault.exe process reports on: the value of its -p option (None when it cannot be read)."""
    command_line = process_command_line(pid)
    if command_line is None:
        return None
    words = command_line.split()
    for index, word in enumerate(words[:-1]):
        if word.lower() in ("-p", "/p") and words[index + 1].isdigit():
            return int(words[index + 1])
    return None


def thread_pid(thread_id: int) -> int:
    handle = kernel32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, thread_id)
    if not handle:
        return 0
    try:
        return int(kernel32.GetProcessIdOfThread(handle))
    finally:
        kernel32.CloseHandle(handle)


def window_pid(hwnd: int) -> int:
    pid = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return int(pid.value)


def window_text(hwnd: int, getter) -> str:
    buffer = ctypes.create_unicode_buffer(512)
    getter(hwnd, buffer, len(buffer))
    return buffer.value


def window_state(hwnd: int) -> dict:
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    cloaked = wintypes.DWORD(0)
    if dwmapi is not None:
        dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
    iconic = bool(user32.IsIconic(hwnd))
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    on_monitor = bool(user32.MonitorFromRect(ctypes.byref(rect), MONITOR_DEFAULTTONULL))
    ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & 0xFFFFFFFF
    return {
        "rect": [rect.left, rect.top, rect.right, rect.bottom],
        "minimized": iconic,
        "cloaked": bool(cloaked.value),
        "onMonitor": on_monitor,
        "noActivate": bool(ex_style & WS_EX_NOACTIVATE),
        "toolWindow": bool(ex_style & WS_EX_TOOLWINDOW),
        # Where the user would see it: on a monitor (or in the taskbar when minimized), not cloaked, not empty.
        "seen": not cloaked.value and (iconic or (on_monitor and width > 0 and height > 0)),
    }


def visible_windows() -> list[int]:
    found: list[int] = []

    def collect(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            found.append(int(hwnd))
        return True

    user32.EnumWindows(WNDENUMPROC(collect), 0)
    return found


def describe(watch: Watch, hwnd: int, started: float, pid: int = 0) -> dict:
    """A window and its process; `pid` stands in when the window is already gone (a hook's event thread's)."""
    pid = window_pid(hwnd) or pid
    title = window_text(hwnd, user32.GetWindowTextW)
    window_class = window_text(hwnd, user32.GetClassNameW)
    info = watch.process(pid)
    matched, reason, chain = watch.classify(info, title, window_class)
    return {
        "t": round(time.monotonic() - started, 3),
        "time": datetime.datetime.now().astimezone().isoformat(timespec="milliseconds"),
        "hwnd": hex(hwnd),
        "pid": pid,
        "image": chain[0].image if chain else "",
        "chain": [{"pid": p.pid, "name": p.name, "image": p.image} for p in chain],
        "title": title,
        "class": window_class,
        "testProcess": matched,
        "reason": reason,
        **window_state(hwnd),
    }


def attribute_terminal_windows(events: list[dict], pseudo_consoles: dict[int, dict],
                               terminal_windows: dict[int, dict]) -> None:
    """Counts as a test process's the terminal window that hosts its console.

    With a default terminal application (Windows Terminal), a console program started with a console of its own gets
    a hidden PseudoConsoleWindow of its own, and its console is shown by a window of the terminal's process (started by
    the system, outside the test's process tree, titled by the terminal). That window owns the pseudo console window
    once the terminal attached to it; before that (or when the console is gone first) a terminal window shown within
    TERMINAL_WINDOW_SECONDS of a test's pseudo console is taken as its own.
    """
    appeared = sorted(window["t"] for window in pseudo_consoles.values())
    for event in events:
        if event["testProcess"]:
            continue
        hwnd = int(event["hwnd"], 16)
        owner_of = terminal_windows.get(hwnd)
        if owner_of is not None:
            event["testProcess"] = True
            event["reason"] = f"terminal window of the console of test process {owner_of['pid']} ({owner_of['image']})"
            continue
        if event["class"] in TERMINAL_CLASSES and any(abs(event["t"] - t) <= TERMINAL_WINDOW_SECONDS for t in appeared):
            event["testProcess"] = True
            event["reason"] = "terminal window shown with the console of a test process (by timing)"


def pump_messages(timeout: float) -> None:
    """Waits up to `timeout` seconds, delivering the hook's events (out-of-context hooks need a message loop)."""
    msg = wintypes.MSG()
    end = time.monotonic() + timeout
    while True:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        remaining = end - time.monotonic()
        if remaining <= 0:
            return
        user32.MsgWaitForMultipleObjects(0, None, False, max(1, int(remaining * 1000)), QS_ALLINPUT)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="Everything after -- is the command to run and watch.")
    parser.add_argument("--watch", action="append", required=True, metavar="DIR",
                        help="a folder whose programs (and their descendants) are test processes; repeatable")
    parser.add_argument("--out", metavar="JSON", help="write the report there (UTF-8)")
    parser.add_argument("--duration", type=float, help="watch for this many seconds (instead of, or after, a command)")
    parser.add_argument("--tail", type=float, default=1.0,
                        help="seconds to keep watching after the command exits (default 1)")
    parser.add_argument("--interval-ms", type=int, default=20, help="poll interval (default 20 ms)")
    argv = sys.argv[1:]
    command: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, command = argv[:split], argv[split + 1:]
    args = parser.parse_args(argv)
    if not command and args.duration is None:
        parser.error("give a command after -- or --duration")

    folders = [os.path.normcase(os.path.abspath(folder)) for folder in args.watch]
    watch = Watch(folders)
    watch.snapshot()
    started = time.monotonic()
    started_at = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    baseline = set(visible_windows())
    shown: set[int] = set(baseline)
    windows: list[dict] = []
    # Windows shown during the run that were not where the user would see them at first sight, looked at again on
    # every poll while they stay visible.
    unseen: dict[int, dict] = {}
    foreground_changes: list[dict] = []
    pseudo_consoles: dict[int, dict] = {}
    terminal_windows: dict[int, dict] = {}
    last_foreground = int(user32.GetForegroundWindow() or 0)

    def note_foreground(hwnd: int, pid: int = 0) -> None:
        nonlocal last_foreground
        if hwnd and hwnd != last_foreground:
            event = describe(watch, hwnd, started, pid)
            foreground_changes.append(event)
        if hwnd:
            last_foreground = hwnd

    def on_foreground(_hook, _event, hwnd, _object, _child, thread, _time) -> None:
        try:
            note_foreground(int(hwnd or 0), thread_pid(thread))
        except Exception as error:  # an exception must not escape into user32
            print(f"focus_watch: foreground event not recorded: {error}", file=sys.stderr)

    hook_callback = WINEVENTPROC(on_foreground)
    hook = user32.SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, None, hook_callback, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)
    if not hook:
        print("focus_watch: no foreground hook; polling only", file=sys.stderr)

    child = None
    if command:
        try:
            child = subprocess.Popen(command)  # inherits this console: no window of its own
        except OSError as error:
            print(f"focus_watch: cannot start {command[0]}: {error}", file=sys.stderr)
            if hook:
                user32.UnhookWinEvent(hook)
            return 2
        print(f"focus_watch: watching {' '.join(command)} (process {child.pid})", flush=True)

    deadline = started + args.duration if args.duration is not None and not command else None
    tail_until = None
    interval = max(args.interval_ms, 5) / 1000.0
    exit_code = None
    try:
        while True:
            now = time.monotonic()
            if now - watch.last_snapshot > 0.25:
                watch.snapshot()
            note_foreground(int(user32.GetForegroundWindow() or 0))
            current = set(visible_windows())
            for hwnd in sorted(current - shown):
                window = describe(watch, hwnd, started)
                windows.append(window)
                if not window["seen"]:
                    unseen[hwnd] = window
                if window["class"] == PSEUDO_CONSOLE_CLASS and window["testProcess"]:
                    pseudo_consoles[hwnd] = window
            # A window that was off-screen, cloaked or empty counts from the moment the user would see it.
            for hwnd in sorted(unseen):
                window = unseen[hwnd]
                if hwnd not in current or not user32.IsWindow(hwnd):
                    del unseen[hwnd]  # hidden or gone: shown again, it is a new window event
                    continue
                state = window_state(hwnd)
                if state["seen"]:
                    del unseen[hwnd]
                    window["firstState"] = {key: window[key] for key in state}
                    window.update(state)
                    window["seenAt"] = round(now - started, 3)
                    title = window_text(hwnd, user32.GetWindowTextW)
                    if title != window["title"]:
                        window["title"] = title
                        if not window["testProcess"]:
                            matched, reason, _ = watch.classify(watch.process(window["pid"]), title,
                                                                window["class"])
                            window["testProcess"], window["reason"] = matched, reason
            shown = current
            # The terminal window that shows a test's pseudo console owns it (once the terminal has attached).
            for hwnd, window in pseudo_consoles.items():
                if user32.IsWindow(hwnd):
                    owner = int(user32.GetWindow(hwnd, GW_OWNER) or 0)
                    if owner:
                        terminal_windows.setdefault(owner, window)
            if child is not None and exit_code is None:
                exit_code = child.poll()
                if exit_code is not None:
                    tail_until = now + max(args.tail, 0.0)
                    if args.duration is not None:
                        tail_until = max(tail_until, started + args.duration)
            if tail_until is not None and now >= tail_until:
                break
            if deadline is not None and now >= deadline:
                break
            pump_messages(interval)
    except KeyboardInterrupt:
        print("focus_watch: interrupted", file=sys.stderr)
    finally:
        if hook:
            user32.UnhookWinEvent(hook)

    attribute_terminal_windows(foreground_changes + windows, pseudo_consoles, terminal_windows)
    found_foreground = [event for event in foreground_changes if event["testProcess"]]
    found_windows = [window for window in windows if window["testProcess"] and window["seen"]]
    allowed_windows = [window for window in windows if window["testProcess"] and not window["seen"]]
    report = {
        "command": command,
        "watch": args.watch,
        "startedAt": started_at,
        "seconds": round(time.monotonic() - started, 3),
        "commandExitCode": exit_code,
        "intervalMs": args.interval_ms,
        "foregroundHook": bool(hook),
        "counts": {
            "foregroundChanges": len(foreground_changes),
            "foregroundChangesToTestProcesses": len(found_foreground),
            "windowsShown": len(windows),
            "visibleWindowsOfTestProcesses": len(found_windows),
            "allowedWindowsOfTestProcesses": len(allowed_windows),
        },
        "findings": {"foreground": found_foreground, "windows": found_windows},
        "allowed": allowed_windows,
        "foregroundChanges": foreground_changes,
        "windowsShown": windows,
    }
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, ensure_ascii=False)
            stream.write("\n")

    counts = report["counts"]
    print(f"focus_watch: {report['seconds']:.1f} s, {counts['foregroundChanges']} foreground change(s) "
          f"({counts['foregroundChangesToTestProcesses']} to test processes), {counts['windowsShown']} window(s) "
          f"shown ({counts['visibleWindowsOfTestProcesses']} visible of test processes, "
          f"{counts['allowedWindowsOfTestProcesses']} hidden or off-screen)"
          + (f"; command exit code {exit_code}" if command else ""))
    for event in found_foreground:
        print(f"  FOREGROUND +{event['t']:.3f}s pid {event['pid']} {event['image'] or '?'} '{event['title']}' "
              f"({event['reason']})")
    for window in found_windows:
        moved = f", seen from +{window['seenAt']:.3f}s" if "seenAt" in window else ""
        print(f"  WINDOW     +{window['t']:.3f}s{moved} pid {window['pid']} {window['image'] or '?'} "
              f"'{window['title']}' [{window['class']}] ({window['reason']})")
    if found_foreground or found_windows:
        return 1
    if command and exit_code not in (0, None):
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
