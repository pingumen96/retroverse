# I7 step 7: the file watcher

Roadmap row: I7 step 7. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §10, §13, §15, §16. Plan:
[plan.md](plan.md).

## What was built

- **The port** (`core::IChangeWatcher`, `rv/core/change_watcher.hpp`): roots named for the report ("rom",
  "system:<name>", "media", "gamelists", "custom-systems", "settings"; never a path), changes taken on the main thread,
  each root's mode (notify, poll, lost). `tests/support/fake_change_watcher.hpp` for the tests.
- **The Windows adapter** (`platform_win::DirectoryWatcher`): one thread below normal priority, an overlapped
  `ReadDirectoryChangesW` per root with a completion routine (any number of roots shares one alertable wait), folders
  opened with `FILE_LIST_DIRECTORY` alone and full sharing. A root not there, failing, deleted or renamed away is closed
  and its existence looked at every 5 s; back, it is armed again (`RootLost`, `RootBack`). A message-only window
  registers each handle (`DBT_DEVTYP_HANDLE`): `DBT_DEVICEQUERYREMOVE` closes it inside the message, so an eject is
  never blocked; `DBT_DEVICEQUERYREMOVEFAILED` arms it again. A root where notifications are refused is polled every 30 s
  (`WatchRoot::pollTargets`: the system folders' level and the gamelists, never a walk). It calls nothing of SDL but the
  host's wake.
- **What a change means** (`esde::changeScope()`, `ChangeScopeIndex`, `watchRoots()`, `rv/esde/change_scope.hpp`): lexical,
  over the configuration's folders folded once: a ROM folder's name with one of its system's extensions (or none: a
  folder moved in), a media file, a gamelist name a system; ES-DE's configuration, the ROM root, a folder of system
  folders, the media root every system; an emulator's save or state, `Thumbs.db`, a gamelist's temporary file nothing.
- **The policy** (`app::LibraryWatch`, `rv_app_cli`, an injected clock): 2 s quiet, at most 30 s, then one refresh a
  minute while a burst lasts; held during a game, the editor or a ride, the window opening again at the return.
  `app::LibraryWatcher` (`rv_app_catalog`) maps the watcher's changes and asks the catalog.
- **Partial refreshes** (`RefreshRequest`, trigger `watcher`): `CatalogWorker::refresh()` narrows
  `EsdeLibrarySourceOptions::systems` and `ImportOptions::systems` together; never `acceptRemovals`. The worker posts the
  watcher's map after its opening and each refresh (`CatalogResults::watchMap`), and, with the watcher, compares the
  newest `rv_imports` row every 5 s to follow another process's import (`otherImport`: the lists and the building asked
  again).
- **The app**: `--watch-library`, `--no-watch-library` (`watchesLibrary()`), the watcher created over the ES-DE catalog
  (the wizard's once its import made one), followed each frame, while idle and while minimized; the report's `watcher`
  and the watcher's fields of `catalog.refreshes`.

## Decisions and departures

- **"An unmapped change" widens to every system only where it may matter:** a folder that holds system folders (the ROM
  root, a nested layout's folder), the media root, the configuration. A change in no system's folder (a stray file at
  the ROM root) and a file without a system's extension are nothing, or every emulator's save would refresh everything.
- **Extensions are compared ignoring case** (ES-DE compares them with case): a `.Sfc` costs one refresh more, never one
  less. A name without an extension counts (a folder moved in, a game directory).
- **A root missing at the start is watched for:** its arrival is a change (`RootBack`), so a drive plugged in after the
  start refreshes. A root lost and back refreshes every system.
- **A notified root's renaming** is found by asking its handle's final path every 5 s (no disk read), since Windows
  reports a folder's own rename to its parent's watchers only.
- **A removal asked and never answered** (no `QUERYREMOVEFAILED`, no `REMOVECOMPLETE`) arms the root again only after
  60 s with its folder still there, so a slow eject is never blocked by a handle opened again.
- **The fallback poll's media targets** are each system's media folder and its kind folders: two levels, still no walk,
  since a file added inside `covers` changes no system folder's time.
- **Another process's import is followed only with the watcher on**, so that automated runs stay as they were.
- **The refresh of a system the configuration no longer defines** is a refresh of every system (the source refuses a
  system it does not define).
- **The report's times** (`at`) are milliseconds since the Unix epoch, not from the process's creation: the test
  compares them with the moment it copied the file.

## What was checked

The cases are `tests/unit/app/test_library_watch.cpp` (the policy and `LibraryWatcher`), `tests/unit/esde/test_change_scope.cpp`,
`tests/unit/platform_win/test_directory_watcher.cpp` (real notifications under the build tree, the polls, the device
notifications), the `[watcher]` cases of `tests/unit/app/test_library_refresh.cpp` (partial refreshes byte-identical
for the other systems, the guard, another process's import), `test_run_report.cpp`, `test_command_line.cpp` and
`tests/smoke/test_watcher.cpp` (`[watcher]`). Real library: `real-library.ps1 -Filter '[real-library][watcher_real]'`
(`tests/unit/app/test_watcher_real.cpp`).
