# The `shortcuts` ES-DE fixture

I11 step 13 (ADR-0020 §3): ES-DE's "open the game's file" command, which Retroverse translates natively and never runs
through cmd.exe. One system under ES-DE's name `windows`, its full name invented (**Desk Arcade Shelf**), whose two
commands are ES-DE 3.4.1's `%HIDEWINDOW% %ESCAPESPECIALS% %EMULATOR_OS-SHELL% /C %ROM%`, the second with
`%RUNINBACKGROUND%`. `OS-SHELL` is found as ES-DE finds it, `cmd.exe` on the executable search path.
`rv::test::Fixture::materialize("esde/shortcuts")` generates what `files.json` lists. Nothing commercial: no real title,
company, store or system name.

## Layout

```text
<fixture root>                       ES-DE's `~`
├── ES-DE/settings/es_settings.xml   as snes-single's: ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory {{FIXTURE_ROOT}}\media
├── ES-DE/gamelists/windows/gamelist.xml
├── ES-DE/custom_systems/            empty
├── Programs/ES-DE/                  ES-DE.exe placeholder, es_systems.xml, es_find_rules.xml
├── roms/windows/Lantern Store.url   an Internet Shortcut of an invented store's scheme, lanternstore: (listed by the
│                                    tests' own emulators.json; never activated: an automated run refuses every one)
└── media/                           empty
```

The tests write their own shortcuts (`.lnk`, through `support/shortcut_file.hpp`) into `roms/windows` before `import`,
pointing at the build's fake emulator: a shortcut holds absolute paths, so none is committed.
