#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <rv/audio/pcm_buffer.hpp>

// The sounds of the rooms (I12 step 7, ADR-0021 §5): synthesized, never sampled, from deterministic recipes (an
// integer hash for the noise, no rand(), no clock), so the provenance registry gains nothing. GPU-free, rv_core only,
// in rv_audio_model (the mixer's rate, kMixRate, is the default).

namespace rv::audio {

/// Every sound the rooms play: loops on the ambience bus, one-shots on the effects bus.
enum class SoundId : std::uint8_t {
    CrtHum,          ///< loop, 2 s: mains hum (50 Hz and its harmonics) with a slow beat
    PhosphorHiss,    ///< loop, 2 s: a picture tube's hiss, high-passed noise
    NeonBuzz,        ///< loop, 2 s: a neon's 100 Hz buzz with a few crackles
    RoomMurmur,      ///< loop, 4 s: a low murmur, slowly swelling (the arcade, the hub)
    ClockTick,       ///< loop, 2 s: a clock's tick and tock
    FootstepParquet, ///< one-shot, 0.22 s: heel and toe on wood, bright
    FootstepRug,     ///< one-shot, 0.22 s: muffled
    FootstepRunner,  ///< one-shot, 0.22 s: between the two
    ItemClack,       ///< one-shot, 0.12 s: a plastic case leaving or meeting its shelf
    CaseOpen,        ///< one-shot, 0.40 s: a case's latch, its hinge, its lid stopping
    InsertionClick,  ///< one-shot, 0.25 s: a cartridge or disc sliding home
    UiTick,          ///< one-shot, 0.03 s: the menus' soft tick
};

inline constexpr std::array<SoundId, 12> kSoundIds{
    SoundId::CrtHum,    SoundId::PhosphorHiss,    SoundId::NeonBuzz,       SoundId::RoomMurmur,
    SoundId::ClockTick, SoundId::FootstepParquet, SoundId::FootstepRug,    SoundId::FootstepRunner,
    SoundId::ItemClack, SoundId::CaseOpen,        SoundId::InsertionClick, SoundId::UiTick};

/// Every sound's peak: half of full scale, so that a few at once never clip before the gains.
inline constexpr double kSynthPeak = 0.5;

/// "crt-hum", "phosphor-hiss", "neon-buzz", "room-murmur", "clock-tick", "footstep-parquet", "footstep-rug",
/// "footstep-runner", "item-clack", "case-open", "insertion-click", "ui-tick".
[[nodiscard]] std::string_view toString(SoundId sound) noexcept;
/// The loops: the first five.
[[nodiscard]] bool isLoop(SoundId sound) noexcept;
/// Its length (seconds), as documented above.
[[nodiscard]] double soundSeconds(SoundId sound) noexcept;

/// `sound` at `sampleRate`, its variation `seed` (a footstep's noise and its toe's timing, a hum's phases): round(
/// soundSeconds x sampleRate) samples, mono, its peak exactly kSynthPeak, finite; a loop's last sample flows into its
/// first (its tones complete whole cycles, its filters run round it). Pure: the same arguments give the same samples,
/// bit for bit.
[[nodiscard]] PcmBuffer synthesize(SoundId sound, std::uint32_t seed, int sampleRate = 48000);

} // namespace rv::audio
