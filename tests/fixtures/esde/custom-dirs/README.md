# The `custom-dirs` ES-DE fixture

ES-DE settings that move the ROM and media roots with `~` and `%ESPATH%` (I2 step 2), and systems whose `<path>` does
not use `%ROMPATH%`. ES-DE 3.4.1 replaces every `~` with its home folder (the parent of the ES-DE home: here the
fixture root) and `%ESPATH%` with the folder of ES-DE.exe; an empty value is its default. The tests make the variants
from the materialized copy with `Fixture::replaceText`.

## Layout

```text
<fixture root>                              the user's home folder: ES-DE's ~
├── ES-DE/settings/es_settings.xml          see below
├── Games/ROM Library/{gb,snes}/            the ROM root, ROMDirectory "~/Games/ROM Library"
├── Homebrew/pico8/                         pico8's <path> "~\Homebrew\pico8"
├── Elsewhere/vectrex/                      vectrex's absolute <path> "{{FIXTURE_ROOT}}\Elsewhere\vectrex"
├── Programs/ES-DE/                         the install folder (%ESPATH%): ES-DE.exe, resources/systems/windows
├── Programs/Media Store/gb/covers/         the media root, MediaDirectory "%ESPATH%\..\Media Store"
├── ROMs/gb/                                ES-DE's default ROM root (the variant with empty values)
└── ES-DE/downloaded_media/gb/covers/       ES-DE's default media root (the variant with empty values)
```

## Settings

`ROMDirectory` `~/Games/ROM Library` (forward slashes and a space), `MediaDirectory` `%ESPATH%\..\Media Store`,
`ShowHiddenFiles` and `AlternativeEmulatorPerGame` false, `UIMode` kid. Not read: the fake scraper credentials
(`custom-dirs-scraper-user-W8k3`, `custom-dirs-scraper-password-R2d7`) and the UI mode's passkey (`UIMode_passkey`
`uuddlrlrba`), which nothing may show, log or record.

## Expectations

| Settings | ROM root | Media root |
|---|---|---|
| as committed | `Games\ROM Library` (ES-DE's text `<home>/Games/ROM Library\`) | `Programs\Media Store` (ES-DE's text `<install>\..\Media Store/`) |
| both values empty | `ROMs` (default) | `ES-DE\downloaded_media` (default) |
| no install folder known | (as committed) | unresolved: `%ESPATH%` (issue `root-unresolved`) |
| ROMDirectory `~/Games/~Old` | every `~` replaced (issue `root-tilde-inside`) | |

Systems and their ROM folders (`SystemInfo::romFolder`): gb `Games\ROM Library\gb`, snes `Games\ROM Library\snes`
(`%ROMPATH%/snes`), pico8 `Homebrew\pico8`, vectrex `Elsewhere\vectrex`, n64 `Games\ROM Library\n64` (missing:
source unavailable, folder missing). With pico8's path made relative (`Homebrew\pico8`), it has no folder (issue
`rom-folder-unresolved`).
