# The `portable` ES-DE fixture

A portable ES-DE installation (I2 step 2): ES-DE.exe with a `portable.txt` beside it, as ES-DE's portable release
unpacks. ES-DE 3.4.1 reads the first line of `portable.txt`: empty, ES-DE's home folder is the folder of ES-DE.exe;
otherwise the line is appended to that folder (`<exe folder>/<line>`). The ES-DE home is always the `ES-DE` folder of
the home folder, and `~` is the home folder. `rv::test::Fixture::materialize("esde/portable")` copies it and generates
what `files.json` lists; variants of `portable.txt` are written into the materialized copy.

## Layout

```text
<fixture root>
├── ES-DE/                                the install folder (%ESPATH%), and ES-DE's home folder (~) with an empty
│   │                                     portable.txt
│   ├── ES-DE.exe                         a 4 KiB placeholder, never run
│   ├── portable.txt                      empty
│   ├── ES-DE/                            the ES-DE home
│   │   ├── settings/es_settings.xml      ROMDirectory and MediaDirectory empty: ES-DE's defaults
│   │   ├── gamelists/nes/gamelist.xml
│   │   ├── custom_systems/               empty
│   │   └── downloaded_media/nes/covers/  the default media root, <ES-DE home>\downloaded_media
│   ├── resources/systems/windows/        es_systems.xml (nes, megadrive), es_find_rules.xml (PORTABLE-EMULATOR)
│   └── ROMs/nes/                         the default ROM root, <home folder>\ROMs
├── PortableHome/                         the home folder of the variant "..\PortableHome"
│   ├── ES-DE/settings/es_settings.xml    ROMDirectory "%ESPATH%\ROMs" (the install's ROMs), MediaDirectory "~/Media"
│   └── Media/nes/covers/
└── Users/Someone/ES-DE/                  an empty ES-DE home: the default home the tests' fake probe reports
```

Both settings files hold fake scraper credentials (`portable-scraper-user-M5p1`, `portable-scraper-password-T3v8`)
that nothing may show, log or record.

## Expectations

| `portable.txt` | Home folder (`~`) | ES-DE home | ROM root | Media root |
|---|---|---|---|---|
| empty (committed) | `ES-DE` | `ES-DE\ES-DE` | `ES-DE\ROMs` (default) | `ES-DE\ES-DE\downloaded_media` (default) |
| `..\PortableHome` (CRLF, more lines after it; or `../PortableHome` and a Ctrl+Z) | `PortableHome` | `PortableHome\ES-DE` | `ES-DE\ROMs` (`%ESPATH%`) | `PortableHome\Media` (`~`) |
| `Missing Home`, `ES-DE.exe` (a file), a lone byte order mark, `C:\Absolute` (appended to the install folder) | the default (`Users\Someone`), issue `portable-home-invalid` | `Users\Someone\ES-DE` | | |
| any, with ES-DE's `--home` given | that folder | its `ES-DE` folder | | |

Systems: `nes` has a ROM folder, `megadrive` does not.
