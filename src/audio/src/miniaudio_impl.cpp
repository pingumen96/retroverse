// miniaudio's implementation (ADR-0016 §9), compiled once. Its header is an external include (/external:W0): its
// warnings are its own, and /W4 /WX still applies to Retroverse's code.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio_config.hpp"
