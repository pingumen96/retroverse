#pragma once

// Filled by I3 step 6 (rv_render_uniforms, GPU-free): the collection arrays' sizes (ADR-0011 §4, §6): the two
// classes (fronts 256 x 188, spines 40 x 256; RGBA8 at half the resolution), layers rounded up to 64 and at most
// 2,048 per chunk, layer 0 pending, the upload budget per frame.
