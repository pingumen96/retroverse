#pragma once

// GPU-free: no OpenGL header is included, so the policy is unit-testable without a context.

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace rv::gl {

/// Severity of an OpenGL debug message (KHR_debug), from least to most severe.
enum class DebugSeverity : std::uint8_t { Notification, Low, Medium, High };

/// "notification", "low", "medium" or "high".
[[nodiscard]] std::string_view toString(DebugSeverity severity) noexcept;

/// Identity of a debug message for de-duplication: the GL source and type enums plus the implementation-defined id.
struct DebugMessageKey {
    std::uint32_t source = 0;
    std::uint32_t type = 0;
    std::uint32_t id = 0;

    friend auto operator<=>(const DebugMessageKey&, const DebugMessageKey&) = default;
};

/// What to do with one incoming message.
enum class DebugMessageAction : std::uint8_t {
    Drop,             ///< notification or ignored id: neither logged nor counted
    Log,              ///< within the repeat limit: log the message
    StartSuppressing, ///< first occurrence past the repeat limit: log one summary line instead of the message
    Suppress,         ///< later repeats: counted, not logged
};

/// Ids of NVIDIA driver messages that are purely informational: buffer and texture memory placement (131169,
/// 131185, 131204) and shader recompilation hints (131218). Only meaningful when the GL vendor is NVIDIA.
inline constexpr std::array<std::uint32_t, 4> kNvidiaInformationalIds{131169u, 131185u, 131204u, 131218u};

/// Policy behind the GL debug callback: drops noise, de-duplicates repeated messages and counts the rest by severity.
/// Not thread-safe; GLDevice enables GL_DEBUG_OUTPUT_SYNCHRONOUS so every message arrives on the GL thread.
class DebugMessageFilter {
public:
    static constexpr std::uint32_t kDefaultRepeatLimit = 3;

    /// Occurrences of one message beyond the repeat limit: the StartSuppressing one plus every Suppress.
    struct SuppressedMessages {
        DebugMessageKey key;
        std::uint64_t count = 0;
    };

    /// `ignoredIds` are dropped whatever their source, type and severity. The first `repeatLimit` occurrences of a key
    /// are logged, later ones are suppressed (a limit of 0 behaves like 1).
    explicit DebugMessageFilter(std::span<const std::uint32_t> ignoredIds = {},
                                std::uint32_t repeatLimit = kDefaultRepeatLimit);

    /// Classifies one message and updates the counters. Every message that is not dropped is counted, including
    /// suppressed repeats.
    [[nodiscard]] DebugMessageAction classify(const DebugMessageKey& key, DebugSeverity severity);

    /// Number of counted messages whose severity is at least `atLeast`.
    [[nodiscard]] std::uint64_t count(DebugSeverity atLeast) const noexcept;

    /// Keys with suppressed repeats, in ascending key order.
    [[nodiscard]] std::vector<SuppressedMessages> suppressed() const;

    [[nodiscard]] std::uint32_t repeatLimit() const noexcept { return m_repeatLimit; }

private:
    std::vector<std::uint32_t> m_ignoredIds; // sorted for binary search
    std::uint32_t m_repeatLimit;
    std::map<DebugMessageKey, std::uint64_t> m_occurrences;
    std::array<std::uint64_t, 4> m_counts{}; // indexed by DebugSeverity
};

} // namespace rv::gl
