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

`building-overflow-seed-102938-generator-1.4.json` is the building generator 1.4 (`main` before I4b step 7) wrote of
five invented selections (`tests/support`'s `makeSelection`, as the tests make them: amiga 1,700 titles of seed 5200,
mame 60 of 5100, snes 40 of 5300, gb 30 of 5400, pokemini 8 of 5500), the counts of the `esde/overflow` fixture's two
systems and three rooms beside them: `retroverse-cli worldplan generate --selection <each> --library esde --seed 102938
--plan <file>` with `main`'s Release build. Its big-box room and arcade room stand at their template's limit, 139 and 7
titles unplaced for capacity. `tests/unit/world/test_world_expansion.cpp` and
`tests/unit/cli/test_cli_worldplan_building.cpp` update it with the same selections and check generator 1.5's
migration of the capacity entries (ADR-0015 §7, I4b step 7): the two rooms' titles enter their chains as additions
(`room:amiga:2`, `area:arcade:2`), every other room byte for byte in its elements.

Never rewrite them: they stand for every building a user's earlier Retroverse stored.
