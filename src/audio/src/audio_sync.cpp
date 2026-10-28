#include <rv/audio/audio_sync.hpp>

#include <algorithm>
#include <cmath>

namespace rv::audio {

SyncDecision AudioSync::update(double errorSeconds) noexcept {
    if (!std::isfinite(errorSeconds)) {
        m_correcting = false;
        return SyncDecision{SyncAction::Resync, 1.0};
    }
    const double size = std::abs(errorSeconds);
    if (size > kSyncResyncBeyondSeconds) {
        m_correcting = false;
        return SyncDecision{SyncAction::Resync, 1.0};
    }
    if (m_correcting ? size <= kSyncSettledSeconds : size <= kSyncDeadbandSeconds) {
        m_correcting = false;
        return SyncDecision{SyncAction::None, 1.0};
    }
    m_correcting = true;
    const double strength = std::min(1.0, size / kSyncFullCorrectionSeconds) * kSyncMaxCorrection;
    // Behind (a negative error): read faster; ahead: slower.
    return SyncDecision{SyncAction::Correct, errorSeconds < 0.0 ? 1.0 + strength : 1.0 - strength};
}

} // namespace rv::audio
