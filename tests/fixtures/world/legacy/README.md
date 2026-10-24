# Legacy world plans

Plans an earlier generator wrote, kept as they were so that this generator's updates of them stay tested.

`building-esde-seed-102938-generator-1.3.json` is `../golden/building-esde-seed-102938.json` as `main` held it before
I6 step 4 (`git show main:tests/fixtures/world/golden/building-esde-seed-102938.json`, generator 1.3): the building
fixture's systems (amiga, gb, mame, pocketlab, pokemini, psx, snes) with the cases standing on their end
(`disc_case_cd`, `generic_case`). `tests/unit/world/test_world_migration.cpp` updates it with the same selections and
checks the one-time migration of ADR-0014 §2 (amended on 2026-10-24): `room:psx` (`disc_case_cd` to
`disc_case_cd_v2`), `area:shared:computer` and `area:shared:handheld` (`generic_case` to `generic_case_v2`) migrated,
every other room (`room:gb`, `room:snes` and the arcade room, which keeps `generic_case`) byte for byte in its
elements; `tests/unit/cli/test_cli_worldplan_update.cpp` and the GPU smoke case `gpu.world families: a stored building
of generator 1.3 is migrated once` start from it too.

Never rewrite it: it stands for every building a user's earlier Retroverse stored.
