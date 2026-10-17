#pragma once

// Port ILibrarySource (ADR-0008 §2, §3): reads a library into a snapshot. Implemented for ES-DE by rv_esde
// (EsdeLibrarySource, I2 step 7); the import (step 11) sees only this port.

#include <atomic>
#include <span>

#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// What a load takes besides the profile: what the caller knows from earlier imports, and a way to stop it. The
/// default value is a first load that cannot be cancelled.
struct LibraryLoadOptions {
    /// Read between pieces of work when not null: once it holds true, the load ends with Cancelled.
    const std::atomic<bool>* cancel = nullptr;
    /// The disc groups the library's last import stored, by system then head (ordinal); empty on a first import. A
    /// head whose key, size and last write time are those of its cached group gives that group's references without
    /// being read (DiscGroup::references: its members are picked from them again, among the entries of this load); a
    /// head that cannot be read keeps its cached group's references whatever its size and time, with an issue; an
    /// unreadable head without one groups nothing (its files stay what the listing makes them), with an issue. Cue
    /// sheets on a slow drive are thus read once, a sheet that cannot be read for a while never turns its tracks back
    /// into games, and a track that comes back joins its group without the sheet being read again.
    std::span<const CachedDiscGroup> discGroups;
};

class ILibrarySource {
public:
    virtual ~ILibrarySource() = default;

    /// Reads the library `profile` describes, read-only towards it (ADR-0008 §7: files opened for reading with full
    /// sharing, nothing written under its folders). What goes wrong below the library's roots never fails the load: it
    /// becomes entry states, unavailable systems and folders (SystemInfo, LibrarySnapshot::unavailableFolders), failed
    /// gamelists (GamelistState::Failed) and issues. Errors: the library cannot be read at all (NotFound or Unavailable
    /// for its home or configuration, ParseError for a configuration that cannot be parsed), Cancelled
    /// (LibraryLoadOptions::cancel).
    [[nodiscard]] virtual Expected<LibrarySnapshot> load(const LibraryProfile& profile,
                                                         const LibraryLoadOptions& options) = 0;
};

} // namespace rv::catalog
