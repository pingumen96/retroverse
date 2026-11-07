# The `theme` ES-DE fixture

ES-DE's theme folders with invented systems' identities (I11 step 1, ADR-0019 §2). They are what the identity reader
reads (`rv/esde/themes.hpp`, `rv/esde/identity.hpp`). Nothing commercial: no real name, company, logo or picture. The
logos are original shapes, and the PNG is generated (`files.json`). The tests are in
`tests/unit/esde/test_esde_identity.cpp`.

## Layout

```text
<fixture root>
├── ES-DE/themes/                                  <ES-DE home>/themes: the user's themes
│   ├── chosen-fixture-es-de/system/metadata/gb.xml    the theme identity.json chooses: gb's name, type and colour
│   └── active-fixture-es-de/                          the active theme (es_settings.xml's Theme): logos only
│       ├── _inc/systems/logos/snes.svg                passed over: linear-es-de's snes logo comes first
│       ├── _inc/systems/logos/megadrive.png           generated, 256 x 64
│       └── n64/images/logo.svg                        the slate-like layout
└── Programs/ES-DE/themes/linear-es-de/system/     <install>/themes: ES-DE's bundled theme
    ├── metadata/snes.xml        "Lumen 16": every value, an it_IT description over two lines, a de_DE one, five
    │                            colours of which one is no colour
    ├── metadata/megadrive.xml   "Orbit Drive" with whitespace to collapse, a year of two digits, a control character
    ├── metadata/gb.xml          "Pocket Lumen (linear)": shadowed by chosen-fixture-es-de's
    ├── metadata/broken.xml      cut short: not XML
    ├── logos/snes.svg           original
    └── logos/gb.svg             generated, 1,200,000 bytes: past the SVG limit
```

The systems the tests read, by their es_systems.xml `<theme>`:

| System | What it gets |
|---|---|
| `snes` | linear-es-de's metadata and SVG logo |
| `megadrive` | linear-es-de's metadata, three values left out, the active theme's PNG |
| `gb` | the chosen theme's metadata, no logo (the SVG is too large) |
| `n64` | no metadata: ES-DE's full name, the active theme's `<id>/images/logo.svg` |
| `broken` | an unreadable metadata file |
| `../escape` | no theme id |
