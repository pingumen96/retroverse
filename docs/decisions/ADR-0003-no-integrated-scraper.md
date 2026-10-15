# ADR-0003: No integrated scraper; ES-DE stays the scraping tool

- Status: accepted
- Date: 2026-10-15

## Context

Specification §10 plans a scraper inside Retroverse (ScreenScraper as the primary provider, TheGamesDB optional;
requirements FR-021 to FR-023, priority P1) so that missing covers, videos and metadata could be downloaded from the
frontend itself. The ScreenScraper API requires developer credentials that identify the application (spec §10.2).
ES-DE's credentials belong to ES-DE and must not be reused, and a user account alone does not give API access.

The collection Retroverse reads is already scraped by ES-DE. On the reference collection (45 systems, 40,870
`gamelist.xml` entries), matching every entry to `downloaded_media` with ES-DE's naming rule on 2026-10-15 gave:

| Content | Entries covered |
|---|---|
| Screenshot | 98.5% |
| Cover | 90.6% |
| Description and metadata | 90.5% |
| Marquee | 89.3% |
| Video | 82.4% |
| Physical media image | 81.2% |
| Back cover | 77.8% |
| Manual | 56.6% |

The major consoles are covered between 93% and 100%; the gaps are concentrated in a few computer systems (Atari 800:
47% covers, 15% videos; Commodore 64: 53% and 12%). Arcade sets have no physical media by nature.

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
