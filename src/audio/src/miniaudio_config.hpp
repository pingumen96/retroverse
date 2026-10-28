#pragma once

// miniaudio as rv_audio builds it (private to rv_audio): the device I/O only, on WASAPI (the product: the shared-mode
// default endpoint) and on miniaudio's null backend (a timer thread, no operating-system audio: the unit tests of the
// device path). Every source including miniaudio.h does it through this header, so that its structures agree.

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_NULL
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#include <miniaudio.h>
