#pragma once

// An original console model made at test time (I11 step 7): the procedural cartridge console's geometry
// (geometry::buildConsoleCartridge) written as a binary glTF, its body a teal of its own with a striped base colour
// picture, its slot marked by "Anchor_CartridgeSlot". Never a real console's shape, name or picture: it stands in for
// the user's content (<user data>/content/consoles/<system>/) in the tests.

#include <filesystem>

#include <rv/core/expected.hpp>

namespace rv::test {

struct FixtureConsoleOptions {
    /// Every side scaled by it (the procedural console is 24 x 7 x 20 cm).
    float scale = 1.0F;
    /// The base colour picture's side, in pixels.
    int imageSide = 256;
    /// With "Anchor_CartridgeSlot" (else the insertion falls back to the top of its bounds).
    bool slotAnchor = true;
};

/// Writes the console to `file` (.glb, parents created). Errors: those of the writer.
[[nodiscard]] Status writeFixtureConsole(const std::filesystem::path& file, const FixtureConsoleOptions& options = {});

} // namespace rv::test
