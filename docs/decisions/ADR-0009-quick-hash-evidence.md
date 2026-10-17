# ADR-0009: An equal quick hash pairs only with an equal file name or modification time

- Status: accepted
- Date: 2026-10-17
- Supersedes: one sentence of [ADR-0008](ADR-0008-esde-catalog.md) §4 (Reconciliation: "It pairs with a new file whose
  quick hash equals its stored one"); ADR-0008 stays in force otherwise

## Context

ADR-0008 §4 keeps a game's GameId, history, sessions and choices when its file is moved or renamed: an identity whose
file vanished pairs with a new file whose quick hash equals its stored one, and without a stored hash only with a
unique new file of equal size, time and file name. The quick hash (rule 1, `kQuickHashRule`) is SHA-256 of a file's
whole content up to 128 KiB, else of its first and last 64 KiB; it is computed by a progressive pass and is current
while the size and time it was taken at are the identity's last file facts (I2 step 11b, `docs/i2/step-11b.md`).

Step 11b measured the rule on the reference library. Files over 128 KiB that share their first and last 64 KiB are
often different ROMs: among 23,164 hashed identities (gb, gba, nds, snes) the review found 5,140 groups of one quick
hash, and files of different full contents in 13 of 120 sampled groups (gb 6 of 40, gba 2, snes 5: `[a]` dumps, hacks,
translations, debug builds). One gba quick hash covers 16 distinct full contents (checked with a full SHA-256): a
release (Final Fantasy Tactics Advance (USA)), its Virtual Console release and 14 hacks, five of the hacks sharing their
modification second. A pairing on the quick hash alone would merge such files silently: a game's GameId, sessions and
choices would move onto another game, and an alias cannot be undone. ADR-0008 forbids a silent merge.

A rename or a move keeps the file name or the modification time; another hack of the same base usually has neither.

## Decision

Reconciliation trusts an equal quick hash (with an equal size, the hash current for the vanished identity's last file
facts) only together with an equal file name or an equal last write time. Anything else becomes a review row for the
user, never a merge:

- **Ties** within a group of one quick hash are scored by the number of trailing path elements the keys share (0 when
  the file names differ); a pair whose identity's best file and whose file's best identity are each other alone is
  paired when it is confirmed (the same file name, a score of 1 or more, or the same last write time), so the equal
  file name wins, then the longest common path suffix.
- **Identical copies** (what is left with every score equal and as many files as identities) pair in key order only
  when each pair in key order is confirmed the same way.
- A group or pair that is not confirmed goes to review: its files are the review row's candidates, and
  `retroverse-cli identities resolve` pairs or keeps them apart.

The size, time and name rule for identities without a current hash is unchanged.

## Consequences

- Libraries with hacks, translations and alternative dumps get more review rows: a file moved and renamed at once,
  with a new modification time, is reviewed rather than paired, even when it is the same file.
- An equal time alone still confirms: different files of one quick hash that share their modification second (five
  of the hacks of the gba hash above do) can be paired when one vanishes as the other appears. The residual risk is
  accepted, since a rename keeps the time.
- Identical copies follow the same confirmation: copies of other names and times are reviewed, not paired in key
  order.
- The quick hash stays weak evidence on its own. A stronger fingerprint (a full hash of files up to some size, or of a
  middle part) would be a decision of its own; it is not planned.
- `IdentityPairing` (`rv/catalog/reconciliation.hpp`) implements the rule;
  `tests/unit/catalog_db/test_reconciliation.cpp` tests it (another name and another time: a review row, nothing merged;
  a rename keeping the time and a move keeping the name: paired; identical copies of other names and times: reviewed).
