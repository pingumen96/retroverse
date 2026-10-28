# The `video-verdicts` ES-DE fixture

I5 step 7 (ADR-0016 §8): the clips' verdicts. One cartridge system, ES-DE's `snes` with the invented full name
**Lumen 16 Home System** (as `snes-single`), eight invented games, each with a screenshot and a clip of one kind;
Ember Quill is a favourite, so that the room features it first (the screen's start choice when it is the room's only
clip, its still first when no clip plays):

| Game | Clip | Verdict |
|---|---|---|
| Amber Tide | `Amber Tide (USA).mp4` | playable: 3 s, decoded whole |
| Brass Kettle | `Brass Kettle (USA).mp4` | truncated |
| Cinder Bell | `Cinder Bell (USA).mp4` | corrupt-data |
| Dune Lantern | `Dune Lantern (USA).mp4` | invalid-file (zero bytes) |
| Ember Quill | `Ember Quill (USA).avi` | unsupported-codec |
| Frost Gate | `Frost Gate (USA).mp4` | no-video-stream |
| Garnet Loom | `Garnet Loom (USA).mp4` | invalid-file (text) |
| Hazel Orbit | `Hazel Orbit (USA).mp4` | playable: 12 s, sampled |

`rv::test::Fixture::materialize("esde/video-verdicts")` generates what `files.json` lists (clips with
`media::writeTestVideo` and the damaged copies of `tests/support/fixture_payloads.hpp`, pictures with the cover
placeholder): nothing is committed but the text files. `tests/unit/cli/test_cli_media_videos.cpp` checks every clip
with `retroverse-cli media --check-videos`; `tests/smoke/test_video_verdicts.cpp` keeps one clip and plays the room.
Nothing commercial: no real title, company or system name.

```text
<fixture root>                       ES-DE's `~`
├── ES-DE/settings/es_settings.xml   ROMDirectory {{FIXTURE_ROOT}}\roms, MediaDirectory {{FIXTURE_ROOT}}\media
├── ES-DE/gamelists/snes/gamelist.xml
├── ES-DE/custom_systems/            empty
├── Programs/ES-DE/                  as snes-single's: ES-DE.exe placeholder, es_systems.xml, es_find_rules.xml
├── roms/snes/<game> (USA).sfc       64 KiB of payload each
└── media/snes/videos, screenshots   one clip and one screenshot per game
```
