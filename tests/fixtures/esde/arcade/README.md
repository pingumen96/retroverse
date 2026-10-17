# The `arcade` ES-DE fixture

Systems of the arcade and neogeo platforms as the ROM scan (I2 step 7) reads them (ADR-0008 §3): ES-DE skips MAME BIOS
and device sets there (`resources/MAME/mamebioses.xml`, `mamedevices.xml`) and names unscraped games from
`resources/MAME/mamenames.xml`. The lists in `Programs/ES-DE/resources/MAME` are minimal and invented, in ES-DE's own
format (top-level elements, no root element). The layout of the `standard` fixture; sets generated from `files.json`;
default settings, `MAMENameStripExtraInfo` written true.

## The MAME lists

| List | Entries |
|---|---|
| `mamenames.xml` | `zapzone` "Zap Zone (World, set 1)", `zapzonej` "Zap Zone (Japan) [bootleg]", `rocketpig` "Rocket Pig", `empty1` with an empty real name, `dup1` twice ("First Name", then "Second Name &amp; Co.": the last counts), `glimmer` "Glimmer (Neo Geo MVS)" |
| `mamebioses.xml` | `fakebios`, `neobios` |
| `mamedevices.xml` | `fakedev` |

## Systems and sets

| System | Platform | Set | State | Title | What it exercises |
|---|---|---|---|---|---|
| mame | arcade | `zapzone.zip` | game | Zap Zone | the real name without its parenthesized part (MAMENameStripExtraInfo) |
| | | `zapzonej.zip` | game | Zap Zone | a gamelist entry without `<name>`: the MAME name stays; brackets removed too |
| | | `zapzone (alt).zip` | game | Zap Zone | the file's parentheses removed before the lookup |
| | | `rocketpig.zip` | game | Rocket Pig Deluxe | the gamelist's `<name>` wins |
| | | `empty1.zip` | game | empty1 | an empty real name: the set's own name |
| | | `dup1.zip` | game | Second Name & Co. | a name listed twice: the last |
| | | `unknownset.7z` | game | unknownset | a set mamenames.xml does not list |
| | | `fakebios.zip` | not a game: BIOS or device set | Fake BIOS | a BIOS set, with a gamelist entry (kept with its metadata) |
| | | `fakedev.zip` | not a game: BIOS or device set | fakedev | a device set |
| | | `Upper/FAKEBIOS.zip` | game | FAKEBIOS | the lists compare with case: not the BIOS set |
| | | `(clone).zip` | no entry | | its name is empty once the parentheses go: ES-DE skips it (`rom-name-empty`) |
| | | `readme.txt` | no entry | | not an extension of mame |
| | | `./gonegame.zip` | missing file | Gone Game | a gamelist entry whose set is gone |
| neogeo | neogeo | `neobios.zip` | not a game: BIOS or device set | neobios | the neogeo platform skips BIOS sets too |
| | | `glimmer.zip` | game | Glimmer | |
| | | `zapzone.zip` | game | Zap Zone | |
| bonus | arcade, ignore | `fakebios.zip` | game | fakebios | ES-DE's `ignore` platform replaces every other: no BIOS skipping, no MAME names |
| | | `zapzone.zip` | game | zapzone | |

| | mame | neogeo | bonus |
|---|---|---|---|
| Entries | 11 | 3 | 2 |
| Games | 8 | 2 | 2 |
| Not a game: BIOS or device set | 2 | 1 | 0 |
| Missing files | 1 | 0 | 0 |

Issues: `rom-name-empty` (`(clone).zip`).

## Variants the tests make

- `MAMENameStripExtraInfo` false: the real names keep their parts: "Zap Zone (World, set 1)" (also for
  `zapzone (alt).zip`), "Zap Zone (Japan) [bootleg]", "Glimmer (Neo Geo MVS)".
- `mamenames.xml` removed: ES-DE then reads no list at all (`mame-list-missing`): the BIOS and device sets are games
  (mame 10 games, neogeo 3) and the sets keep their own names, parentheses removed ("zapzone" for `zapzone (alt).zip`).
- A `mamebioses.xml` in the ES-DE home's `resources/MAME` listing `zapzone`: ES-DE's resource lookup takes it instead
  of the install folder's, so `zapzone.zip` is a BIOS set (in mame and neogeo) and `fakebios.zip` a game.
