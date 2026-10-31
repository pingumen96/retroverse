#version 450 core
// A panel of the VR mode's UI (I10 step 5, rv::gl::PanelPass): the UI's picture as RmlUi drew it (premultiplied alpha,
// display-encoded), blended over the view's display-ready output with ONE, ONE_MINUS_SRC_ALPHA like the UI over the
// window, faded by uPanelOpacity.
#include "common/common.glsl"

layout(binding = 14) uniform sampler2D uPanel;
uniform float uPanelOpacity;

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

void main() {
    oColor = texture(uPanel, vUv) * uPanelOpacity;
}
