#pragma once

// Watching a launched game from VR (I11 step 14, ADR-0020 §6), GPU-free (rv_app_cli):
//  - HoldCommand: the left controller's menu button held for kHoldSeconds while the game shows stops watching it (the
//    room's picture and its screens come back, the game runs on); the session panel then offers "Torna al gioco" and
//    "Il gioco è terminato" (LaunchFlow::stopWatching()).
//  - sameAudioOutput(): the headset's audio endpoint (the runtime's) against Windows' default, which a game plays on.

#include <string_view>

namespace rv::app {

/// A button held for a time: once per hold. update() each frame with whether the command may fire (`armed`), whether
/// the button is held, and the frame's seconds; it returns true on the frame the hold reaches kHoldSeconds. A hold that
/// began while not armed, or that already fired, counts only once the button was released. Disarming forgets the hold.
class HoldCommand {
public:
    static constexpr float kHoldSeconds = 1.0f;

    bool update(bool armed, bool held, float seconds) noexcept;

    /// The time held so far toward the command (0 while released, disarmed or spent).
    [[nodiscard]] float heldSeconds() const noexcept { return m_held; }

private:
    float m_held = 0.0f;
    /// The current hold cannot fire: it began unarmed, or fired already. Cleared on release.
    bool m_spent = false;
};

/// Whether two audio endpoint ids name the same device: equal ignoring case, or one is a bare "{guid}" that ends the
/// other's "{0.0.0.00000000}.{guid}" (a runtime may give either form). Empty ids never match.
[[nodiscard]] bool sameAudioOutput(std::string_view a, std::string_view b) noexcept;

} // namespace rv::app
