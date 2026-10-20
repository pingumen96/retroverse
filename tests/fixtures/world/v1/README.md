# Frozen schema-1 plans (the lift's oracles)

Copies of I3's golden plans as generator 1.2 wrote them in schema 1 (commit 81800b0, before ADR-0013's schema 2):
`plan-<titles>-seed-<world seed>.json` and `update-*.json`, made from the selections that
`tests/unit/world/world_generation_support.hpp` generates (`makeSelection`, selection seed 20261018; see
`../golden/README.md`).

They are **never rewritten**. They are the inputs of the schema-1 lift (ADR-0013 §5): lifting one and updating it with
generator 1.3 and the same selection must leave the room's id, template and version, bounds, height, growth steps,
seed, media family and theme pack, and every fixture, placement, dressing, light and unplaced element equal to this
file's. Nothing here comes from a real library: the plans hold GameIds and the world's ids only.
