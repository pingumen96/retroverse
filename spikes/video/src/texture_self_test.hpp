#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rv::spike {

/// Outcome of runTextureSelfTest().
struct TextureSelfTestResult {
    bool passed = false;
    /// What did not hold, one line each.
    std::vector<std::string> failures;
    /// VideoTextureStats at the end.
    std::uint64_t uploads = 0;
    std::uint64_t reallocations = 0;
    std::uint64_t ringBusy = 0;
    /// Bursts it took to find the ring busy (each burst issues twice the ring's slots of uploads within one frame).
    int burstsUntilBusy = 0;
    /// GPU timings read back after the uploads completed.
    int gpuTimings = 0;

    [[nodiscard]] nlohmann::json toJson() const;
};

/// Exercises the paths of gl::VideoTexture that a steady video never takes, with exact readbacks of the texture
/// (main thread, with the context current): frames of three sizes (each reallocates the texture; the pixel buffer
/// ring grows for the larger frame and is kept for the smaller one; rows arrive top first and byte for byte, a padded
/// stride included), a burst of uploads within one frame, queued behind a few milliseconds of GPU copies, that finds
/// the next pixel buffer still in use (VideoUpload::RingBusy, returned without waiting), the ring free again once the
/// GPU is done (glFinish), and the GPU timings of the uploads. Pixel values are 0 or 255 only, which sRGB decoding and
/// encoding leave unchanged.
[[nodiscard]] TextureSelfTestResult runTextureSelfTest();

} // namespace rv::spike
