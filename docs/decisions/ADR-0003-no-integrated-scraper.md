# ADR-0003: No integrated scraper; ES-DE stays the scraping tool

- Status: accepted
- Date: 2026-10-15
- Amended: 2026-10-17, at I2 step 8: the context table's media rows give the coverage that
  `retroverse-cli media --coverage` reproduces, over the population it names; the figures of 2026-10-15 mixed
  populations. The decision does not change.

## Context

Specification §10 plans a scraper inside Retroverse (ScreenScraper as the primary provider, TheGamesDB optional;
requirements FR-021 to FR-023, priority P1) so that missing covers, videos and metadata could be downloaded from the
frontend itself. The ScreenScraper API requires developer credentials that identify the application (spec §10.2).
ES-DE's credentials belong to ES-DE and must not be reused, and a user account alone does not give API access.

The collection Retroverse reads is already scraped by ES-DE. On the reference collection (45 systems, 40,870 `<game>`
entries of `gamelist.xml`, those whose file is gone included; `<folder>` entries left out), matching every entry to
`downloaded_media` with ES-DE 3.4.1's naming rule and extensions (I2 step 8, `retroverse-cli media --coverage`) gives
the media rows below:

| Content | Entries covered |
|---|---|
| Screenshot | 98.75% |
| Cover | 90.90% |
| Description and metadata | 90.8% (37,128 entries with a `<desc>`, counted from the gamelists; no media kind, not part of `media --coverage`) |
| Marquee | 89.23% |
| Video | 82.19% |
| Physical media image | 81.48% |
| Back cover | 77.73% |
| Manual | 56.55% |

The figures first recorded here, on 2026-10-15 (a day before the read-only survey of the reference machine, with no
method recorded beyond "matching every entry to `downloaded_media` with ES-DE's naming rule"), were 98.5, 90.6, 89.3,
82.4, 81.2, 77.8 and 56.6% for the seven media rows and 90.5% for the description: they mixed populations, and no single
population reproduces all seven rows (`docs/i2/step-8.md`, "ADR-0003's table"). The data had not changed in between.
`tests/unit/cli/test_cli_media_real.cpp` checks the seven media rows above to 0.1 point (`kAdr0003`) and the Atari 800
and Commodore 64 figures below in whole percents (`kAdr0003Systems`); a different amendment changes them there.

The major consoles are covered between 93% and 100%; the gaps are concentrated in a few computer systems (Atari 800:
47% covers, 15% videos; Commodore 64: 53% and 12%; over each system's `<game>` entries). Arcade sets have no physical
media by nature.

## Decision

- Retroverse has no integrated scraper and no HTTP client for metadata or media. Out of scope: FR-021, FR-022 and
  FR-023, the `ScrapeJob` and `MediaMatchReview` entities and the `scrape_jobs` table (spec §6), the scraping part of
  the media management screen (spec §10.3), the scraping workers (spec §15.3), the `[scraping]` configuration
  (spec §16.2) and libcurl (spec §3.1).
- ES-DE remains the scraping tool. Retroverse reads `gamelist.xml` and the media directory in place (spec §4), never
  writes to them, and picks up files ES-DE adds without a restart (file watcher, iteration I7).
- Missing media use the fallbacks of style §21 and spec §9.3: a generated label with the title for a missing cover;
  screenshot, then title screen, then cover for a missing video. A media coverage report per system (I7) tells the
  user what to scrape in ES-DE.
- No ScreenScraper developer credentials are requested.

## Consequences

- No credentials, quotas or provider terms to manage, and Retroverse works fully offline (spec §1.1).
- The scraping experience of spec §10.1 is not provided: to fill gaps the user runs ES-DE's scraper, and the new files
  appear in Retroverse.
- In-app scraping in a future version would need Retroverse's own developer credentials and a new ADR.
