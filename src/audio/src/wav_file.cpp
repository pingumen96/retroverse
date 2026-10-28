#include <rv/audio/wav_file.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rv::audio {
namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

void putTag(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

template <class T>
void put(std::vector<std::byte>& out, T value) {
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * index)) & 0xFFu));
    }
}

template <class T>
T get(const std::vector<std::byte>& bytes, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<std::uint64_t>(bytes[at + index]) << (8 * index);
    }
    return static_cast<T>(value);
}

bool tagAt(const std::vector<std::byte>& bytes, std::size_t at, std::string_view tag) {
    return at + 4 <= bytes.size() && std::memcmp(bytes.data() + at, tag.data(), 4) == 0;
}

} // namespace

Status writeWavFile(const std::filesystem::path& path, std::span<const float> interleaved, int sampleRate,
                    int channels) {
    const std::string context = core::pathToUtf8(path);
    if (channels < 1 || channels > 8 || sampleRate < 1 || interleaved.size() % static_cast<std::size_t>(channels) != 0) {
        return makeError(ErrorCode::InvalidArgument, "a WAV file needs 1 to 8 channels, a rate and whole frames",
                         context);
    }
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(interleaved.size()) * sizeof(float);
    if (dataBytes > std::numeric_limits<std::uint32_t>::max() - 64u) {
        return makeError(ErrorCode::InvalidArgument, "more than 4 GiB of samples", context);
    }
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(dataBytes) + 64u);
    putTag(out, "RIFF");
    put<std::uint32_t>(out, static_cast<std::uint32_t>(4 + 8 + 16 + 8 + dataBytes));
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    put<std::uint32_t>(out, 16);
    put<std::uint16_t>(out, kFormatFloat);
    put<std::uint16_t>(out, static_cast<std::uint16_t>(channels));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate));
    put<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate * channels * 4));
    put<std::uint16_t>(out, static_cast<std::uint16_t>(channels * 4));
    put<std::uint16_t>(out, 32);
    putTag(out, "data");
    put<std::uint32_t>(out, static_cast<std::uint32_t>(dataBytes));
    const std::size_t at = out.size();
    out.resize(at + static_cast<std::size_t>(dataBytes));
    if (dataBytes > 0) {
        std::memcpy(out.data() + at, interleaved.data(), static_cast<std::size_t>(dataBytes));
    }
    return core::writeFileAtomic(path, out);
}

Expected<WavData> readWavFile(const std::filesystem::path& path) {
    const std::string context = core::pathToUtf8(path);
    auto read = core::readBinaryFile(path);
    if (!read) {
        return propagate(read.error());
    }
    const std::vector<std::byte>& bytes = *read;
    if (!tagAt(bytes, 0, "RIFF") || !tagAt(bytes, 8, "WAVE")) {
        return makeError(ErrorCode::ParseError, "not a WAV file", context);
    }
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint16_t bits = 0;
    std::optional<std::pair<std::size_t, std::size_t>> data;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const auto size = static_cast<std::size_t>(get<std::uint32_t>(bytes, at + 4));
        if (at + 8 + size > bytes.size()) {
            return makeError(ErrorCode::ParseError, "a chunk runs past the end of the file", context);
        }
        if (tagAt(bytes, at, "fmt ") && size >= 16) {
            format = get<std::uint16_t>(bytes, at + 8);
            channels = get<std::uint16_t>(bytes, at + 10);
            rate = get<std::uint32_t>(bytes, at + 12);
            bits = get<std::uint16_t>(bytes, at + 22);
            if (format == kFormatExtensible && size >= 26) {
                format = get<std::uint16_t>(bytes, at + 32); // the sub-format GUID's first two bytes
            }
        } else if (tagAt(bytes, at, "data")) {
            data = std::make_pair(at + 8, size);
        }
        at += 8 + size + (size & 1u);
    }
    if (!data || channels == 0 || rate == 0) {
        return makeError(ErrorCode::ParseError, "no format or no data chunk", context);
    }
    WavData wav;
    wav.sampleRate = static_cast<int>(rate);
    wav.channels = channels;
    if (format == kFormatFloat && bits == 32) {
        wav.samples.resize(data->second / 4);
        std::memcpy(wav.samples.data(), bytes.data() + data->first, wav.samples.size() * 4);
    } else if (format == kFormatPcm && bits == 16) {
        wav.samples.resize(data->second / 2);
        for (std::size_t index = 0; index < wav.samples.size(); ++index) {
            const auto value = static_cast<std::int16_t>(get<std::uint16_t>(bytes, data->first + index * 2));
            wav.samples[index] = static_cast<float>(value) / 32768.0f;
        }
    } else {
        return makeError(ErrorCode::ParseError, "only 32-bit float and 16-bit PCM WAV files are read", context);
    }
    return wav;
}

} // namespace rv::audio
