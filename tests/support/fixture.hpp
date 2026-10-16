#pragma once

// Fixtures (ADR-0008 §10): small original trees under tests/fixtures/<name>/ (an ES-DE home, ROM and media folders),
// materialized for a test into a fresh folder under the build tree. ROM payloads are generated, never committed, and
// the fixture's own absolute path is written into its text files where they need it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <rv/core/expected.hpp>

namespace rv::test {

/// tests/fixtures in the source tree.
[[nodiscard]] std::filesystem::path fixturesDir();

/// The folder of the running test executable, where the build puts every program and DLL (rv_fake_emulator.exe,
/// retroverse-cli.exe, SDL3.dll, ...): what files.json's "copyOf" copies from.
[[nodiscard]] std::filesystem::path testProgramsDir();

/// A fixture materialized for one test: the tree copied into <build>/test-fixtures/<name>-<process id>-<n>, a fresh
/// folder removed again when the object goes (unless keep() was called). While the tree is copied:
///  - `{{FIXTURE_ROOT}}` in a text file (.xml, .json, .txt, .cfg) becomes the folder's absolute Windows path, written
///    as the file's syntax needs it: escaped for XML in .xml files (&amp;, &lt;, ...) and for a JSON string in .json
///    files (\\), as it is in the others;
///  - files.json at the top of the tree is not copied. It lists what git cannot hold or should not: files generated
///    with a size, a seed, optionally a last write time (UTC, "2001-02-03T04:05:06Z") and the hidden attribute, and
///    empty folders:
///      {"files": [{"path": "roms/snes/Alpha (USA).sfc", "size": 1024, "seed": 7,
///                  "mtime": "2001-02-03T04:05:06Z", "hidden": true}],
///       "directories": ["roms/empty"]}
///    Paths are relative ("/" or "\", no ".."), unknown keys are errors, a file is at most 64 MiB, and a generated file
///    may neither replace a copied one nor be listed twice, names compared as the file system compares them
///    ("ROMS/A.BIN" is "roms/a.bin"). The bytes come from payloadBytes(seed, size). Instead of a size and a seed, a
///    file may name a program of the build to copy: {"path": "emulators/fake.exe", "copyOf": "rv_fake_emulator.exe"}
///    copies that file of testProgramsDir() (a file name only). The copy finds the DLLs it needs (SDL3, fmt) only
///    through PATH: a test that starts it puts testProgramsDir() on the PATH it passes to the program.
/// Everything is written through \\?\ paths, so fixtures may reach past MAX_PATH whatever the system's long path
/// setting (read them back through core::readFileShared, core::walkTree or core::extendedLengthPath).
class Fixture {
public:
    /// Materializes tests/fixtures/<name>/.
    [[nodiscard]] static Expected<Fixture> materialize(std::string_view name);
    /// Materializes the tree at `source`, named `name` (fixtures a test writes, the materializer's own tests).
    [[nodiscard]] static Expected<Fixture> materializeFrom(const std::filesystem::path& source, std::string_view name);

    Fixture(Fixture&& other) noexcept;
    Fixture& operator=(Fixture&&) = delete;
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture();

    /// The materialized folder (absolute).
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }
    /// root() / `relative` (UTF-8, '/' between the elements).
    [[nodiscard]] std::filesystem::path path(std::string_view relative) const;
    /// Makes a variant of the materialized fixture: replaces `text`, which must occur exactly once in the file
    /// `relative`, with `replacement` (an ES-DE setting turned off, say). Errors: NotFound when the file or the text is
    /// missing, InvalidArgument when the text occurs more than once, those of reading and writing the file.
    [[nodiscard]] Status replaceText(std::string_view relative, std::string_view text,
                                     std::string_view replacement) const;
    /// Leaves the folder in place when the object goes, to look at it after a failure.
    void keep() noexcept { m_keep = true; }

private:
    explicit Fixture(std::filesystem::path root) : m_root(std::move(root)) {}

    std::filesystem::path m_root;
    bool m_keep = false;
};

/// The content of a generated file: `size` bytes from a SplitMix64 generator seeded with `seed`, eight bytes per step,
/// little endian. The same seed always gives the same bytes; a shorter payload is a prefix of a longer one.
[[nodiscard]] std::string payloadBytes(std::uint64_t seed, std::size_t size);

} // namespace rv::test
