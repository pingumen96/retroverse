#pragma once

// Filled by I3 step 5: the derived-texture cache under <user data>/cache/textures (ADR-0011 §7): entries named
// without the source, invalidated by its size and time, found while it is unavailable, kept under 512 MiB by
// trimming the oldest, never an entry in use.
