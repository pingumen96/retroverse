# The `case-shelves` ES-DE fixture

The shelves of cases and boxes standing on their side (I6 step 4, ADR-0014 §2): enough titles in a jewel-case room
and a big-box room that most of them stand spine-out, their spines upright, beside a cartridge room for contrast.
Everything is generated when the fixture is materialized from the `collections` items of `files.json`
(`tests/support/fixture_collection.hpp`), so file names, gamelists and covers are the same on every machine and every
run; titles and pictures are invented (word lists and the procedural cover with the title), nothing commercial.

| System | Full name (invented) | Family | Titles | Covers |
|---|---|---|---|---|
| `psx` | Orbit Disc System | `disc_case_cd_v2` | 160 | square, one in three portrait |
| `amiga` | Lumen Desk Computer | `big_box_v2` | 120 | portrait 600 x 760 |
| `gb` | Pocket Lumen | `cartridge_handheld` | 40 | 600 x 610 |

`es_settings.xml` and `es_find_rules.xml` are the `building` fixture's, `es_systems.xml` its three systems. Used by
`tests/smoke/test_world_families.cpp`'s side-spine case (the jewel cases' upright spines up close).
