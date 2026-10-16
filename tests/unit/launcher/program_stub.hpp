#pragma once

// A file that passes rv::launcher::validateLaunchRequest's program check without being runnable: a DOS header whose
// e_lfanew points at a PE signature for `machine` (x64 by default). Validation reads headers only.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace rv::test {

inline std::filesystem::path writeProgramStub(const std::filesystem::path& path, std::uint16_t machine = 0x8664) {
    std::filesystem::create_directories(path.parent_path());
    std::array<unsigned char, 0x80> bytes{};
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[0x3C] = 0x40; // e_lfanew
    bytes[0x40] = 'P';
    bytes[0x41] = 'E';
    bytes[0x44] = static_cast<unsigned char>(machine & 0xFF);
    bytes[0x45] = static_cast<unsigned char>(machine >> 8);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path;
}

} // namespace rv::test
